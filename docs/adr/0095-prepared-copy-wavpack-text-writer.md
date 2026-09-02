# ADR-0095: Prepared-copy WavPack text writer

- Status: accepted
- Date: 2026-09-02
- Owners: Trackknife project
- Extends: ADR-0043 prepared-copy FLAC writer, ADR-0087 paired totals

## Context

M5's format matrix advertised exactly one qualified text writer — native
FLAC. WavPack files read fine through TagLib's generic surface but every
write stayed blocked ("no proven field writer"). The recorded next step
was qualifying WavPack without inferring artwork support from its read
capability.

## Decision

### One shared text-writer core

- The format-agnostic half of the FLAC writer moves to an internal
  header (`text_writer_detail`): exclusive prepared-copy creation,
  original-value and reread-vs-plan verification, and the TagLib
  PropertyMap application including the ADR-0087 paired-totals rules —
  which now live in exactly one place so writers can never diverge on
  them. Error wording is parameterized by format label; every FLAC
  message stays byte-identical.

### A qualified native WavPack adapter

- The reader detects native WavPack (TagLib type plus the leading
  `wvpk` marker) as adapter `taglib-wavpack-v1` with writable fields and
  proven preservation; pictures stay read/write-false, so artwork
  operations remain FLAC-only. Unlike FLAC, APEv2 binary items do not
  block preservation: the writer enumerates and verifies them instead.
- `prepare_wavpack_metadata_write_copy` mirrors the FLAC flow —
  revision-gated exclusive copy, TagLib APEv2 property write, reread
  equality against the plan — and adds WavPack-specific binary proof:
  the byte region before the APEv2 trailer (the WavPack blocks) must be
  identical to the source, and every binary/external APEv2 item (cover
  art and friends) must survive byte-exactly with none invented.
  Sources carrying an ID3v1 trailer are rejected as unqualified rather
  than half-preserved.
- Plan validation runs the same mapping checks for both adapters, and
  the commit and refreshed-publication paths dispatch the preparer by
  the plan's adapter name; journals, recovery, and reconciliation are
  untouched because the prepared-write evidence is format-independent.

## Consequences

- WavPack files gain the full M5 tagging workflow — grid edits,
  scripts, Suggest/Identify proposals, Rename/Move with saved tags —
  through the same journaled, preservation-verified pipeline as FLAC,
  Picard-paired totals included.
- Artwork writes still require native FLAC; the WavPack writer proves
  that existing APE cover-art items pass through untouched.
- Real-fixture coverage: a tagged WavPack tone with an injected binary
  APE item round-trips edits (add, replace, remove, paired totals) with
  audio and item bytes verified, and an appended ID3v1 trailer blocks
  the prepare with a typed unsupported error.
