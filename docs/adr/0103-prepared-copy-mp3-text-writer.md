# ADR-0103: Prepared-copy MP3 text writer

- Status: accepted
- Date: 2026-09-03
- Owners: Trackknife project
- Extends: ADR-0043 FLAC and ADR-0095 WavPack prepared-copy writers

## Context

MP3 is the most widespread format Trackbench could not yet write. The
cheaper APEv2 family (Musepack, Monkey's Audio) is blocked on honest
fixtures — no encoder for either exists on the development system — so
MP3, fixture-able through libmp3lame, is the third qualified writer.

## Decision

- The reader detects native MPEG (TagLib type plus a leading "ID3" tag
  or MPEG frame sync) as adapter `taglib-mpeg-v1` with writable fields;
  pictures stay false (ID3 APIC artwork is future work), and unsupported
  objects do not block preservation because the writer verifies the
  audio region itself.
- `prepare_mp3_metadata_write_copy` runs the shared prepared-copy flow
  with paired-totals handling disabled — ID3 has no TOTALTRACKS/
  TRACKTOTAL spelling pair; every field writes exactly one mapped frame
  (standard frames via TagLib's ID3v2 mapping, TXXX for the rest).
- Binary preservation is region-based: the MPEG audio bytes between the
  leading ID3v2 tag (sized from its own syncsafe header, footer flag
  honoured) and any trailing ID3v1/APEv2 tags must be byte-identical
  even as the leading tag resizes, and trailing APEv2 binary items must
  survive byte-exactly. The APEv2/ID3v1 trailer analysis moved into a
  shared detail header used by the WavPack writer too. Unlike WavPack,
  ID3v1 trailers are tolerated: they are a rewritable tag on MP3, not an
  unqualified appendage.
- Commit and refreshed publication now dispatch through one
  `prepare_qualified_metadata_write_copy` keyed by adapter, and
  `is_qualified_text_adapter` names the qualified set — future formats
  extend two functions instead of every call site.

## Consequences

- MP3 files get the full tagging workflow — grid edits, scripts,
  Suggest/Identify/ReplayGain proposals, Rename/Move with saved tags —
  through the same journaled, preservation-verified pipeline.
- Real-fixture coverage: a libmp3lame tone (provenance in the fixture
  README) round-trips replace/remove/add with the audio region verified
  byte-identical through an ID3v2 resize, and an appended ID3v1 trailer
  is updated rather than rejected while the audio still verifies.
- Musepack/Monkey's Audio remain open pending fixture tooling; Ogg
  Vorbis/Opus remain open pending a decoded-PCM verification design
  (their container re-pages on every tag write).
