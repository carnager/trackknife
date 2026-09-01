# ADR-0085: Artwork thumbnails and direct apply

- Status: accepted
- Date: 2026-09-01
- Owners: Trackknife project
- Extends: ADR-0083 direct apply and ADR-0084 silent recovery
- Amends: ADR-0077 read-only artwork presentation and ADR-0080 artwork
  review/apply surfaces (their inventory core, write plans, and journaled
  batch executor are unchanged)

## Context

The Artwork tab was an artwork surface without images: an 11-column
inventory (SHA-256 hex, native types, ordinals, raw paths), an always-on
five-column "Source capabilities" diagnostics table, a permanent paragraph
about SQLite storage internals, and the ADR-0080 review-then-progress dialog
chain that ADR-0083 had already removed from tag and path Apply.

## Decision

### The inventory shows pictures

- Each row leads with a decoded thumbnail. The existing background inventory
  job rereads encoded bytes through the revision-qualified
  `read_artwork_image_bytes` evidence (duplicates reuse the earlier decode)
  and scales in the worker; the UI thread only displays. Undecodable images
  fall back to a dash.
- Columns compress to thumbnail · File · Role · Image ("PNG · 64 × 64 ·
  12.3 KB") · Source ("Embedded"/"External", duplicate note). Full paths,
  SHA-256, native picture types, and ordinals move to tooltips.
- The storage-note paragraph and the capabilities table are gone. A file
  earns a row in the existing problems pane only when it is actually
  view-only (non-FLAC adapter, changed since Properties opened), with one
  plain-language reason; the status line counts images, files, and
  view-only files.

### Artwork changes apply directly

- Add / Replace / Remove / Copy revalidate as before, but a wholly ready
  plan enters the bounded ADR-0080 batch executor immediately. The plan and
  apply dialogs are deleted; a blocked plan opens the compact ADR-0083
  feedback window listing only the offending files, kind-qualified.
- Progress lives inline beside the section status: a progress bar, "Saving
  artwork · n of N", and a Stop button on the shared cooperative
  cancellation source. Fully committed runs end with one status line and the
  automatic inventory refresh; failures and stops open the feedback window
  listing untouched files.
- Export drops its modal `QProgressDialog` for the same inline progress and
  Stop; not-written files (never-overwrite collisions, stops, errors) are
  reported in the feedback window.

## Alternatives considered

### Keep a compact review dialog for artwork only

Rejected. The same trust argument as ADR-0083 applies — the operator picked
the image and the targets; the safety chain (fresh revalidation, journaled
single-source commits, recovery) never depended on the dialog.

### Thumbnails from a persistent cache

Rejected. ADR-0076/0077 deliberately store no image bytes; transient
revision-qualified rereads in the existing background job keep that
contract and stay bounded by the 64-source scope limit.

## Consequences

- The tab reads as an artwork manager: pictures first, plumbing in tooltips.
- One dialog vocabulary serves tags, paths, and artwork: silent success,
  compact problems-only feedback.
- Inventory loads reread and decode image bytes; cost is bounded by the
  source limit and the existing 16 MiB per-image cap, and cancellation
  still aborts mid-batch.

## Validation

- Offscreen tests prove the inventory presents decoded thumbnails with the
  compact columns and tooltips, reports a mutation-less fixture as view-only
  in the problems pane, and that clicking Remove applies directly — no
  review or progress dialog — commits the exact occurrence, refreshes the
  inventory, and opens no feedback window on success.

## Revisit when

- non-FLAC artwork writers arrive (view-only reasons and Add gating change);
- provider-proposed artwork (M6) needs inspection before writing.
