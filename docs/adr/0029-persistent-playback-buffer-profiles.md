# ADR-0029: Persistent playback-buffer profiles

- Status: accepted
- Date: 2026-08-29
- Owners: Trackknife project
- Complements: ADR-0021 serialized local audition

## Context

Trackbench's decoded-PCM ring already uses duration-based capacity and start
thresholds, converted at the actual source rate. The default was only an
internal value, however. Users could neither choose the responsiveness versus
stall-resistance tradeoff nor inspect whether a changed value was active.

The ring is allocated once and read concurrently by PipeWire's real-time
callback. Resizing or replacing it beneath that consumer would violate the
quiescence contract and could interrupt the current track.

## Decision

- Trackbench exposes three named profiles in the Playback menu:
  **Responsive** (250 ms capacity, 50 ms start threshold), **Balanced**
  (750/100 ms), and **Resilient** (2,000/250 ms). Balanced remains the default.
- These values size Trackknife's source-rate decoded-PCM ring. They do not set
  PipeWire's graph quantum and do not claim an end-to-end device latency.
- An advanced **Custom** dialog accepts exact capacity and start-threshold
  durations. Capacity is bounded to 10–10,000 ms in the UI; threshold is
  positive and no larger than capacity. The Qt-free service rejects any
  capacity above ten seconds so non-UI callers retain the same memory bound.
- The stable profile identifier and exact values are stored as application
  settings. A named profile follows its current application definition;
  Custom pins the exact stored values. Invalid or unknown persisted state
  falls back to Balanced.
- `LocalAuditionService::set_buffer_config` is a bounded worker command. The
  immutable snapshot reports both configured and active buffer values.
- With no active source, a change applies to the next load directly. With an
  active source, its existing ring stays attached until that track finishes.
  Any prepared gapless continuation is cleared, and further continuation
  requests are declined while the values differ, so the next ordinary load
  applies the new ring without mutating real-time state. Gapless scheduling
  resumes after that one boundary.
- The menu selection updates immediately. A transient message and the compact
  audio-button tooltip say when a value applies on the next track. The tooltip
  also reports configured durations, underrun count, and the PipeWire node when
  available; equivalent window properties remain observable for UI budget and
  regression tests.

## Alternatives considered

### Rebuild the current source at its audible position

Rejected. Reopening the decoder and reconnecting PipeWire merely to resize a
ring creates an audible discontinuity, complicates segment and gapless state,
and turns a preference change into blocking playback work.

### Resize the active ring in place

Rejected. The SPSC ring's allocation and indices are part of the real-time
producer/consumer contract; mutation would require synchronization on the
render path or unsafe storage replacement.

### Force a PipeWire quantum for each profile

Rejected for this slice. The session manager and device graph own that
scheduling policy. A decoded-PCM safety buffer is independently useful and
must not be presented as measured hardware latency.

## Consequences

- Buffer policy is understandable and persistent without adding header chrome.
- A mid-track change deliberately costs one gapless boundary, but does not
  restart, seek, or disturb the audible track.
- Exact custom values provide reproducible tuning while the named presets may
  improve as measured evidence accumulates.
- Underrun counts are visible without introducing a permanently occupied
  diagnostics pane.

## Validation

- Unit/integration coverage fixes the preset identifiers and duration values,
  rejects invalid and over-limit custom configurations, and proves an empty
  service publishes its configured value.
- The live audition regression proves the active ring remains unchanged after
  a mid-track profile update and that a following segment load uses the new
  value.
- The offscreen Trackbench regression proves preset restoration, exclusive
  menu selection, settings persistence, diagnostic properties, and tooltip
  presentation.

## Revisit when

- representative devices and source rates provide enough latency/underrun data
  to tune the named values;
- per-device profiles become useful;
- PipeWire graph-latency reporting and hotplug monitoring enter the service.
