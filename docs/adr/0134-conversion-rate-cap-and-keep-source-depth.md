# ADR-0134: Downsample-only rate cap and keep-source bit depth

- Status: accepted
- Date: 2026-09-10
- Owners: Trackknife project
- Extends: ADR-0109 conversion resampling, ADR-0110 bit-depth policy

## Context

The converter's rate option forces one output rate for every source, so a
mixed 44.1/96 kHz selection cannot be normalized "down only": capping
hi-res material at 48 kHz without touching CD-rate sources was ADR-0109's
recorded follow-up. Likewise the bit-depth option forces 16 or 24 for the
whole scan; keeping each source's stored depth was deferred in ADR-0110
because it needs per-item probe data.

## Decision

- `AudioConversionRequest` gains `sample_rate_cap`: sources above the cap
  are resampled to it, sources at or below keep their rate. The cap shares
  the 8–768 kHz bounds, is mutually exclusive with `target_sample_rate`
  (`invalid_argument`), and the encoder's supported-rate constraint still
  applies afterwards — Opus maps every result into its 48 kHz family and
  never downward.
- `AudioConversionRequest` gains `keep_source_bit_depth`: the converter
  probes the source's stored sample format and keeps 16 bits for formats
  storing at most 16 (`u8`, `s16`), otherwise 24 — the pipeline's maximum
  stored depth — including float and unknown formats. Mutually exclusive
  with `target_bit_depth`; encoders without integer PCM (Opus) remain
  unaffected. Probe failures fail the item.
- `ConversionScanOptions` and the converter dialog carry both: the
  resample combo gains "Downsample to 44.1/48 kHz if higher" entries and
  the bit-depth combo a "Keep source depth" entry, persisted under the
  existing settings keys.

## Consequences

- One scan normalizes a mixed-rate collection downward-only and preserves
  each source's stored depth, closing the last two open M8 "Next" slices
  besides grouped/merge output.
- Real-file tests pin: capped hi-res lands at the cap, at-or-below rates
  stay untouched, kept depth reproduces `s16`/`s32` stored formats, and
  the two option pairs reject conflicting combinations.
