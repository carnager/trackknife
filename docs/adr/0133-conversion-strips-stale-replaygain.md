# ADR-0133: Conversion strips stale ReplayGain instead of transferring it

- Status: accepted
- Date: 2026-09-09
- Owners: Trackknife project
- Extends: ADR-0106 conversion metadata transfer, ADR-0131/0132 conversion
  slices

## Context

The mux-time text transfer copies every effective field into the converted
output — including `REPLAYGAIN_*` and `R128_*` tags measured against the
source signal. Every conversion decodes and re-encodes through the float
pipeline (lossy codecs change the signal outright; resampling and 16-bit
dither change it for lossless targets too), so the copied values are stale.
The specification requires "do not transfer ReplayGain when the signal was
changed; scan output instead".

## Decision

- `convert_audio_file` strips loudness fields from the requested metadata
  before the mux-time transfer: every canonical name beginning with
  `replaygain_` and the `R128_TRACK_GAIN`/`R128_ALBUM_GAIN` pair. All other
  fields transfer unchanged.
- The pre-publication verification is extended: besides the exact reread of
  every transferred field, the finished temporary must contain **no**
  loudness field, else the conversion fails with `invariant` and the
  temporary is discarded.
- The rule is unconditional for this slice: a conversion is treated as
  signal-changing. A proven bit-identical lossless transfer exception, and
  automatically rescanning converted outputs (which needs a qualified
  post-encode tag write into each target container), remain follow-up work
  tied to the M7 storage milestone.

## Consequences

- Converted files never carry loudness claims measured against a different
  signal; users rescan outputs with the existing measurement tools.
- Real-file tests convert sources whose request carries ReplayGain and
  R128 fields into every qualified preset and prove the outputs keep their
  ordinary tags while containing no loudness field.
