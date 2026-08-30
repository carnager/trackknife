# ADR-0046: Metadata recovery history and bounded retained-backup undo

- Status: accepted
- Date: 2026-08-29
- Owners: Trackknife project
- Extends: ADR-0044 journaled native-FLAC commit/recovery and ADR-0045
  dependent-state refresh

## Context

ADR-0044 deliberately retained the old inode after a successful native-FLAC
metadata publication, but did not decide how long backups live, how users see
startup recovery or ambiguous evidence, or how an undo updates every persisted
and visible occurrence. Exposing Apply before those answers would make the
first reachable file mutation consume unbounded storage and leave recovery as
headless behavior.

Undo cannot be a blind rename. A later external edit, missing backup, changed
hard-link topology, crash during restore, or dependent-state failure must not
overwrite newer data or leave Trackbench's lists inconsistent with the file.
It also needs a new idempotency identity: replaying the original commit identity
would make ADR-0045's cache transaction treat the reverse refresh as an old
no-op.

## Decision

- Reversible migration 8 adds a backup-lifecycle record separate from the main
  mutation state. Transitioning a metadata journal from `published` to
  `complete` atomically creates a `retained` backup record. Its states are
  `retained`, `undoing`, `undone`, `released`, and
  `needs_reconciliation`. This separation ensures ordinary commit recovery
  never mistakes an intentional undo artifact for an unfinished publication.
- Trackbench starts recovery only after its list/source-cache persistence is
  initialized. Recovery, metadata rereads, backup maintenance, undo, and
  release run on a background worker. The existing serialized persistence
  service performs only the short idempotent dependent-state transaction; the
  UI thread receives small completed snapshots and updates every in-memory
  duplicate together.
- The Metadata operations workspace shows the escaped raw source path, state,
  completion time, change count, retained bytes, journal identity, and failure
  evidence. It opens automatically when startup finds evidence needing
  attention. Ambiguous records offer no destructive resolution command; the
  evidence remains available for later reconciliation tooling.
- A retained backup is a single-step undo. Undo requires the source to match
  the recorded published revision and the backup to match the exact previous
  revision, both as direct single-link regular files. It records a fresh undo
  identity before exchanging the two directory entries. The restored file is
  reread and its original planned fields verified, then ADR-0045's
  all-occurrence transaction runs with the reverse revision pair and the new
  identity. Only after that succeeds is the replaced publication removed and
  the lifecycle marked `undone`. Redo is not implied.
- An interrupted `undoing` record is recoverable. A published-source/old-backup
  pair resumes the exchange; an old-source/published-backup pair resumes the
  reread, dependent-state replay, and cleanup. A failure before dependent state
  is committed exchanges the source back and returns to `retained`; identities
  that cannot be proved become `needs_reconciliation` without deleting either
  candidate.
- Users may explicitly Delete backup after confirmation. The release path
  locks and verifies the recorded old inode before unlinking it. A missing
  executor-owned backup completes release idempotently; an unexpected inode or
  topology becomes reconciliation evidence.
- Automatic maintenance runs after startup recovery, not immediately after a
  commit. The initial fixed policy keeps backups for at most seven days, keeps
  only the newest completed operation for one exact raw source path, and keeps
  at most 256 backups and 10 GiB of recorded old-file sizes globally,
  newest-first. This guarantees the new commit remains undoable for the rest of
  its first process session. A changed/missing published source or ambiguous
  backup is never deleted to satisfy a budget; it becomes visible
  reconciliation evidence instead.
- The Properties write-plan dialog remains preview-only. This slice removes
  the recovery/retention blocker but does not substitute for the cancellable,
  bounded Apply job and its per-source progress/error presentation.

## Alternatives considered

### Retain every backup until the user deletes it

Rejected. Metadata-only edits can retain nearly the full size of every source;
an indefinite default silently doubles collection storage over time.

### Keep only a time limit

Rejected. A large batch near the same time can exceed a reasonable operation
count or disk budget before age-based cleanup applies.

### Undo by copying the backup over the source

Rejected. Copying introduces partial-write and filesystem-metadata failure
surfaces already avoided by the atomic publication design. Exchanging the
recorded directory entries retains a reversible candidate until dependent
state succeeds.

### Mark undo complete before refreshing lists and caches

Rejected. A crash would leave a restored file with stale durable presentation
state and no pending operation to replay.

### Automatically delete a backup when the source changed externally

Rejected. The old inode may be the only trustworthy recovery evidence. Storage
budgets do not authorize deleting ambiguous user data.

## Consequences

- Native-FLAC commits now have a bounded, visible, crash-recoverable single-step
  undo story suitable for the later Apply job.
- Retention counts recorded old-file sizes rather than filesystem allocated
  blocks. This is deterministic and conservative enough for the initial policy,
  but sparse/reflink-aware accounting may improve it later.
- Terminal journal/history rows remain small durable evidence even after their
  file backup is released. A later operation-history retention policy may
  compact those records independently without affecting file recovery.
- Reconciliation is presentation-only in this slice. Trackbench does not guess
  which ambiguous inode should win.

## Validation

- Migration and journal tests cover atomic retained-record creation, guarded
  lifecycle transitions, undo identity persistence, and restart reload.
- Real-FLAC tests cover byte-exact undo, original-field reread, reverse revision
  delivery, backup cleanup, interrupted post-exchange recovery, and explicit
  budget-driven release.
- Offscreen Trackbench coverage proves startup history loading, the policy
  summary, retained-backup undo through the real persistence callback, coherent
  in-memory row refresh, byte-exact source restoration, and automatic
  reconciliation presentation with undo/release disabled.

## Revisit when

- the cancellable multi-source Apply job defines batch-level retry and partial
  success semantics;
- settings expose retention policy customization;
- reconciliation tooling can compare/export ambiguous candidates safely;
- rename/move/copy operations reuse the history and undo surface.
