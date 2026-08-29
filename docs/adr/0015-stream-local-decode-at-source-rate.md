# ADR-0015: Stream local decode as source-rate floating-point PCM

- Status: accepted
- Date: 2026-08-26
- Owners: Trackknife project

## Context

The FFmpeg adapter needs a stable Qt-free output contract before PipeWire,
ReplayGain, DSP, seeking, and gapless scheduling can be built independently.
Returning a whole decoded file would violate bounded-memory playback, while
converting immediately to an audio device's format would couple decoding to
PipeWire and could hide unintended signal processing.

## Decision

- A move-only decoder owns one FFmpeg demuxer, selected audio decoder, packet,
  frame, and `libswresample` conversion context.
- It yields bounded chunks of interleaved 32-bit floating-point PCM while
  preserving the decoded stream's sample rate, channel count, and channel
  layout. This stage performs sample-format conversion only: no resampling,
  channel mixing, gain, dither, or device negotiation.
- The adapter exposes sample positions and an optional duration in samples.
  Later segment/gapless logic operates in this sample domain rather than on
  rounded display time.
- Container/frame timestamps locate the first decoded frame and seek preroll;
  they do not define adjacency between emitted PCM chunks. After skip and range
  trimming, the adapter publishes one zero-based contiguous logical PCM
  timeline. This is required for valid streams such as Ogg Vorbis whose
  best-effort frame timestamps can begin above zero, overlap, or leave gaps
  while the decoded PCM itself is continuous.
- FFmpeg I/O uses an interrupt callback tied to Trackknife's cancellation token,
  and decode checks cancellation between chunks.
- Start with one FFmpeg codec thread per decoder. The bounded job/playback
  scheduler may raise this only after oversubscription measurements.
- Request FFmpeg's manual skip-sample mode and apply its frame start/end discard
  metadata exactly once in Trackknife's sample domain. Never infer codec delay
  or padding from detected silence.
- Seeking starts at least one second before the logical target, or farther when
  the codec/container reports a larger initial-padding, seek-preroll, or decoder
  delay requirement. Decoder, packet, frame, and conversion state are flushed;
  output before the requested sample is then discarded.
- The initial proven contract covers real PCM WAV, AAC/M4A, LAME MP3, and
  Opus/Ogg fixtures. Every additional container/codec and gapless claim requires
  a real fixture and independent acceptance row.

## Alternatives considered

### Decode a complete source into memory

This is simple for tests but unsuitable for long tracks and prevents bounded
prefetch and prompt cancellation.

### Emit decoder-native planar/packed sample formats

It avoids one conversion but forces every gain, DSP, buffering, and output
consumer to handle FFmpeg's full format matrix. A source-rate float boundary is
smaller and makes later processing explicit.

### Convert directly to the current PipeWire device format

That couples source decode to a changing device and makes it difficult to prove
that the clean path did not resample or mix channels.

## Consequences

- PipeWire and gain/DSP stages can consume a narrow streaming contract without
  FFmpeg types.
- Integer PCM becomes float before output, but its sample rate, channel layout,
  timing, and amplitude are otherwise unchanged at this boundary.
- Resampler delay flushing, segment trim, accurate seek discard, codec padding,
  and gapless handoff remain explicit at this boundary. The current fixtures
  prove decode and segment identity, not yet a timed two-decoder device handoff.

## Validation

- A repository-owned test writes a real 100 ms mono PCM WAV fixture, probes its
  container/codec/rate/layout through FFmpeg, decodes exactly 800 source-rate
  frames, and verifies silence is unchanged.
- The same test uses a filename containing invalid UTF-8 and proves that the
  adapter retains the exact raw path.
- Cancellation is tested both before probe and between decoder open and the
  next chunk.
- A non-silent 4,096-frame PCM fixture proves exact arbitrary forward/backward
  seek and adjacent end-exclusive ranges without duplicate or missing frames.
- Repository-owned 4,800-frame AAC/M4A, LAME MP3, and Opus/Ogg fixtures carry
  independent encoder-delay and trailing-padding metadata. Each decodes to the
  exact logical duration with non-silent first/last windows, and its sought
  segment equals the corresponding complete-decode PCM slice.
- A repository-owned Ogg Vorbis fixture begins its first decoded frame at a
  positive best-effort timestamp. Complete decode and a sought range prove that
  the adapter publishes contiguous logical positions while retaining exact PCM
  slice identity.

## Revisit when

- a measured playback or DSP path needs planar float internally;
- a decoder cannot preserve required gapless/segment semantics through this
  boundary;
- bounded scheduler measurements justify multiple codec threads.
