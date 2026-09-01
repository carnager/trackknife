# ADR-0077: Read-only Properties artwork section

- Status: accepted
- Date: 2026-08-31
- Owners: Trackknife project
- Extends: ADR-0038 file-selection-driven Properties and ADR-0076 bounded
  artwork inventory

## Context

ADR-0076 supplies a typed, revision-qualified native-FLAC and exact-external
artwork inventory, but it deliberately has no user-facing presentation. The
Properties workspace already uses its file selector as the scope authority for
field editing. Artwork inspection must reuse that scope without restoring the
superseded duplicate Fields/Tracks editing modes, blocking the UI thread, or
turning SQLite into an artwork store.

Large Properties selections also need an honest bound. Automatically reading
artwork for every source as soon as Properties opens would make an inspection
section compete with the primary tag grid and could perform substantial I/O
that the user never requested.

## Decision

- The lower half of Properties contains sibling **Fields** and **Artwork**
  sections beneath the existing shared file selector. Switching sections does
  not change edit scope; one selected file gives an individual artwork view and
  several selected files give a multi-source inventory.
- Artwork loading starts only while the Artwork section is active. Selection
  changes cancel and supersede the previous generation. One QtConcurrent task
  calls the synchronous ADR-0076 boundary sequentially, so Properties admits
  only one artwork reader at a time and never performs tag or image I/O on the
  UI thread.
- Repeated logical occurrences with the same raw media path collapse into one
  physical-source request while retaining an occurrence count in the source
  label. The initial presentation accepts at most 64 distinct raw sources in
  one scope. A larger scope is rejected visibly and asks the user to narrow the
  file selection; it is never truncated silently.
- A source-capability table explicitly distinguishes embedded read support,
  exact-sibling external discovery, unavailable artwork changes, and whether
  the freshly observed media revision still matches the Properties capture.
  A separate inventory table exposes role, embedded/external provenance, exact
  native type, MIME/description, dimensions, encoded size, complete SHA-256
  identity, raw artwork source, native ordinal, and duplicate linkage. Typed
  per-source issues remain visible in their own table.
- The section decodes no pixels and holds no encoded image bytes. It does not
  persist inventory rows or image data in SQLite and creates no backup. Closing
  Properties discards the presentation; reopening performs a fresh inventory.
- There are no add, replace, remove, export, copy, or Apply controls. The
  explicit **Changes unavailable (read-only)** capability is presentation of
  the current backend truth, not a disabled mutation promise.

## Alternatives considered

### Read artwork for the complete Properties selection immediately

Rejected. Most tag edits do not require artwork inspection, and opening the
workspace must not begin potentially large unrelated I/O.

### Persist the inventory in the source metadata cache

Rejected. External files have independent revisions, the inventory is cheap to
reconstruct compared with keeping it correct, and the workspace is not a local
artwork database. The existing metadata cache remains correctness evidence for
journal recovery, not a generic UI cache.

### Show only the first selected source

Rejected. It would hide mixed provenance and capability state in the exact
multi-file workflow where Properties is intended to be useful.

### Add thumbnails by reusing the album-list artwork cache

Rejected for this slice. That cache projects one display image and loses the
native ordinal, type, and duplicate distinctions the inventory is meant to
show. A later bounded preview may decode a specifically selected inventory
item without weakening the typed model.

## Consequences

- Artwork inspection follows the same visible selection grammar as tag edits
  without becoming another editing mode.
- Users can see why an external cover is present, why a native type remains
  `Other`, and why no artwork write action is offered.
- Reopening the app or Properties does not depend on stale inventory state; the
  filesystem remains authoritative.
- The 64-source bound may require narrowing a very large initial selection.

## Validation

- An offscreen Properties test opens the Artwork section over a real FLAC with
  an embedded PNG and an exact external JPEG fallback.
- It proves lazy asynchronous completion, native-FLAC embedded-read and exact-
  sibling external-read capability, explicit read-only change state, captured
  revision agreement, repeated logical-occurrence collapse, and separately
  visible embedded/external inventory rows.
- The same test verifies native type, role, dimensions, full SHA-256 identity,
  raw source paths, and the absence of mutation buttons.
- Existing Properties field-edit and full sanitizer suites remain green.

## Revisit when

- a specifically selected inventory item gains a bounded pixel preview;
- Resolved by ADRs 0078–0079: immutable native-FLAC replace/remove plans and
  journaled publication/recovery are specified, while Properties controls
  remain the next UI gate;
- shared external-artwork reads across many album tracks need measured batch
  caching;
- non-FLAC embedded artwork adapters are qualified.
