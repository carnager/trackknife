# ADR-0091: Cover Art Archive front-cover fetch

- Status: accepted
- Date: 2026-09-02
- Owners: Trackknife project
- Extends: ADR-0080/0081 artwork review and Apply, ADR-0088 MusicBrainz
  client, and ADR-0090 in-app identification

## Context

After ADR-0090 an identified release carries its MUSICBRAINZ_ALBUMID —
possibly still as a colored draft. The M6 milestone asks for Cover Art
Archive proposals "through the existing artwork operations": fetching a
cover must reuse the preservation-exact prepared-copy add path, never a
second write mechanism.

## Decision

### Typed archive boundary beside the ws/2 one

- The musicbrainz library gains `build_cover_art_listing_url` (UUID-
  validated), `parse_cover_art_listing` (bounded like every other parser;
  numeric image ids kept as text; `http://` image URLs upgraded to
  `https://`; entries without a usable https URL dropped), and
  `select_front_cover` — deterministic: flagged front, else typed
  "Front", else first approved, else first.
- Archive image URLs redirect cross-origin to the Internet Archive, so
  the shared Qt transport moves from same-origin to no-less-safe redirect
  handling: cross-origin is allowed, scheme downgrades never are. Both
  listing and image fetches ride the ordinary ADR-0088 paced, cached
  fetch boundary — an image up to the cache's body bound is cached like
  any response; larger ones simply skip the cache.

### One release, one button, the ordinary add path

- Properties derives the artwork tab's release identity alongside the
  artwork scope: every selected file must carry the same non-empty
  MUSICBRAINZ_ALBUMID, read from the grid's draft-or-baseline values so
  a just-identified selection qualifies before Apply has run. Anything
  ambiguous disables the feature rather than guessing.
- The artwork tab gains "Fetch cover" (`bench-metadata-artwork-fetch-cover`),
  enabled exactly when adding is possible, a cover service is wired, and
  the release is unambiguous. It fetches the listing, picks the front
  cover, downloads the image, sniffs PNG/JPEG magic (anything else fails
  typed — the FLAC writer supports nothing else), stores the bytes in a
  dialog-owned temporary directory, and hands the path to the same
  `startReview(add, path, front)` used by manual Add — direct apply,
  journaled prepared-copy write, inventory refresh, problems-only
  feedback. A selection change during the download drops the result.
- The section sees only `ArtworkCoverArtService` — a single injected
  fetch-front callable — so tests script the whole flow with fixture
  JSON and generated PNG bytes, no network.

## Consequences

- Identify → Fetch cover embeds the release's front cover with two
  paced, cached requests and zero new write paths; every preservation
  guarantee of ADR-0078–0081 applies unchanged.
- Non-front archive images (back covers, booklets) remain an explicit
  follow-up.

## Addendum (2026-09-02): the fetch replaces an existing front

Stacking a second front picture on already-covered files was the wrong
default for the primary flow (identify a Picard-tagged album, refresh
its cover). The fetched image now becomes each file's front cover: for
every scoped file that carries an embedded front picture, the plan
stages an ordinal- and fingerprint-targeted replace of exactly that
picture; files without one still get an add. Both intents ride in one
plan through the same preservation-exact prepared-copy apply.
