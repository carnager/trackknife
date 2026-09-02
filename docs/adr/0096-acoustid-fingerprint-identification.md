# ADR-0096: AcoustID fingerprint identification

- Status: accepted
- Date: 2026-09-02
- Owners: Trackknife project
- Extends: ADR-0088 MusicBrainz client and ADR-0090 in-app identification

## Context

The M6 milestone made AcoustID conditional: it ships only if match
quality justifies the dependency. The evaluation ran against the user's
real Picard-tagged corpus with the embedded MUSICBRAINZ_TRACKID as
ground truth: 11/11 top-result hits on a studio disc, 8/9 on a
live-heavy disc (the miss being a crossed submission in the AcoustID
database itself), and 8/8 plausible ~0.98-score matches on an entirely
untagged album by an obscure band — the exact rescue case. Quality
justifies shipping. Chromaprint arrives with no build dependency:
`fpcalc` is invoked as an external tool, the same binary Picard ships.

## Decision

### Typed AcoustID boundary beside ws/2

- The musicbrainz library gains `build_acoustid_lookup_body`
  (form-encoded POST requesting recordings with releases) and
  `parse_acoustid_lookup` — bounded like every parser, score-sorted,
  keeping recording and release ids only when they are valid MusicBrainz
  UUIDs, and turning a non-ok status into a typed backend error carrying
  the service's message.

### The service seam grows two optional functions

- `MusicBrainzLookupService` adds `fingerprint` (file path → fpcalc
  duration+fingerprint via an asynchronous QProcess; a missing binary
  fails typed) and `acoustid_lookup` (fingerprint → paced POST at 400 ms
  minimum interval, under AcoustID's three-per-second guideline). The
  client key comes from the `musicbrainz/acoustid-client-key` QSettings
  value — user-registered, never compiled in — and an empty key disables
  the feature with a typed message naming the setting. Both functions
  empty means the Identify dialog offers text search only; constructor
  signatures are untouched.

### Fingerprints feed the same version picker

- The Identify dialog gains "Fingerprint files": every selected file is
  fingerprinted and looked up sequentially, results at score ≥ 0.5 vote
  for the releases their matched recordings appear on, and the top
  candidates (by how many files matched, shown as "7/9 files" in the
  Match column) are loaded through the ordinary release lookup into the
  same results list. Everything after the picker — choose a version,
  align, propose, stage as colored drafts — is the existing ADR-0089/0090
  path, provenance marks included. Candidates whose release lookup fails
  drop out instead of blocking the list.

## Consequences

- Files with no usable tags at all — the case text search cannot serve —
  are identified in-app with two clicks, and identification quality is
  measured, not assumed.
- Fingerprints are computed on demand and never stored; AcoustID sees
  only fingerprint, duration, and the user's own API key.
- Submission of new fingerprints to AcoustID (giving back to the
  database) is a possible follow-up on the same seam.
