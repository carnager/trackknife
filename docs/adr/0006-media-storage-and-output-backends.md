# ADR-0006: Select focused media, storage, loudness, and output backends

- Status: accepted
- Date: 2026-08-23
- Owners: Trackknife project

## Context

Trackknife needs broader format coverage than a small project can implement
safely, while retaining control over metadata preservation, gapless playback,
loudness semantics, library performance, and Linux audio latency. No single
framework has the right behavior for every boundary.

## Decision

Use focused adapters behind Trackknife-owned interfaces:

- **Decode/encode/container:** FFmpeg libraries (`libavformat`, `libavcodec`,
  `libavutil`, and `libswresample`) are the primary backend. Never shell out to
  `ffmpeg` for playback. Optional external converter encoders may be launched
  without a shell after capability checks.
- **Metadata:** TagLib is the primary read/write backend, supplemented by
  format-specific code whenever its generic property API cannot preserve the
  native structure. A capability matrix and real round-trip fixtures gate every
  writable format.
- **Loudness:** libebur128 provides streaming BS.1770-family measurement. The
  Trackknife layer owns ReplayGain grouping, target/reference policy, result
  provenance, true-peak policy, and parallel scheduling.
- **Library/state:** SQLite is the embedded database, using WAL where the
  filesystem supports it, short transactions, migrations, indexed relational
  metadata values, and prepared statements. Decode and filesystem work never
  occurs inside a database transaction.
- **Audio output:** direct PipeWire is primary. Keep an output interface narrow
  enough for an ALSA fallback if real deployments require it; PulseAudio is
  expected to work through PipeWire compatibility and is not a separate first
  backend.

All five are implementation backends, not domain models. Backend-native handles
and types stay inside adapter targets.

## Consequences

- “FFmpeg decodes it” means it can be analyzed and may be playable; it does not
  imply safe tag writing, exact seeking, verified gaplessness, or advertised
  full support.
- Track-level parallelism must account for FFmpeg's internal codec threads to
  prevent oversubscription.
- TagLib writes remain disabled per format until unknown-data and artwork
  preservation tests pass.
- ReplayGain results can always live in Trackknife's database/sidecar even when
  a container has no safe embedded mapping.
- PipeWire code must keep its real-time callback free of allocation, blocking
  I/O, SQL, locks shared with UI work, and decoder work.

## Validation

- Real-file fixtures cover each claimed format and each independent capability.
- Gapless fixtures assert decoded sample boundaries, not just reported duration.
- Loudness test vectors record the algorithm and reference level.
- Database stress tests cover concurrent reads, bounded writes, migrations, and
  interruption recovery.
- Output tests measure underruns and device-change recovery under background
  decode, scan, and UI load.

## Revisit when

- a specialist decoder, tag implementation, or output backend demonstrably
  improves correctness;
- a selected project becomes unmaintained or license-incompatible;
- PipeWire is unavailable on a material supported deployment.
