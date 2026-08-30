# ADR-0057: Journaled same-filesystem file publication

- Status: accepted
- Date: 2026-08-30
- Owners: Trackknife project
- Extends: ADR-0056 fresh file preflight and publication journal

## Context

ADR-0056 records enough evidence to distinguish same-filesystem rename from
cross-filesystem copy, but deliberately does not mutate files. The first
executable path slice needs to prove the shortest publication state machine
without weakening target-collision, source-identity, cancellation, dependent-
state, or crash-recovery guarantees.

An atomic directory-entry rename is not the whole transaction. The original
path disappears before Trackbench's durable list/cache/playback references can
advance. A dependent-state failure must therefore restore the original path,
while a crash after the dependent transaction succeeds must replay that
idempotent transaction rather than roll the file back behind already committed
references.

## Decision

### Descriptor-relative same-filesystem execution

- `commit_same_filesystem_publication` accepts exactly one ready ADR-0056
  preflight source classified as `same_filesystem_rename`. No-change and cross-
  filesystem sources are rejected at this boundary.
- Before journal creation it opens and locks the direct source, verifies a
  regular single-link descriptor/path identity and the complete captured
  revision, re-walks the target parent from `/` with `O_NOFOLLOW`, requires the
  exact previously absent directory chain, repeats device and access checks,
  and refuses an occupied target. These failures cause no journal or filesystem
  mutation.
- The exact ADR-0056 `planned` record is durable before directories are
  created. Planned directories are then created outermost-first with
  `mkdirat`, reopened with `O_NOFOLLOW`, and their parent entries are synced.
  A directory that unexpectedly appears is a conflict. Rollback leaves created
  empty directories in place because another process may have begun using
  them.
- Immediately before publication, source descriptor/path identity and target
  absence are checked again. Linux `renameat2(RENAME_NOREPLACE)` performs the
  atomic publication; there is no overwriting `rename` fallback. Source and
  target parent directories are synced and the target must expose the original
  source revision while the source name is absent.
- A target path that already aliases the source on a case-folding filesystem is
  explicitly unsupported by this first executor. A plain rename cannot both
  change only the stored spelling and provide `RENAME_NOREPLACE`'s conditional
  publication guarantee. Case-only changes on case-sensitive filesystems still
  use the normal absent-target path.
- Cancellation is honored while waiting for the file lock and until the atomic
  rename. Once a target is published, the executor reaches a safe terminal or
  recoverable journal boundary rather than abandoning the transaction.

### Dependent state and rollback boundary

- The executor supplies an immutable `FilePublicationCommitResult` containing
  the journal ID, raw source/target paths, source/target revisions, and all
  logical occurrence indexes to an injected dependent-state committer.
- That committer must be idempotent and all-or-nothing. Until it reports
  success, any failure renames the exact target identity back to an absent
  source with `RENAME_NOREPLACE`, syncs both parents, verifies the restored
  topology, and advances the journal to `rolled_back`.
- After the committer reports success, a journal-transition failure leaves the
  physical target and `target_published` evidence intact. Recovery replays the
  idempotent committer. Once `dependent_state_committed` is durable, recovery
  completes without replaying it.
- Ambiguous identity, topology, or rollback evidence advances to
  `needs_reconciliation`; neither path is deleted automatically. Terminal
  journal errors persist code/message without transient presentation context.

### Startup recovery

- `recover_same_filesystem_publications` processes only incomplete same-
  filesystem records. The future cross-filesystem executor owns its distinct
  prepared-copy and source-removal states.
- A `planned` record with the exact source and absent target becomes a terminal
  rolled-back operation. A `planned` record with an absent source and exact
  target proves that rename occurred before its journal transition; recovery
  records `target_published` and resumes the dependent commit.
- `target_published` verifies and replays dependent state; callback failure
  restores the original path. `dependent_state_committed` verifies the target
  and advances directly to `complete`.
- Both paths present, both absent, unexpected revisions, hard links, symlinks,
  or changed components are reconciliation cases. Recovery first locks the one
  unambiguous physical file and then repeats topology observation.

The actual all-occurrence list/cache/playback relocation transaction, undo,
cross-filesystem copy verification, bounded batch scheduling, and workspace
enablement remain separate slices. This executor is available to those layers
but is not itself a user-visible Rename or Move capability.

## Alternatives considered

### Rename first and create journal evidence afterwards

Rejected. A crash between those actions would leave no durable source/target
intent or occurrence set from which to reconcile dependent references.

### Fall back to ordinary overwriting rename

Rejected. An appeared target must never be silently replaced. Filesystems or
kernels without atomic no-replace publication report an unsupported operation.

### Roll back after the dependent callback succeeds but its journal transition fails

Rejected. The callback may already have durably moved every Trackbench
reference. Replaying an idempotent transaction is safe; moving the file behind
those committed references is not.

### Remove executor-created empty directories during rollback

Rejected as in ADR-0056. The journal proves that Trackbench planned their
creation, not that no other process subsequently populated or adopted them.

## Consequences

- Path-only same-filesystem publication now has a real crash-recoverable core
  and failure-injection coverage.
- The state callback is the only bridge needed by the next persistence slice;
  operations code remains independent of SQLite list schemas and Qt.
- Cross-filesystem Move, path-aware dependent persistence, undo, batching, and
  UI choices remain disabled and cannot inherit an incomplete capability claim
  from this slice.

## Validation

- Real temporary-file tests cover nested planned-directory creation, exact byte
  preservation, callback ordering, source/target evidence, and terminal
  completion.
- Injected dependent-state failure and injected publication/dependent journal
  failures cover safe rollback and idempotent replay.
- Startup tests cover a journal before rename, rename before its journal
  transition, callback failure during recovery, already committed dependent
  state, and ambiguous occupied targets without deletion.
- Changed sources, appeared targets, and pre-commit cancellation prove the
  no-journal/no-mutation boundary.

## Revisit when

- `openat2` resolution constraints can strengthen mount/topology handling;
- a safe conditional case-only rename primitive is available;
- companion files require multi-entry atomicity or a publication graph.
