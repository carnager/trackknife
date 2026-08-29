# ADR-0021: Serialize local audition on a dedicated worker

- Status: accepted
- Date: 2026-08-27
- Owners: Trackknife project

## Context

ADR-0015 established cancellable FFmpeg decoding, ADR-0016 established the
bounded playback/render core, and ADR-0018 connected its real-time consumer to
PipeWire. The application still needed one owner for blocking source opening,
decoder production, PipeWire transitions, seeking, draining, replacement, and
diagnostics. Putting any of those operations directly in a Qt action would
violate the UI-thread budget, while independently dispatching them could race a
decoder fill against a seek or destroy a source beneath a PipeWire callback.

Buffer sizes expressed only in frames also produce very different latency at
8 kHz and 192 kHz. The application-level policy needs time-based defaults while
the ring and real-time boundary continue to operate in exact source frames.

## Decision

- A Qt-free `LocalAuditionService` owns one serialized worker, one current
  `LocalPlayback`, and one `PipeWireOutput`. Its public API only submits bounded
  commands and reads immutable snapshots.
- Source load/replace, decoder fills, play, pause, stop, seek, PipeWire
  activate/quiesce/drain, error cleanup, and destruction are performed by that
  worker. The PipeWire process callback remains the only concurrent consumer
  and retains the allocation-free contract from ADR-0018.
- A newer load supersedes queued transport work and cancels a blocking or
  decoding source through the existing FFmpeg cancellation token. Repeated
  pending seeks coalesce to the newest target. The command queue is bounded.
- Buffer policy is expressed as a capacity duration and start-threshold
  duration, converted once to source frames with checked arithmetic after the
  decoder reveals its sample rate. Exact frame configuration remains available
  for tests and later advanced settings.
- The service publishes source path, PCM format, playback position/range,
  buffered frames, underrun count, PipeWire node/state/counters, and structured
  failure. Snapshot publication is cadence-limited so diagnostics cannot
  contend continuously with PipeWire's thread loop.
- The Widgets shell presents a separate, transient **Local audition** toolbar
  when a local source is loaded. It has its own play/pause, stop, seek, source,
  output-node, and underrun presentation and does not reuse or mutate MPD
  transport.
- Activating a local item in a Trackknife working list means **Audition
  locally**. Remote items in the same list retain their MPD queue action. This
  is per-item capability resolution, not a mixed playback queue.

## Consequences

- The UI remains responsive while FFmpeg opens files or PipeWire changes
  state, and destructive source reset cannot overlap decoder production.
- A single local source is auditioned at a time. List progression, output-device
  discovery, volume, ReplayGain application, hotplug, and a broader format
  matrix remain later M4 work.
- Closing the application joins and quiesces the local worker; ordinary runtime
  commands never wait for those backend transitions on the UI thread.
- Tests can validate invalid-source failure without PipeWire and exercise the
  complete coordinator against a live server when one is available.

## Validation

- Duration-based buffering at 8 kHz produces the requested frame capacity and
  threshold rather than assuming a 48 kHz source.
- The service test covers asynchronous missing-source failure, PipeWire load,
  scheduled production beyond the initial buffer, pause stability, exact seek,
  resume, stop/reset, diagnostics, and clear. It skips only the live portion
  when no PipeWire server/output is available.
- The native workspace test proves that a local working-list row exposes
  **Audition locally**, opens the separate toolbar, and reports an asynchronous
  decode failure without requiring an MPD connection.

## Revisit when

- measured latency and underrun evidence establishes shipped buffer presets;
- PipeWire device discovery, hotplug, per-device format negotiation, and volume
  enter the service;
- gapless Local-list progression is defined without implying an MPD/local mixed
  queue.
