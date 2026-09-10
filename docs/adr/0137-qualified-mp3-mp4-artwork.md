# ADR-0137: Qualified MP3 and MP4 artwork management

- Status: accepted
- Date: 2026-09-10
- Owners: Trackknife project
- Extends: ADR-0076 artwork inventory, ADR-0078/0080/0081 FLAC artwork
  writes, ADR-0103 MP3 writer, ADR-0136 MP4 writer

## Context

Embedded artwork reading and mutation were native-FLAC only: the
inventory's embedded branch, the byte reread, the write plan's readiness,
the prepared-copy writer, the commit projection, and the workspace gate
all hard-coded `taglib-flac-picture-v1` (roadmap area 4). MP3 and MP4 are
the next most common collection formats.

## Decision

- **Inventory** gains two embedded adapters beside FLAC, selected by
  container signature:
  - `taglib-id3v2-apic-v1`: ID3v2 `APIC` frames in frame order. APIC
    picture types share FLAC's numbering, so the role mapping and the
    exact type vocabulary ("Front Cover", "Other", …) are reused;
    descriptions and frame MIME are preserved, dimensions are sniffed.
  - `taglib-mp4-covr-v1`: `covr` entries in list order. `covr` carries no
    picture type and no description — every item is an untyped front
    cover with empty `native_type`/`description`; MIME comes from the
    cover format enum, falling back to the signature sniff.
  `read_artwork_image_bytes` rereads embedded ordinals per container
  under the same revision/fingerprint/MIME/dimension equality contract,
  so donors, export, thumbnails, and conversion carriage work on all
  three formats.
- **Write plan and dispatch:** readiness and the build gate accept any
  qualified artwork adapter (`is_qualified_artwork_adapter`);
  `prepare_qualified_artwork_write_copy` dispatches by adapter name,
  mirroring the text-writer dispatcher. The journal schema is untouched —
  its evidence (fingerprints, counts, ordinals) is format-neutral — and
  the commit's projected-inventory prediction branches per adapter for
  the `native_type`/`description` semantics above.
- **Writers** (`prepare_mp3_artwork_write_copy`,
  `prepare_mp4_artwork_write_copy`) mirror the FLAC driver: revision
  brackets, fresh-inventory revalidation, exclusive prepared copy, the
  change applied through TagLib (mutating the targeted APIC frame in
  place to keep ordinals and descriptions; replacing the indexed `covr`
  entry), then verification — text document unchanged, resulting
  inventory exactly as planned, every unrelated picture byte-identical
  by fingerprint, and the existing container preservation proofs
  (MPEG audio region plus APEv2 items; MP4 `ftyp`/`mdat`/other boxes).
  An MP4 add ignores the requested role (covr is untyped) and rejects a
  non-empty description as unsupported.
- The workspace artwork section gates on the qualified-adapter set and
  generalizes its wording; the commit entry point is renamed
  format-neutrally.

## Consequences

- Replace, remove, add, copy, Cover Art Archive fetch, export, and
  thumbnails work identically for FLAC, MP3, and MP4 sources with the
  same journaled publication and undo evidence.
- Real-file tests round-trip each new adapter (inventory identity,
  replace/remove/add, unrelated-picture preservation, PCM equality) and
  a commit case proves the per-adapter inventory projection.
- Ogg (`METADATA_BLOCK_PICTURE`) artwork mutation, richer `covr`
  formats (BMP/GIF writes), and multi-change-per-file plans remain
  follow-up work.
