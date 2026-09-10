# ADR-0136: Qualified MP4/M4A text writer

- Status: accepted
- Date: 2026-09-10
- Owners: Trackknife project
- Extends: ADR-0103 MP3 writer, ADR-0114 Ogg writers, ADR-0066 native
  identity policy

## Context

MP4/M4A (AAC and ALAC) files read through the generic TagLib properties
path but advertise no write capability, so tag edits on a very common
collection format fail with `writer_unavailable` (roadmap area 4).
ADR-0066 additionally required a deliberate native-identity decision
before an MP4 writer.

## Decision

- `prepare_mp4_metadata_write_copy` (adapter `taglib-mp4-v1`) reuses the
  shared prepared-copy text core over `TagLib::MP4::File`, exactly like
  the MP3 and Ogg writers: revision-bracketed exclusive copy, plan
  original verification, PropertyMap patch application, exact reread
  verification, preservation proof, journaled publication unchanged.
- **Native identity (empirically pinned):** TagLib's documented table maps
  the standard atoms onto the conventional logical names (`©nam` ↔ TITLE,
  `©ART` ↔ ARTIST, `©alb` ↔ ALBUM, `trkn` ↔ TRACKNUMBER, …); every other
  property name round-trips as an exact freeform
  `----:com.apple.iTunes:<NAME>` atom with its spelling preserved. No
  spelling heuristic aliases a freeform key onto a standard atom. `trkn`
  carries its total inside the same atom, surfacing as the combined
  `number/total` TRACKNUMBER value, so the FLAC-style paired-totals
  expansion stays off. MP4 stores exact empty values, so the ID3-style
  exact-empty plan gate does not apply (like Ogg).
- **Preservation proof:** TagLib rewrites the trailing `moov` (and may
  grow `free` padding), so a whole-file byte proof cannot hold. The
  writer proves instead that the `ftyp` and `mdat` boxes are
  byte-identical (a top-level box parse of both files, 64-bit sizes
  included), that the top-level box sequence is unchanged apart from
  `moov`/`free` sizes, and the qualification test additionally decodes
  both files and compares the full PCM, mirroring the Ogg test. A
  pre-existing `covr` atom must survive a text write byte-identically.
- The reader detects native MP4 (`ftyp` marker at offset 4 plus the
  TagLib type) and advertises `taglib-mp4-v1` with writable text fields;
  pictures stay FLAC-only.

## Consequences

- Tag editing works on MP4/M4A with the same honesty guarantees as the
  other qualified writers; real-file tests round-trip standard atoms,
  freeform creation and exact reread, the combined `trkn` total, empty
  values, `covr` survival, and byte-identical `ftyp`/`mdat`.
- ALAC shares the container and the same adapter; a dedicated ALAC
  fixture, `covr` write support, and richer typed atom kinds (integers,
  booleans) remain follow-up work.
