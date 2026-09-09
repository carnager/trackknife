# ADR-0131: Cover artwork carriage into converted output

- Status: accepted
- Date: 2026-09-09
- Owners: Trackknife project
- Extends: ADR-0105/0106 conversion core and metadata transfer,
  ADR-0076 artwork inventory, ADR-0108 encoder presets

## Context

The converter transfers text metadata into every qualified target
(ADR-0106) but silently drops cover art, so a converted album is not
"ready for use" (roadmap area 3; the specification requires "copy artwork
subject to format limits"). Qualified artwork *editing* remains native-FLAC
only; conversion writes a fresh, exclusively-owned output, which needs no
journaled mutation path.

## Decision

- `AudioConversionRequest` gains an optional `ConversionArtwork` — exact
  encoded PNG/JPEG bytes plus mime type, dimensions, FLAC/ID3 picture
  type, and description. When present, the converter embeds exactly one
  picture into the output at mux time:
  - **FLAC and MP3**: a second `attached_pic` stream, so FFmpeg's muxers
    write the native `PICTURE` block or ID3v2 `APIC` frame; the picture
    type and description ride the stream's `comment`/`title` metadata.
  - **Opus and Vorbis**: a `METADATA_BLOCK_PICTURE` Vorbis comment carrying
    the base64 FLAC picture structure serialized by the converter.
- Like text metadata, artwork is verified before publication: the finished
  temporary is reread with the project's container-agnostic embedded-
  artwork reader and the returned bytes must equal the requested bytes
  exactly, else the conversion fails with `invariant` and the temporary is
  discarded.
- **Source resolution** (`resolve_conversion_artwork`) picks one image per
  source, best-effort and read-only, in this order:
  1. native-FLAC embedded pictures via the qualified ADR-0076 inventory
     (front cover first, then ordinal), preserving role-level picture type
     and description under the revision/fingerprint bracket;
  2. the first embedded picture of any other container via the FFmpeg
     reader, normalized to picture type front cover;
  3. exact-basename external siblings from the default inventory policy
     (`cover`/`folder`/`Folder`/`front` × png/jpg/jpeg), as front cover.
  Only PNG/JPEG up to 16 MiB qualifies; a source without usable artwork
  converts without a picture rather than failing.
- The parallel scan gains `ConversionScanOptions::carry_artwork`; the
  Bench converter dialog exposes it as a persisted "Embed cover art"
  checkbox, enabled by default.

## Consequences

- Every qualified preset (FLAC, Opus, MP3, Vorbis) produces outputs whose
  cover art round-trips byte-exactly, proven by real-file tests that
  reread each target with TagLib (`pictureList`, `METADATA_BLOCK_PICTURE`,
  `APIC`) — independent of the FFmpeg writer.
- Only one picture is carried; multi-picture carriage, fine-grained native
  type preservation beyond the role mapping, and surfacing
  "converted without artwork although the source had some" per item remain
  follow-up work.
- The metadata module exposes its PNG/JPEG signature inspector
  (`inspect_encoded_image_bytes`) for reuse instead of duplicating the
  parser in the converter.
