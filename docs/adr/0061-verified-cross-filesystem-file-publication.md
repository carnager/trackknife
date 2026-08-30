# ADR-0061: Verified cross-filesystem file publication

- Status: accepted
- Date: 2026-08-30
- Owners: Trackknife project
- Extends: ADR-0056 fresh file preflight and publication journal, ADR-0059
  revision-qualified source relocation

## Context

A path-only Move cannot use one directory-entry rename when its source and
target are on different filesystems. Copying straight to the final name would
expose an incomplete file, while deleting the source before every Trackbench
reference advances could leave neither the player nor the workspace able to
find the only valid copy. Crashes can occur after a prepared copy, after target
publication, after the dependent-state transaction, or after source removal.

The state machine in ADR-0056 already records these boundaries. This decision
qualifies their executor and recovery behavior without yet making Move a
workspace action.

## Decision

### Bounded verified preparation

- `commit_cross_filesystem_publication` accepts one ready preflight source
  classified as `cross_filesystem_copy`. It opens and locks the exact direct,
  single-link source and repeats target topology, device, access, and absent-
  path checks before creating journal evidence.
- The exact `planned` record is durable before missing target directories or
  copy artifacts are created. Directories are created descriptor-relatively,
  reopened without following symlinks, and their parent entries are synced.
- The executor creates only the journal-derived
  `.trackknife-<UUID>.prepared` sibling with `O_EXCL|O_NOFOLLOW`. A bounded
  one-MiB buffer copies with positioned reads/writes and cancellation checks.
- The prepared file preserves and verifies source ownership, permission bits,
  access/modification timestamps, and bounded Linux extended attributes. An
  unsupported or uncopyable attribute fails while the original remains. The
  file is synced, locked, identity-checked, and compared byte-for-byte with the
  still-locked source before `target_prepared` becomes durable.
- A source larger than the platform's positioned-I/O offset range is rejected.
  Sparse extent shape is not preserved; logical file bytes and qualified
  filesystem metadata are the invariant.

### Publication and deletion boundary

- The prepared sibling is renamed to the absent final name with
  `renameat2(RENAME_NOREPLACE)` on the target filesystem and its parent is
  synced. The prepared path must be absent and the final target must expose the
  recorded prepared identity before `target_published` becomes durable.
- The executor sends source and target revisions, raw paths, operation ID, and
  all logical occurrences to the same idempotent all-or-nothing dependent-state
  committer used by same-filesystem rename. ADR-0059 therefore advances every
  persisted list occurrence and cache key before source deletion.
- Callback failure removes only the exact already-locked target and verifies
  the unchanged original. The runtime rollback uses the prepared descriptor
  after its rename rather than reacquiring its own advisory lock.
- Once `dependent_state_committed` is durable, the locked original path is
  identity-checked immediately before `unlinkat`; its parent is synced and the
  exact source-absent/target-present topology is verified before
  `source_removed → complete`.
- Cancellation removes an exact executor-owned prepared sibling before target
  publication. Once publication starts, execution reaches a safe durable
  boundary instead of abandoning two-path state.

### Startup recovery

- `recover_cross_filesystem_publications` processes only cross-filesystem
  records and never treats a same-filesystem journal as copy evidence.
- A clean `planned` record rolls back. An exact prepared sibling discovered
  before its journal transition is locked, metadata- and byte-verified, synced,
  adopted as `target_prepared`, and resumed. Unknown or differing prepared
  content is retained for reconciliation rather than deleted.
- `target_prepared` either publishes the recorded sibling or recognizes its
  exact identity already at the final name when a crash followed rename but
  preceded the transition. A missing prepared and target with an unchanged
  source is a completed safe cleanup and becomes `rolled_back`.
- `target_published` verifies both copies, replays the idempotent dependent
  transaction, and then resumes source removal. Callback failure removes the
  exact target while the source is still present.
- `dependent_state_committed` removes an exact remaining source or recognizes
  that removal already happened. `source_removed` verifies the final topology
  and completes without replaying dependent state.
- Changed revisions, unexpected hard links or symlinks, both prepared and
  target paths, an occupied final name, missing durable evidence, or any other
  ambiguous topology become `needs_reconciliation`; recovery never guesses
  which user-visible file to delete.

## Alternatives considered

### Copy directly to the final target

Rejected. Readers could observe partial bytes, and a crash would not distinguish
an incomplete copy from a complete published target.

### Delete the source before committing dependent state

Rejected. A database failure would leave every durable reference pointing at a
name that no longer exists. Temporarily retaining two verified copies is the
safe crash state.

### Verify only size and modification time

Rejected. Equal-length corruption would pass. The executor performs a complete
bounded byte comparison and verifies the filesystem metadata it promises to
preserve.

### Automatically delete an unrecorded prepared path during recovery

Rejected. Without a durable identity, a replaced path cannot be proven to be
the executor's incomplete file. Exact content may be adopted; differing
content remains visible for reconciliation.

## Consequences

- Path-only cross-filesystem publication now has the same journal-before-
  mutation, idempotent dependent-state, rollback, and conservative recovery
  guarantees as the qualified same-filesystem path.
- The ADR-0059 all-occurrence transaction works unchanged even though a copied
  target necessarily has a different device/inode revision.
- Cross-filesystem undo is still undefined: restoring the original may require
  another verified copy and a distinct retention policy.
- Active-playback reconciliation, bounded batch orchestration, operation
  history/undo policy, combined content publication, and workspace controls
  remain required before Rename or Move is user reachable.

## Validation

- Real files on distinct mounted device identities exercise a multi-buffer
  copy, exact callback ordering, changed target revision, preserved ownership,
  permissions and extended attributes, absent prepared sibling, source removal,
  and terminal journal evidence.
- Injected dependent failure proves that only the published target is removed
  and the original survives.
- An injected post-callback journal failure proves startup replay occurs before
  source removal.
- Recovery tests cover a target rename before its durable transition and source
  removal before its durable transition; both converge to `complete` without a
  destructive guess or duplicate callback.

## Revisit when

- cross-filesystem undo and redo retention are specified;
- sparse extent or reflink preservation is worth exposing as a policy;
- ACLs need a representation beyond the preserved Linux extended attributes;
- companion files require a multi-target publication graph.
