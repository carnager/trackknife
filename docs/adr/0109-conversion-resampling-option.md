# ADR-0109: Conversion resampling option

- Status: accepted
- Date: 2026-09-03
- Owners: Trackknife project
- Extends: ADR-0105 conversion core, ADR-0107 converter dialog

## Context

The converter always kept the source rate (subject to encoder
constraints). Downsampling hi-res material for a phone library — or
normalizing a mixed library onto one rate — needs an explicit choice.

## Decision

- `AudioConversionRequest` (and the scan options, one policy per scan)
  gain an optional target sample rate. Absent keeps today's behavior;
  set, it replaces the source rate *before* encoder negotiation, so the
  encoder's supported-rate constraint still applies afterwards — Opus
  maps every request into its 48 kHz family instead of failing.
  Requests outside 8-768 kHz fail typed before any decoding.
- The converter dialog gains a "Resample" combo — Keep source rate
  (default) or a fixed rate from 44.1 to 192 kHz — persisted across
  sessions and passed to the whole scan. It lives on the request, not
  the encoder preset, so one policy composes with any preset.
- Duration verification already scales with the output rate, so the
  no-partial-output proof covers resampled conversions unchanged.

## Consequences

- Tests pin the semantics end to end: FLAC honors a forced 96 kHz with
  a scaled verified duration, Opus lands on 48 kHz for the same
  request, absurd rates fail typed leaving nothing, and the dialog's
  choice provably reaches the pipeline (the output decodes at 96 kHz).
- A "downsample only above N" cap mode remains open for a later slice.
