# ADR-0149: Opus loudness follows RFC 7845 R128 tags

Date: 2026-09-11

Status: accepted

## Context

Opus was the last format without a loudness story. RFC 7845 §5.2 defines
it precisely: `R128_TRACK_GAIN` and `R128_ALBUM_GAIN` are Vorbis
comments holding a Q7.8 fixed-point decibel value (a signed 16-bit
integer, so dB = value / 256) *relative to the `OpusHead` output gain*,
referenced to the R128 target of −23 LUFS — and `REPLAYGAIN_*` comments
"SHOULD NOT be used in Opus files". MPD, VLC, and ffmpeg-based players
read exactly this convention.

Trackbench did neither side of it. The decoder read only `REPLAYGAIN_*`
text tags and deliberately excluded Opus from the ffmpeg side-data
fallback, so a correctly tagged `.opus` file played at unity gain. A
scan proposed `REPLAYGAIN_*` fields that the qualified Ogg writer would
happily embed — the tags the RFC advises against, with a −18 LUFS
reference no RFC-conforming player expects.

Two facts make the mapping between the conventions trivial. libopus
applies the `OpusHead` output gain during decode, so everything
Trackbench measures or plays already includes it — a value "relative to
output gain" is simply additive on our PCM path. And the references
differ by a constant: ReplayGain 2.0 normalizes to −18 LUFS, R128 to
−23 LUFS, so `replaygain_db = r128_db + 5` in both directions.

## Decision

### Reading and playback

`AudioDecoder::replay_gain()` gains an Opus branch: when the selected
stream is Opus and an `R128_TRACK_GAIN` or `R128_ALBUM_GAIN` comment
parses as an exact in-range Q7.8 integer, the gains become
`value / 256 + 5` dB — directly comparable to every other ReplayGain
source in the pipeline — and the peaks stay empty because the RFC
defines no peak field (the playback clamp simply does not engage).
When R128 tags are present they own the result; `REPLAYGAIN_*` remnants
on the same file are ignored rather than mixed. Files without R128 tags
keep the existing lenient `REPLAYGAIN_*` reading. The Opus side-data
exclusion stays: ffmpeg's demuxer-level ReplayGain frame has no defined
reference for Opus.

The bench playback override projection needs no R128 branch: it only
reads sidecar and CUE-segment provenance, and R128 fields are excluded
from both carriers. Embedded R128 reaches playback through the decoder
branch above.

### Scanning and writing

A measured Opus source stages `R128_TRACK_GAIN` / `R128_ALBUM_GAIN`
proposals — `round(256 × (gain_db − 5))`, clamped to the signed 16-bit
Q7.8 domain — instead of the four `REPLAYGAIN_*` fields, and stages no
peaks. The values are relative to the output gain by construction,
because the measurement decoded PCM that already includes it. The
proposal rationale names the convention. The scan learns which items
are Opus from the decoder it already opens (`LoudnessTrackScan.opus`).

The sidecar-only policy (ADR-0146) outranks this: with "Store in
sidecar only" enabled, Opus items stage conventional `REPLAYGAIN_*`
like every other source, because the sidecar is a Trackbench-owned
carrier with a fixed −18 LUFS reference and the audio file is not
touched. R128 fields themselves stay excluded from CUE and sidecar
routing (no such conventions exist) and stay blocked on logical
tracks, as before.

The loudness provenance view grows two columns for the R128 fields so
Opus drafts and embedded values are visible beside the conventional
ones.

### Out of scope

Rewriting the `OpusHead` output gain remains the separate expert
operation the docs describe; the qualified Ogg writer's proof that
packet 0 is byte-identical stays absolute. The converter continues to
strip both loudness families (ADR-0133); emitting fresh R128 tags after
an Opus conversion is future work.

## Consequences

- Correctly tagged Opus files finally play normalized, and freshly
  scanned ones are readable by MPD/VLC/ffmpeg without any Trackbench
  knowledge.
- A library mixing Opus and other formats in one album programme gets
  one shared album gain, encoded per-format (Q7.8 vs dB text).
- Opus rows have no peak, so the clipping clamp never limits their
  positive gains; the R128 convention accepts that, and the sidecar
  path remains available where peaks matter.
- `R128_*` values are strict integers: scientific notation, decimals,
  or out-of-range magnitudes are ignored on read, never guessed at.
