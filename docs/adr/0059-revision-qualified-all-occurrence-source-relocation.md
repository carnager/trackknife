# ADR-0059: Revision-qualified all-occurrence source relocation

- Status: accepted
- Date: 2026-08-30
- Owners: Trackknife project
- Extends: ADR-0045 provenance-aware metadata cache and ADR-0057 journaled
  same-filesystem file publication
- Partially superseded by: ADR-0082, which accepts an exact revision-matching
  occurrence already pre-resolved to the reviewed target

## Context

ADR-0057 publishes one physical source at a new raw path and requires an
idempotent, all-or-nothing dependent-state callback. A physical source can
occur in several local lists and several logical rows. Its verified metadata
cache is also keyed by the exact raw path. Updating only the occurrence indexes
captured by one operation would leave duplicates behind.

The ordinary workspace persistence path replaces all list documents from an
in-memory snapshot. A snapshot captured before publication may be delivered
after the dependent transaction. A one-time path update could therefore be
undone by a delayed save, just as a one-time metadata refresh was insufficient
in ADR-0045. A permanent unqualified old-path redirect is also unsafe because a
different physical file may later reuse that path.

## Decision

### One exact dependent-state transaction

- Reversible migration 15 adds `local_source_relocations`. Each row records a
  monotonic sequence, unique operation identity, exact raw source and target
  BLOBs, previous and published filesystem revisions, affected occurrence
  count, and whether the metadata cache was re-keyed.
- `ListRepository::relocate_local_source` uses one `BEGIN IMMEDIATE`
  transaction. It finds every local list occurrence with the exact source
  bytes, requires every occurrence to identify the captured previous revision,
  rejects an existing local target occurrence or target cache, updates every
  occurrence to the target path and published revision, re-keys the source
  cache and its fields when present, records the relocation, and commits.
- Missing revision evidence, a missing source occurrence, or mixed revisions
  blocks the complete transaction. The operation never guesses that an
  unprobed or changed row refers to the published file.
- MPD references use a separate authority and are not changed even when their
  URI bytes equal a local target path. Logical references, sample ranges,
  decoder selections, fields, annotations, and ordering remain unchanged.
- Replaying an operation identity with identical evidence is a no-op that
  returns the original result. Reusing it for different paths or revisions is
  a conflict.

### Revision-qualified stale-snapshot protection

- Loading or replacing list documents resolves local source relocations only
  when both the exact raw source bytes and captured revision match a durable
  record. A different file at a reused path is therefore not redirected.
- Records are replayed in increasing sequence. After each edge, both path and
  revision advance to the recorded target evidence. This makes an old A
  snapshot converge through A→B→C and also gives future copy publication a
  place to record a changed target device/inode.
- Resolution is bounded. Invalid rows, revision decoding failures, and an
  overlong chain fail explicitly instead of yielding a partial workspace.
- Relocation history is recovery/presentation evidence for sources already in
  working lists. It is not a media-library index or a claim of permanent path
  identity.

### Thread and visible-model boundary

- The serialized persistence worker exposes the relocation transaction only
  as a blocking call for a non-UI mutation worker, matching metadata commit.
- `LocalListModel` can apply one committed relocation to every matching visible
  duplicate while retaining logical identity and its current-row anchor. It
  applies the same revision and target-collision guards before resetting group
  geometry.
- Workspace Rename/Move remains disabled. The bounded job, active playback
  reconciliation, undo, cross-filesystem publication, and operation UI must
  consume this transaction before the capability is user reachable.

## Alternatives considered

### Update only the selected occurrence indexes

Rejected. Those indexes describe the operation input, not every duplicate or
logical presentation of the physical source.

### Delete the old path after one successful update

Rejected. A delayed replace-all workspace save could restore it after the
physical file has moved.

### Store one permanent old-path-to-new-path map

Rejected. Paths can be reused, and multiple moves or a future undo form an
ordered history rather than an immutable alias. Revision qualification and
monotonic replay preserve that distinction.

### Redirect rows without revision evidence

Rejected. Once the source name can be reused, raw path equality alone cannot
prove that an unprobed row names the published physical file.

## Consequences

- The ADR-0057 callback now has a concrete SQLite implementation and real-file
  integration coverage rather than only injected test committers.
- Every persisted duplicate and the metadata cache advance atomically, and
  stale debounced snapshots converge to the latest proved relocation.
- Relocation history grows until a later bounded compaction policy proves that
  no retained workspace or recovery evidence can reference an older edge.
- Same-filesystem undo and cross-filesystem verified-copy publication remain
  the next filesystem slices.

## Validation

- Repository tests cover raw non-UTF-8 paths, all-list duplicates, logical
  overlays, cache re-keying, MPD/local authority separation, exact replay,
  replay conflicts, stale replace-all saves, A→B→C replay, path reuse with a
  different revision, target collisions, and restart-safe schema migration.
- A real-file test connects `commit_same_filesystem_publication` to the actual
  repository callback and proves physical rename plus all-occurrence durable
  relocation and stale-save suppression.
- Qt model coverage proves duplicate/logical path advancement, revision
  advancement, unrelated-row isolation, and preservation of the current-row
  anchor.

## Revisit when

- combined tag-and-path publication needs more than one accepted predecessor
  revision for a snapshot;
- bounded relocation-history compaction is designed;
- sidecar, statistics, or library records add further path-keyed state.
