# ADR-0097: Validated loudness analysis core

- Status: accepted
- Date: 2026-09-02
- Owners: Trackknife project
- Opens: M7 universal parallel ReplayGain

## Context

M7 treats loudness as first-class metadata. Everything above — grouping
modes, the parallel decode graph, the result grid, embedded mappings —
stands on one measurement core whose correctness must be validated, not
assumed.

## Decision

### One streaming analyzer over the existing decoder's PCM

- A new `Trackknife::Loudness` library wraps libebur128 (≥ 1.2).
  `LoudnessAnalyzer` consumes interleaved float frames — exactly what
  the ADR-qualified FFmpeg decoder already emits — and reports EBU R128
  gated integrated loudness, the linear sample peak, and optionally the
  oversampled true peak (`EBUR128_MODE_TRUE_PEAK`).
- High-rate policy: analysis always runs at the source's native sample
  rate; libebur128 derives its K-weighting filter per rate, so 88.2/96/
  176.4/192 kHz material is never resampled for measurement.
- ReplayGain 2.0 target: gains aim tracks at −18 LUFS (the EBU R128
  −23 LUFS reference raised by the 5 dB the RG2 specification mandates),
  exposed as `replaygain_reference_lufs` and `track_gain_db()`.

### Album loudness is programme loudness

- Analyzer states stay valid after `finish()`, and
  `album_integrated_lufs` combines them through
  `ebur128_loudness_global_multiple` — the gated loudness of the whole
  programme with the relative gate applied across every track's blocks,
  never an average of per-track gains.

### Honest unmeasurability

- Material shorter than one 400 ms gating block has no gated integrated
  loudness; `TrackLoudness::measurable()` exposes that so no caller ever
  derives a gain from negative infinity. The real-fixture integration
  test pins this: the 100 ms tone decodes, analyzes, reports its peaks,
  and is honestly unmeasurable.

### Validation against the ITU conformance points

- The unit suite checks libebur128 against ITU-R BS.1770's analytic
  references: a 997 Hz full-scale sine reads −3.01 LKFS in one channel
  and ~0.00 LKFS across both stereo channels (both within 0.1 dB), and
  halving the amplitude moves loudness by exactly −6.02 dB (within
  0.05). Album reduction is verified to track the loud programme rather
  than averaging in a gated-out quiet track, and true peak is verified
  ≥ sample peak.

## Consequences

- Later M7 slices (grouping modes, the parallel decode graph, the
  result grid, per-format embedded mappings) consume one small validated
  surface: create → add_frames → finish, plus the album reducer.
- Cancellation is honoured mid-stream through the existing token, so
  the future parallel graph can stop cleanly.
