# ADR-0079: Journaled native-FLAC artwork publication

- Status: accepted
- Date: 2026-09-01
- Owners: Trackknife project
- Extends: ADR-0044 journaled metadata publication, ADR-0045
  all-occurrence metadata refresh, ADR-0046 retained-backup lifecycle, and
  ADR-0078 immutable native-FLAC artwork write plans

## Context

ADR-0078 produces a preservation-verified native-FLAC artwork copy but stops
before replacing the user's source. Publishing that copy creates the same
crash boundary as a text-tag write: the directory entry can contain the new
inode while the operation state or dependent Trackbench rows still describe
the old revision. Volatile session state cannot distinguish a clean rollback,
an already-published result, and an ambiguous external change after restart.

The existing metadata journal records text changes in detail. Artwork recovery
needs different proof, but SQLite must not become an artwork library, image
backup, or competing metadata authority.

## Decision

- Reversible migration 23 adds an explicit `content_kind` to the existing
  metadata-operation journal and one keyed artwork-evidence row. Text records
  keep their existing change rows. Artwork records instead retain intent kind,
  target ordinal, original and planned item counts, original target SHA-256,
  optional replacement SHA-256, and original and planned complete-inventory
  SHA-256 digests.
- The inventory digest is version/domain-separated and covers every ordered
  embedded picture's ordinal, role, exact native type, MIME type, description,
  dimensions, encoded size, and content SHA-256. It excludes raw path, inode
  revision, and derived duplicate annotations so atomic publication can be
  verified at its new identity. Verifying only the changed target is
  insufficient because it would not prove preservation of unrelated pictures.
- SQLite stores only these small operation and recovery facts. It stores no
  encoded image, decoded pixel data, replacement payload, inventory row, or
  durable artwork cache. This evidence survives restart specifically because
  a process can stop between filesystem publication and the idempotent
  all-occurrence state transaction.
- One shared unchanged-path publication helper now serves text metadata and
  embedded artwork. It locks and revalidates the exact source, creates the
  journal before the prepared artifact, preserves filesystem metadata, syncs
  the prepared file, retains the old inode, atomically replaces the source,
  rereads the published content, commits dependent state, and only then marks
  the operation complete.
- Artwork admission re-inventories the source while its mutation lock is held
  and projects the planned complete-inventory digest before creating the
  journal. The prepared-copy result and the published source must both match
  that digest and the unchanged text document.
- Startup recovery uses recorded filesystem revisions plus the planned
  complete-inventory digest. A proved publication replays the same idempotent
  dependent-state callback; a proved original removes owned preparation
  debris and rolls back; identities or content that cannot be proved become
  explicit reconciliation evidence.
- The dependent result carries the physical source path, old and new
  revisions, verified unchanged text document, and every logical occurrence
  index. ADR-0045's existing transaction therefore updates all duplicate,
  CUE, and chapter occurrences and the source metadata cache even though an
  artwork-only change does not alter text fields.
- The existing retained-backup lifecycle applies unchanged. The backup is the
  old filesystem inode referenced by a journal path, not image data stored in
  SQLite; undo verifies the original complete-inventory digest before updating
  dependent rows. Existing explicit release and bounded retention policy remain
  the cleanup controls.
- This slice exposes the Qt-free single-source executor. ADR-0080 now adds
  Properties replacement selection, Replace/Remove review, bounded batch
  Apply, and truthful capability state.

## Alternatives considered

### Keep artwork recovery state only in memory

Rejected. A restart after atomic replacement would leave Trackbench unable to
prove whether it should complete dependent state or restore the original.

### Store complete artwork inventories or replacement bytes in SQLite

Rejected. That would duplicate user data, retain large payloads, and turn stale
database content into an artwork authority.

### Record only target and replacement fingerprints

Rejected. Those identities do not prove that unrelated pictures, ordering, or
descriptions survived publication.

### Build a separate artwork publication pipeline

Rejected. Text and artwork have the same filesystem crash states. Sharing the
proven lifecycle prevents their rollback and recovery rules from diverging.

### Publish without retaining the old inode

Rejected. Destructive metadata mutation requires a recoverable journal or undo
story. The existing hard-link retention is byte-exact, bounded, explicitly
releasable, and does not put media payloads in SQLite.

## Consequences

- Artwork operation evidence is durable across application restarts while
  artwork data remains file-owned.
- Native-FLAC replace/remove can now use the same conservative rollback,
  reconciliation, undo, and all-occurrence refresh guarantees as text writes.
- Schema version 23 is required before opening the operation journal.
- ADR-0080 qualifies the Properties batch review and Apply surface.

## Validation

- Persistence tests round-trip and reopen artwork kind/count/ordinal/hash
  evidence and reject structurally incomplete rows.
- A real multi-picture FLAC is published at its unchanged path with a real JPEG
  replacement. Tests prove the new revision, complete planned inventory,
  retained byte-exact old inode, every logical occurrence in the dependent
  callback, and exact artwork undo.
- A simulated crash after atomic replacement but before the published journal
  transition closes and reopens SQLite. Startup recovery rereads the real FLAC,
  proves the planned inventory digest, replays all-occurrence refresh, and
  completes the journal without storing image bytes.
- Existing text commit, rollback, recovery, undo, retention, and list-cache
  tests pass through the shared publication helper.

## Revisit when

- native-FLAC Add, Export, or Copy is specified;
- another container receives a fixture-backed picture writer and digest
  mapping;
- retained-backup policy becomes user-configurable rather than fixed bounded
  maintenance;
- add/export/copy or type/description editing is specified.
