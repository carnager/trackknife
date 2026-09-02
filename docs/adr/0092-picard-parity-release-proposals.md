# ADR-0092: Picard-parity release proposals

- Status: accepted
- Date: 2026-09-02
- Owners: Trackknife project
- Extends: ADR-0087 paired totals identities, ADR-0089 matching and
  proposal bridge, and ADR-0090 in-app identification

## Context

The M6 milestone asks for complete identifier coverage — recording,
track, release, release-group, artist, and work ids — with credited
names kept separate from sort names. The ADR-0089 bridge stopped at the
basic tags plus six ids; a Picard user identifying the same album would
get materially more metadata than Trackbench proposed.

## Decision

### The lookup carries everything the tags need

- The release lookup adds `isrcs`, `recording-level-rels`, and
  `work-rels` to its `inc` set. Parsing gains, all bounded by a new
  `identifiers` limit: release-group primary/secondary types and
  first-release date, the text representation's script and language,
  medium titles, per-recording ISRC lists, and work ids taken only from
  relation entries whose work id is a valid MusicBrainz UUID.
- `FlattenedReleaseTrack` carries its medium's format and title so the
  bridge can propose per-disc fields without re-walking the release.

### The bridge proposes Picard's matched-release tag set

- New per-file proposals beside the existing ones: ARTISTSORT and
  ALBUMARTISTSORT (sort names joined with the same join phrases,
  falling back to the credited name — credited and sort names never
  collapse into one), ORIGINALDATE and ORIGINALYEAR from the release
  group's first release, RELEASETYPE (lowercased primary type followed
  by lowercased secondary types), RELEASESTATUS (lowercased),
  RELEASECOUNTRY, SCRIPT, LANGUAGE, LABEL, CATALOGNUMBER, BARCODE,
  MEDIA, DISCSUBTITLE, ISRC (multi-value), and — with identifiers —
  MUSICBRAINZ_WORKID. Empty source data is never proposed.
- Disc numbering follows Picard: single-disc releases propose
  DISCNUMBER 1 and TOTALDISCS 1 instead of omitting them.
- Six names join the conventional FLAC registry so they keep one
  logical column and canonical identity everywhere: SCRIPT,
  DISCSUBTITLE, ORIGINALYEAR, RELEASETYPE, RELEASESTATUS,
  RELEASECOUNTRY. The rest already had conventional identities.

## Consequences

- An identified album now stages the same field set Picard would write
  for a matched release, all through the ADR-0086 boundary as ordinary
  colored drafts with per-field confidence and version-naming
  rationales.
- Disc ids (`MUSICBRAINZ_DISCID`) remain out of scope: they derive from
  physical disc table-of-contents data no file-based lookup can supply.
  AcoustID remains a separate evaluation.
