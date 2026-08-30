# ADR-0045: Provenance-aware all-occurrence metadata cache transactions

- Status: accepted
- Date: 2026-08-29
- Owners: Trackknife project
- Extends: ADR-0033 typed local metadata and ADR-0044 journaled native-FLAC
  commit

## Context

ADR-0044 requires one idempotent, all-or-nothing dependent-state commit after a
physical metadata publication. A physical source may appear in several
Trackbench tabs and several logical rows, including CUE tracks and container
chapters. Every occurrence must show the committed embedded metadata without
losing its logical-track overlay.

The migration-6 list snapshot stored only effective field names and values.
That was sufficient for fast restart display, but it discarded provenance. A
blind replacement would therefore turn a CUE title into the file-level title.
Updating only the selected occurrence would leave duplicates stale. Updating
ordinary list rows alone would also race the serialized, debounced workspace
save: a snapshot captured before commit could overwrite the new values after
the dependent callback returned.

## Decision

- Reversible SQLite migration 7 adds native field names, provenance, and
  qualifiers to persisted list-item fields. Trackbench now stores every
  metadata layer instead of flattening each row to effective values. These
  remain presentation caches and never become mutation authority.
- A durable local-source metadata cache is keyed by the exact raw path BLOB.
  It stores the previous and verified published revisions plus embedded fields
  from the executor reread. List items retain their observed revision as stale
  evidence. It is separate from replace-all workspace snapshots, so a delayed
  older list save from the previous revision cannot erase a completed source
  refresh; a snapshot freshly observed at a third revision is not shadowed.
- `ListRepository::refresh_local_metadata` runs one `BEGIN IMMEDIATE`
  transaction. It finds every local list item with the exact physical source,
  replaces cached/embedded/stream layers in all occurrences, retains
  annotation/segment/sidecar layers, updates the source cache, and records the
  operation identity before commit. No matching occurrence is an error rather
  than an apparently successful partial commit.
- A completed operation identity and published revision make recovery replay a
  no-op. Reusing an identity for another source or revision is a conflict.
- Loading lists composes the durable source cache over item snapshots. This is
  the protection against a stale debounced `replace_all`, and it also restores
  the committed view after a crash between the SQLite callback and UI repaint.
- Migration cannot reconstruct provenance from legacy flattened logical rows.
  A matching CUE/chapter/subsong row that still contains
  `cached_snapshot` fields blocks commit until it has been freshly probed and
  saved. Guessing which values were embedded versus logical would risk silent
  metadata loss.
- `LocalListModel` applies the same source-layer replacement to every matching
  in-memory row and resets group geometry once, so title/album regrouping and
  all visible duplicate occurrences update coherently. The serialized
  persistence service exposes the blocking transaction only to a non-UI
  mutation worker.
- This slice does not expose Apply. Startup recovery/reconciliation,
  retained-backup undo/retention, and the bounded mutation job remain required
  by ADR-0044 before physical writes become reachable from Trackbench.

## Alternatives considered

### Replace every row with the verified physical document

Rejected. CUE, chapter, subsong, sidecar, and annotation layers have distinct
precedence and would be erased.

### Update only the occurrence indexes captured by Properties

Rejected. Those indexes describe the selected draft, not every duplicate
presentation of the physical source. All exact-path local occurrences must
refresh after the one physical write.

### Store new values only in each list item

Rejected. A workspace snapshot queued before commit could run afterward and
restore stale fields. A separate source cache provides a durable monotonic
layer while list tabs remain freely replaceable.

### Infer legacy CUE provenance by comparing values

Rejected. A logical override may equal the old embedded value, so equality
cannot identify its source. Reprobe is explicit and lossless.

## Consequences

- Duplicate tabs and logical tracks share one durable source truth while
  retaining their own overlays.
- List snapshots become slightly larger because they preserve layers and
  qualifiers, and the source cache duplicates the latest embedded projection.
- Source cache rows are not a library database: they exist only as verified
  presentation/recovery state for files already present in working lists.
- Future move/rename operations can reuse the same all-occurrence transaction
  pattern while changing the raw-path key as well as metadata.

## Validation

- Repository tests cover schema 7, raw-byte source keys, all-tab duplicates,
  CUE/segment overlay retention, qualifiers, unrelated-source isolation,
  idempotent replay, replay conflicts, stale workspace saves, externally
  refreshed revisions, sequential commits, restart, and conservative rejection
  of legacy flattened logical rows.
- Qt model coverage proves that every matching row refreshes together, album
  metadata changes while a CUE title remains effective, unrelated rows stay
  unchanged, revisions advance, and ambiguous cached logical rows do not
  mutate.

## Revisit when

- the cache gains bounded retention independent of retained operation backups;
- physical rename/move commits need to re-key source cache and list references;
- a future schema can retire the migration-6 legacy snapshot fallback.
