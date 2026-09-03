# ADR-0110: Conversion bit-depth policy

- Status: accepted
- Date: 2026-09-03
- Owners: Trackknife project
- Extends: ADR-0109 resampling option, ADR-0105 conversion core

## Context

Downsizing a 24-bit/96 kHz library to CD format (16/44.1) needs a
stored-bit-depth choice beside the sample-rate choice, and quantizing a
float pipeline down to 16 bits without dither would trade inaudible
noise for correlated truncation distortion.

## Decision

- The conversion request and scan options gain an optional target bit
  depth, 16 or 24 (anything else fails typed before decoding). Where
  the encoder stores integer PCM — FLAC — the request outranks the
  preset's sample-format hint and selects s16 or s32 (FFmpeg's FLAC
  encoder stores s32 as 24-bit). Float-based encoders like Opus have
  no stored depth; the request is inert there rather than an error,
  matching the rate policy's constraint-mapping behavior.
- Whenever the negotiated output format is 16-bit integer, the
  resampler applies high-passed triangular (TPDF) dither. 24-bit
  output from the 32-bit-float pipeline is exact and stays undithered.
- The converter dialog offers "Bit depth" (Preset default /
  16-bit (dithered) / 24-bit) beside Resample, persisted across
  sessions, one policy per run composing with any preset.

## Consequences

- The headline path is pinned end to end: a generated 24-bit/96 kHz
  source converts to FLAC at 16/44.1 (probed s16, scaled verified
  duration), the preset default still produces 24/96, Opus ignores the
  depth request, unsupported depths fail typed, and the dialog's
  choice provably reaches the pipeline.
- Preserving each source's own depth ("keep source") would need
  per-item probe data in the request; deferred with the
  downsample-only rate cap.
