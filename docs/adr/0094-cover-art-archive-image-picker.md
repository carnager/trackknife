# ADR-0094: Cover Art Archive image picker

- Status: accepted
- Date: 2026-09-02
- Owners: Trackknife project
- Extends: ADR-0091 front-cover fetch (and its replace addendum)

## Context

ADR-0091 covered the one-click happy path — the release's front cover.
The archive usually holds more: back covers, booklets, media shots. The
remaining M6 artwork gap is choosing among them without leaving the
application, with enough visual context to tell three booklets apart.

## Decision

### One boundary, recomposed

- `ArtworkCoverArtService` is reshaped into three injected functions:
  `fetch_listing` (release id → parsed archive listing), `fetch_bytes`
  (any https archive URL → bytes, through the same paced cache-first
  client), and `store_image` (verified PNG/JPEG bytes → local file ready
  for review). The front-fetch orchestration moves from the dialog into
  the artwork section, composed from these three; the parser now also
  carries each image's 250px thumbnail URL (https-upgraded, dropped when
  insecure).

### A compact picker beside the one-click fetch

- "Fetch cover" keeps its one-click front semantics. The new "Covers…"
  button (`bench-metadata-artwork-covers`) loads the listing and opens a
  window-modal picker listing every archive image with its types,
  approval state, and comment. Thumbnails trickle in through the paced
  fetcher without blocking the picker.
- "Use this image" (or double-click) downloads the full image and places
  it by mapped role: Front (typed or flagged) runs the ADR-0091
  replace-or-add front placement; Back maps to the back role, Medium to
  disc, everything else to other — all added through the ordinary
  preservation-exact review, one plan, direct apply.

## Consequences

- The whole archive inventory of a release is usable in-app: refresh the
  front with one click, or pick any booklet/back/media image with two.
- The scripted `MusicBrainzLookupService` seam still backs everything,
  so the picker is offscreen-tested end-to-end — listing, thumbnail
  delivery, role mapping, and the applied back cover on a real FLAC —
  with zero network.
- M6's remaining open item is the optional AcoustID evaluation.
