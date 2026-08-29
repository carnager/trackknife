# ADR-0018: Bind bounded local playback to a quiesceable PipeWire stream

- Status: accepted
- Date: 2026-08-26
- Owners: Trackknife project

## Context

ADR-0016 separates blocking FFmpeg production from a real-time-safe PCM render
consumer. The first actual output adapter must preserve that boundary, make
source reset/destruction safe, and remain testable on machines that do not have
a running PipeWire server.

The core currently describes a source rate and channel count but carries its
channel layout as text. Silently guessing a wider hardware channel map would be
less safe than exposing a narrow first implementation.

## Decision

- Link PipeWire 0.3.50 or newer and create one simple output stream on a
  dedicated thread loop. A process-lifetime guard pairs PipeWire initialization
  and deinitialization; each adapter owns and destroys its loop and stream.
- Connect the stream inactive with autoconnect, mapped buffers, and the
  real-time process flag. An optional target object uses PipeWire's stable
  target property rather than deprecated numeric target IDs; exclusive mode is
  an explicit configuration flag.
- Negotiate native-endian interleaved float PCM at the exact source rate. Mono
  uses the mono position and stereo uses front-left/front-right. Reject other
  layouts until the format boundary provides a typed channel map.
- The process callback only dequeues a mapped buffer, clamps the requested
  frames to capacity, calls `LocalPlayback::render()`, updates lock-free
  counters, and queues the buffer. Zero-filled starvation is valid timed PCM,
  not an empty buffer. It performs no allocation, blocking, logging, decode, or
  UI notification.
- Activation waits for PipeWire's streaming state. Quiescence deactivates,
  waits for paused, flushes queued data without draining, and confirms that any
  in-flight process callback left the source. Stop, seek, move, and source
  destruction remain forbidden until this succeeds.
- End-of-source drain is separate: after the local core reaches `ended`, flush
  with drain and wait for PipeWire's drained event.
- All non-real-time stream operations obey thread-loop locking; destruction
  stops the loop outside its lock before destroying stream resources.
- Leave latency/buffer presets, device-clock projection, volume, and producer
  scheduling to the coordinator/UI slice instead of hiding provisional policy
  in this adapter.

## Alternatives considered

### Render from a normal main-loop process callback

This avoids the real-time flag but gives the callback less predictable
scheduling and does not prove the intended audio-thread contract.

### Let PipeWire choose an arbitrary PCM format

That moves sample conversion into an implicit path and makes clean-path
behavior harder to reason about. Explicit source-rate float PCM matches the
decoder/playback boundary; later conversion policy remains visible.

### Destroy or seek after requesting deactivation without waiting

An in-flight callback could still dereference the source or ring. Synchronous
quiescence makes the lifetime boundary explicit.

## Consequences

- `LocalPlayback` must outlive its `PipeWireOutput`, and callers have a concrete
  quiesce operation before destructive transport changes.
- The adapter exposes device/source frame counts, callback count, invalid
  buffer count, node ID, state, and backend error for diagnostics.
- PipeWire-less CI can validate configuration and skip the live integration
  portion; a running server exercises real negotiation and callback lifecycle.
- Multichannel playback is reported as unsupported rather than being silently
  reordered.

## Validation

- A silent 800-frame WAV connects to the actual default PipeWire sink,
  activates, consumes exactly 800 source frames, drains, quiesces, and reports
  no invalid buffers. Silence makes the live test inaudible.
- The test is marked skipped with return code 77 when no server/output is
  available, while invalid-name and invalid-timeout checks always run.
- Core transport/concurrency tests remain server-independent and continue to
  prove the SPSC/render behavior beneath the adapter.

## Revisit when

- typed channel layouts enable correct multichannel negotiation;
- measured hardware/graph behavior establishes latency and buffer presets;
- device-clock timing and device hotplug enter the Local audition service.
