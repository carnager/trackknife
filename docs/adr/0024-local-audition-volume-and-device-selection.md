# ADR-0024: Local audition volume and device selection

- Status: accepted
- Date: 2026-08-27
- Owners: Trackknife project

## Context

The bound transport (ADR-0022) kept the volume slider disabled under Local
binding and the audition always played to the system-default PipeWire sink.
M4 requires device selection, volume, and underrun reporting for the direct
PipeWire output without violating the no-sample-processing rule: with gain,
DSP, and conversion disabled, Trackknife must not modify decoded samples.

## Decision

- **Volume is PipeWire's, not Trackknife's.** `PipeWireOutput::set_volume`
  applies a clamped linear soft volume through `pw_stream_set_control`
  (`SPA_PROP_volume`); the samples the render callback copies are never
  modified. The applied value is reported in the output snapshot.
- **Sliders are perceptual.** The audition service stores a volume percent
  (0–100, validated at the API boundary) and maps it cubically onto the linear
  stream control — the familiar PulseAudio taper. The percent survives without
  a source and is reapplied whenever a new source or device connects.
- **Device enumeration is one bounded roundtrip.** A Qt-free
  `list_pipewire_output_devices` connects a temporary registry, collects
  `Audio/Sink`/`Audio/Duplex` nodes (`node.name` as the stream target,
  `node.description` as the label), and completes on one core sync with a
  timeout. It is blocking and runs only on the audition worker via an explicit
  `refresh_output_devices` command; the result is snapshot data.
- **Selecting a device reconnects in place.** `set_output_target` updates the
  stream's `target_object` (nullopt = system default) and, when a source is
  loaded, quiesces the old stream and connects a new one around the untouched
  `LocalPlayback` — position, volume, and play/pause state are preserved. All
  of this serializes on the ADR-0021 worker.
- **Failure is sticky.** A failed load now stays `failed` across unrelated
  publishes (volume or device updates) until the next load or clear; before
  this, any later publish silently reset the snapshot to `empty`.
- **UI stays scoped.** Under Local binding the shared volume slider is live
  and writes the audition volume; a compact device button appears beside it
  with a "System default" entry plus the enumerated sinks (current choice
  checked). Under MPD binding the button hides and the slider is server
  volume, unchanged. MPD outputs in the status bar remain MPD-only.

## Alternatives considered

- **Software gain in the producer.** Rejected: it modifies samples, which the
  audition path promises not to do, and duplicates a mixer PipeWire already
  has.
- **Metadata-based stream moving instead of reconnect.** Rejected for now:
  reconnect through the existing quiesce/connect contract reuses proven code
  and keeps the adapter free of session-manager metadata coupling.
- **Continuous registry monitoring for hotplug.** Deferred: the chooser
  refreshes on open, which covers plugging in a device before picking it;
  live hotplug reaction remains an open M4 decision.

## Validation

- The live PipeWire output test covers volume application, clamping, snapshot
  reporting, and non-empty sink enumeration with usable names.
- The live audition service test covers percent validation, retention without
  a source, cubic application on connect, retargeting a paused source in
  place (same position, same state), and returning to the system default.
- The workspace test covers the sticky-failure regression: a failed audition
  stays visibly failed while device/volume publishes occur.

## Revisit when

- Hotplug/default-change reaction is designed (registry monitoring).
- Measured latency presets or per-device buffer tuning arrive.
