# ADR-0056: Fresh file preflight and publication journal

- Status: accepted
- Date: 2026-08-30
- Owners: Trackknife project
- Extends: ADR-0054 unified preparation plan and ADR-0055 pure path planning

## Context

ADR-0055 can map final metadata to deterministic source/target paths, but its
explicit snapshot is not authority to mutate the live filesystem. Rename and
move require a second observation that distinguishes an atomic same-filesystem
rename from cross-filesystem copy publication and rejects unsafe path topology
before any directory, copy, rename, or deletion occurs.

The existing metadata journal records prepared-copy replacement at one source
path. A move has different crash boundaries: a cross-filesystem target may be
prepared and published while the original still exists, dependent references
must advance, and only then may the source be removed. Reusing the metadata
state codes would silently reinterpret persisted evidence.

## Decision

### Fresh, non-mutating filesystem preflight

- A ready pure `OutputPathPlan` enters a second bounded Qt-free preflight. The
  preflight performs filesystem observation only: it never calls `mkdir`,
  copies bytes, renames, unlinks, or edits metadata.
- Every existing source-parent, operation-root, and target-parent component is
  opened from `/` with `O_NOFOLLOW`. A symbolic link at any component is a
  blocking issue. The source itself is opened with `O_NOFOLLOW` and must be a
  regular file with exactly one hard link.
- The freshly observed source revision must exactly equal the planner's
  captured device, inode, size, and nanosecond modification time. Changed,
  missing, non-regular, symlinked, or multiply linked sources block the complete
  physical source.
- Move requires an existing non-symlink directory root. Missing directories
  below that root are allowed, listed outermost-first in the immutable result,
  and not created by preflight. The nearest existing target parent must be
  writable and searchable; the source parent must be writable and searchable
  for every real path change.
- A newly occupied target blocks. An exact no-change target is classified as a
  no-op. A target that resolves to the same source identity is accepted only
  for the case-only warning already present in the pure plan.
- `_PC_NAME_MAX` and `_PC_PATH_MAX` from the nearest existing target directory
  may tighten the pure planner's limits. Failures are visible per-source
  blockers rather than a partial ready claim.
- Device identity of the source and nearest existing target parent classifies
  publication as code 1, `same_filesystem_rename`, or code 2,
  `cross_filesystem_copy`. Code 0 is `no_change` and never creates mutation
  journal evidence.

This preview is still not commit authority. The executor must repeat its
descriptor-relative observations while holding the physical-source lock.
Bind-mount topology and races after preflight remain executor concerns; this
slice claims complete symlink rejection, not mount-namespace confinement.

### A distinct persisted publication state machine

Reversible SQLite migration 14 adds `file_publication_journal`. Its state codes
are fixed persisted behavior:

| Code | State | Durable meaning |
| --- | --- | --- |
| 0 | `planned` | Exact source, target, expected source identity, occurrences, and absent-directory chain are durable; no file publication occurred. |
| 1 | `target_prepared` | A cross-filesystem executor-owned target sibling has been fully prepared and its identity recorded. |
| 2 | `target_published` | The target name is published and its identity recorded. For cross-filesystem work the source still exists; same-filesystem rename removed the source name atomically. |
| 3 | `dependent_state_committed` | Every durable Trackbench reference transaction points at the target. |
| 4 | `source_removed` | The cross-filesystem original has been identity-checked and removed. |
| 5 | `complete` | All required boundaries completed. |
| 6 | `rolled_back` | Safe rollback completed and failure evidence is retained. |
| 7 | `needs_reconciliation` | Filesystem identity is ambiguous; evidence remains visible and nothing is deleted automatically. |

- Same-filesystem rename advances `planned → target_published →
  dependent_state_committed → complete`.
- Cross-filesystem publication advances `planned → target_prepared →
  target_published → dependent_state_committed → source_removed → complete`.
- Cross-filesystem dependent state deliberately commits before source deletion.
  A crash may temporarily leave two verified copies, but never deletes the only
  copy before Trackbench can durably find the published target.
- Transitions use optimistic expected-state guards and carry the complete
  prepared/target revision evidence for their boundary. Illegal state/kind
  combinations, zero identities, stale transitions, and terminal failures
  without an error code/message are rejected before a write.
- The cross-filesystem prepared name is exactly
  `.trackknife-<journal UUID>.prepared` beside the target. Arbitrary temporary
  paths cannot be injected into recovery evidence.
- Logical occurrence indexes are sorted and unique. Planned missing-directory
  paths form one exact parent chain ending at the target parent. They are
  recovery evidence, not deletion authority: rollback may leave harmless empty
  directories rather than risk removing a directory another process populated.
- `complete` and `rolled_back` records do not replay. `needs_reconciliation`
  remains in the incomplete query so startup can present it.

The metadata-only journal remains unchanged for the already qualified native-
FLAC Apply path. A future path-enabled combined preparation uses the file-
publication state machine as the publication owner and must add its content
intent before the UI choices become usable; it must not run two unrelated
journals for one physical publication.

## Alternatives considered

### Treat the pure planner snapshot as fresh enough

Rejected. It cannot detect changes between planning and Apply and cannot
classify the actual target filesystem.

### Follow symlinks and retain only resolved strings

Rejected for the initial executor boundary. Resolved strings lose the
descriptor-relative topology needed to notice replacement races. Blocking
symlink components is conservative and explainable.

### Insert move states into the existing metadata state enum

Rejected. Existing numeric states are persisted and describe source-path
replacement, including a retained-backup lifecycle. Appending path states would
still overload fields and legal transitions with incompatible meanings.

### Delete newly created empty directories during rollback

Rejected for the first publication contract. A directory can acquire unrelated
content after creation. Leaving a harmless empty directory is safer than
claiming ownership after a crash.

## Consequences

- A path preview can now distinguish lexical readiness from current Linux
  filesystem readiness and show whether execution would be atomic rename or
  verified cross-filesystem copy.
- Every recovery boundary required by a path executor has explicit durable raw
  paths, identities, logical occurrences, and optimistic transitions.
- Rename and Move remain unavailable as usable workspace operations. The next
  slice must implement descriptor-relative directory creation, copying and
  verification, durable publication, rollback/recovery, dependent-state path
  reconciliation, and undo with injected-failure real-file tests.

## Validation

- Real temporary-file preflight tests cover unchanged same-filesystem
  classification, missing-directory reporting without mutation, revision
  changes, hard links, occupied targets, target-parent symlinks, no-op paths,
  and cancellation.
- Journal tests cover raw-byte restart round trips, exact missing-directory and
  occurrence order, same- and cross-filesystem legal state sequences, stale
  expected states, illegal kind/state combinations, terminal evidence, and
  visible reconciliation.
- Migration 14 must pass reversible 13→14→13 schema and preservation probes.

## Revisit when

- the executor can use `openat2` resolution flags on every supported kernel;
- bind-mount policy is qualified;
- combined metadata/ReplayGain content intent is attached to publication;
- companion files require a multi-target publication graph.
