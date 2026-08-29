# ADR-0016: Separate bounded local decode production from real-time PCM consumption

- Status: accepted
- Date: 2026-08-26
- Owners: Trackknife project

## Context

The proven FFmpeg decoder can block on file/container/codec work and may
allocate while producing a chunk. PipeWire's process callback must do none of
those things. Transport behavior also needs deterministic tests that do not
depend on a running audio server or a particular hardware quantum.

The PipeWire buffer/latency preset remains an open product decision, so the
core must not bury a provisional device buffer choice in its implementation.

## Decision

- Add a Qt-free `audio` module above `formats`. Its local playback object owns
  the decoder, transport state, source-sample position, and underrun counter.
- A single non-real-time producer calls `fill_buffer()`. Each call performs at
  most one configured ring capacity of decode/copy work.
- A single real-time consumer calls `render()`. It only zero-fills/copies
  preallocated interleaved float PCM and updates atomics; it performs no
  allocation, locking, decoding, file I/O, logging, or callbacks into the UI.
  The build rejects targets where the atomics used by this path are not always
  lock-free.
- Buffer capacity and the playback start threshold are explicit construction
  inputs. PipeWire presets will choose them later from measured latency and
  underrun behavior.
- The state model is `stopped`, `buffering`, `playing`, `paused`, `draining`,
  `ended`, and `failed`. Decode errors enter `failed` and require source reload.
- Buffer starvation while playing emits silence, increments the underrun
  counter, and returns to `buffering`. Silence emitted for starvation does not
  advance the logical source-sample position.
- End-of-source becomes `draining` while queued PCM remains and `ended` only
  after the consumer has taken the final source frame.
- Pause/resume does not consume or discard queued PCM. Stop returns to the
  logical source start. Seeking empties stale PCM and resumes in the equivalent
  stopped, paused, or buffering state at the exact source sample.
- Stop, seek, move, and destruction require the output callback to be quiesced.
  The future PipeWire adapter owns that synchronization; the SPSC ring does not
  place a lock in the real-time callback to compensate for misuse.

## Alternatives considered

### Decode from the PipeWire process callback

This has a smaller apparent API but violates the real-time prohibition as soon
as demuxing, decoding, allocation, cancellation, or filesystem I/O occurs.

### Put a mutex around one shared PCM queue

It makes reset easy but allows priority inversion and blocking in the audio
callback. The queue instead has one producer and one consumer with atomic
indices.

### Pick one hard-coded buffer duration now

That would settle the open PipeWire latency/underrun decision without device
measurements. The core accepts explicit frame counts instead.

## Consequences

- Decoder scheduling and PipeWire lifecycle can evolve independently around a
  narrow, testable fill/render boundary.
- The PipeWire adapter must deactivate/quiesce its process callback before a
  destructive queue reset and reactivate it afterward.
- Source position represents consumed source PCM, not wall-clock time during an
  underrun. Device-clock projection remains adapter/coordinator work.
- One pending decoder chunk can exist outside the ring. Its size remains
  bounded by ADR-0015's streaming decoder chunk contract.

## Validation

- A real non-silent PCM WAV fixture proves stopped/buffering/playing/paused/
  draining/ended transitions, exact pause/resume and seek samples, bounded ring
  wrap, stop/restart, and end-exclusive segment completion.
- A concurrent producer/consumer run crosses the ring boundary repeatedly with
  a deliberately non-power-of-two capacity and reproduces every source sample
  in order.
- Forced starvation proves zero-fill, one underrun report, source-position
  preservation, and recovery after refill.
- Invalid seeks leave the previous snapshot unchanged. Cancellation propagates
  the structured error, enters `failed`, and prevents replay/seek without a
  reload.

## Revisit when

- PipeWire device/quantum measurements establish shipped buffer presets;
- a DSP graph needs a different internal sample layout;
- gapless preloading needs more than one producer/source at the handoff layer.
