# ADR-0060: Journaled same-filesystem publication undo

- Status: accepted
- Date: 2026-08-30
- Owners: Trackknife project
- Extends: ADR-0057 journaled same-filesystem publication and ADR-0059
  revision-qualified source relocation

## Context

A completed path-only publication has moved the only direct source name,
committed every dependent reference, and discarded no file bytes. Undo should
restore the original name, but it is still a destructive filesystem mutation:
the original name may have been reused, the published target may have changed,
and a crash can occur between reverse publication, dependent-state relocation,
and journal completion.

Changing the completed original record back into an active state would erase
the fact that the forward operation completed. A standalone reverse operation
without a durable relation would make repeated undo ambiguous and prevent the
history UI from distinguishing an undo from another ordinary rename.

## Decision

### Undo is a related reverse publication

- Reversible migration 16 adds nullable `reverses_id` evidence and an index to
  the file-publication journal. An undo attempt is a new same-filesystem record
  with its own operation identity, source/target paths swapped, and a relation
  to the completed original.
- The journal accepts a reversal only when its parent exists, is a completed
  original same-filesystem publication, is not itself a reversal, exposes the
  reverse source revision, has exactly inverse raw paths, and has the same
  ordered occurrence indexes. Reverse records never invent missing-directory
  evidence.
- Rolled-back attempts remain in history and permit a fresh reverse record.
  A completed reversal makes another request idempotently return that result.
  A nonterminal or reconciliation attempt blocks another request until startup
  recovery or manual reconciliation resolves it.

### Exact reverse execution

- `undo_same_filesystem_publication` accepts only a completed original record.
  Before creating reverse evidence it opens and locks the published target,
  requires its complete recorded revision and single-link direct regular-file
  topology, reopens the original parent without symlinks, requires matching
  filesystem and write/search access, and refuses an occupied original name.
- The original parent must still exist. Undo does not recreate a directory
  removed by another actor and never removes directories created by the
  forward publication.
- The reverse record is durable before Linux
  `renameat2(RENAME_NOREPLACE)` moves the target. Both parent entries are
  synced and the exact restored topology is verified.
- The reverse `FilePublicationCommitResult` carries the undo record identity,
  published target as source, original path as target, and the original
  occurrence indexes. ADR-0059 consumes it as an ordinary B→A relocation, so
  stale workspace snapshots converge through ordered A→B→A history.
- A dependent-state failure moves the exact file back to the forward target
  and marks the attempt rolled back. Once the reverse callback succeeds, a
  journal-transition failure retains the restored original path and normal
  startup recovery replays the idempotent reverse callback.

### Recovery and reachability

- Reverse records use the existing same-filesystem states and are processed by
  `recover_same_filesystem_publications`; no second recovery state machine is
  introduced.
- Changed revisions, hard links, symlinks, occupied names, both/neither path
  identities, and callback/rollback ambiguity never delete either path.
- Undo is not exposed in the workspace yet. Cross-filesystem verified-copy
  publication, active-playback reconciliation, bounded jobs, and operation
  history UI remain required before Rename/Move becomes user reachable.

## Alternatives considered

### Reopen the completed forward journal record

Rejected. It destroys the immutable forward history and makes recovery unable
to tell whether a state belongs to publication or undo.

### Rename back without another journal

Rejected. A crash after the physical reverse but before list/cache relocation
would leave the same split-brain state ADR-0057 was designed to prevent.

### Overwrite a reused original path

Rejected. Undo restores one exact name only when that name is absent; it never
authorizes deletion or replacement of a later file.

### Remove directories created by the forward operation

Rejected. Another actor may have adopted them since publication. Leaving empty
directories is harmless and preserves the conservative ownership boundary.

## Consequences

- Completed path-only same-filesystem operations now have byte-preserving,
  crash-recoverable, idempotent undo in the core.
- Forward and reverse operations remain independently auditable, while the
  relation supports a future history surface and retry policy.
- The same all-occurrence path transaction handles forward publication and
  undo without special-case cache or list logic.
- Cross-filesystem copy/move needs a distinct undo policy because restoring its
  source may require another verified copy rather than a directory-entry
  rename.

## Validation

- Real-file tests cover successful reverse publication, exact callback
  evidence, completed idempotency, occupied-original refusal before journaling,
  dependent-state failure rollback, retry through a second reverse record, and
  journal failure after callback success followed by startup replay.
- SQLite tests prove relation round trip, parent existence, inverse completed
  evidence, and bounded reverse lookup.
- The real repository integration test proves physical A→B→A plus every
  persisted duplicate and stale workspace snapshot converging back to A.

## Revisit when

- cross-filesystem publication defines restore-versus-copy undo semantics;
- operation history exposes retention/release controls;
- redo is designed as a user-facing operation rather than inferred by undoing
  an undo record.
