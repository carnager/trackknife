# ADR-0148: Opt-in true peak as the ReplayGain peak

Date: 2026-09-11

Status: accepted

## Context

Every ReplayGain scan already measures true peak: the analyzer enables
libebur128's oversampled true-peak mode unconditionally, the per-track
result carries `true_peak` beside `sample_peak`, and the album
aggregation keeps the programme-wide maximum of both. But the measured
true peak is then discarded — `REPLAYGAIN_TRACK_PEAK` and
`REPLAYGAIN_ALBUM_PEAK` proposals, the CSV export, and the sidecar all
receive the plain sample peak.

The ReplayGain 2.0 specification defines the peak fields as the maximum
sample value, and that stays the interoperable default. But sample peak
understates the analogue reconstruction level: inter-sample peaks
routinely exceed it by 0.5–1 dB on loudness-war material, which matters
to anyone using the peak for clipping prevention. foobar2000 and
loudgain both expose true-peak scanning as an option and store the
result in the same `REPLAYGAIN_*_PEAK` fields — linear amplitude, values
above 1.0 permitted — so there is established interop precedent and no
separate tag convention to invent (no player reads a dedicated
"peak kind" tag).

## Decision

### Policy

A "True peak as ReplayGain peak" checkbox joins the sidecar-only
checkbox in the Properties ReplayGain section (persisted QSettings key
`replaygain/true-peak`, default off). When enabled, the scan's
`REPLAYGAIN_TRACK_PEAK` proposal carries the track's linear true peak
and `REPLAYGAIN_ALBUM_PEAK` the programme maximum of the members' true
peaks, exactly as foobar2000's true-peak option does. When a track's
true peak is unavailable the proposal falls back to its sample peak
rather than dropping the field. The default stays sample peak per the
ReplayGain 2.0 specification.

### Recording the kind

Embedded tags and CUE `REM REPLAYGAIN_*` lines carry only the value —
no interoperable convention exists for the peak kind, and inventing a
tag would not be read by anyone. The Trackbench-owned surfaces do
record it:

- Sidecar entries gain an optional `"peak_kind": "true_peak"` string
  member (tkmeta stays version 1). Absence means sample peak, so every
  existing sidecar remains valid and byte-identical on rewrite; the
  strict parser accepts exactly the one value and rejects anything
  else. The flag travels from the scan through
  `MetadataWritePlanOptions.true_peak_loudness` and the planned sidecar
  entries; a committed entry updates its kind whenever a peak value is
  written and clears it when both peaks are removed, so a merge never
  mislabels values written under the other policy.
- The CSV export gains a final `peak_kind` column (`sample` or
  `true_peak`) describing the exported peak columns.
- The scan's proposal rationale mentions true peak so the plan review
  shows which policy produced the value.

## Consequences

- Sample peak remains the measured baseline in scan results; the policy
  only decides which number becomes the proposed `REPLAYGAIN_*_PEAK`.
  Rescanning with the other setting yields new proposals, not edits.
- A sidecar written by a newer build with `peak_kind` present is
  rejected by older builds' fail-closed parser. Accepted: the format is
  pre-release and self-owned, and fail-closed beats silently dropping
  provenance.
- Peaks above 1.0 become routine under the policy; the sidecar codec
  already permits any non-negative peak, and downstream consumers treat
  the value as linear amplitude either way.
- The loudness provenance view is unchanged — it projects field values
  and origins; the kind lives in the sidecar document and the export.
