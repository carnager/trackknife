# ADR-0044: Journaled native-FLAC metadata commit and recovery

- Status: accepted
- Date: 2026-08-29
- Owners: Trackknife project
- Extends: ADR-0042 revalidated write plans and ADR-0043 prepared-copy FLAC
  writer

## Context

ADR-0043 can produce a preservation-verified FLAC copy without touching the
source. Publishing that copy is a separate risk boundary. A safe commit must
serialize writers, revalidate again, retain recoverable evidence before the
first mutation, publish atomically, verify the published source, update every
dependent Trackbench occurrence, and make interrupted states recoverable.

The repository has no operations module or operation-journal persistence yet.
Putting these responsibilities in the Properties dialog would couple file
mutation to Qt and make later rename, artwork, ReplayGain, and conversion paths
invent incompatible recovery schemes.

## Decision

- A new Qt-free `operations` library owns mutation execution and recovery. It
  depends on typed metadata plans/adapters but not on UI or SQLite. A journal
  interface is part of the operation domain; the persistence library implements
  it in the existing state database through explicit reversible migration 6.
- One journal record covers one physical source and stores a versioned operation
  kind, immutable raw source/prepared/backup paths, expected revision, all
  affected occurrence indexes, field indexes, exact physical property names,
  original/planned ordered values, and logical intent indexes. State transitions
  add prepared/published revisions and structured failure evidence.
- Journal states are `planned`, `prepared`, `published`, `complete`,
  `rolled_back`, and `needs_reconciliation`. SQLite uses `FULL` synchronous
  durability and transactional inserts/transitions. Invalid or stale transitions
  fail instead of silently rewriting history.
- The first executor accepts only a ready `taglib-flac-v1` source plan whose
  exact path is a direct regular file with one hard link. Symlink and preexisting
  hard-link semantics remain blocked. This avoids replacing a symlink itself or
  silently splitting one name away from an unselected inode alias.
- Writers serialize by expected device/inode inside the process and hold
  advisory locks on both the old source inode and the prepared inode across
  publication. Lock waits are cancellation-aware. Revision and topology are
  checked after locking and again by the prepared-copy adapter.
- Prepared and backup names are hidden UUID siblings of the source, keeping all
  publication steps on one filesystem without interpreting raw path bytes. The
  complete journal is durable before creating either path.
- Before publication, the executor copies and verifies ownership, mode, and all
  bounded Linux extended attributes from source to prepared output, fsyncs the
  prepared file, and transitions the journal to `prepared`. Unsupported or
  uncopyable filesystem metadata blocks while the source remains unchanged.
- A hard link at the backup path retains the exact old inode. Renaming prepared
  over source is the single atomic publication step. Directory fsync follows
  backup creation, publication, rollback, and cleanup.
- The published source is reread and must match the prepared revision/document
  and every planned result. The executor then invokes a caller-supplied
  dependent-state committer with the new document/revision and every affected
  occurrence. That committer must be idempotent and all-or-nothing because
  recovery may replay it after a crash. Only its success permits the journal to
  become `complete`.
- A successful commit retains the hidden backup named by the journal as the
  initial undo/recovery policy. Automatic retention limits and a visible undo
  command are deferred, so the Trackbench UI still exposes no Apply action.
- Cancellation is honored while waiting, revalidating, preparing, and verifying.
  Once atomic publication begins, the executor reaches either verified complete
  state or rollback before returning.
- Failure before publication removes only executor-owned prepared/backup paths
  and records `rolled_back`. Failure after publication atomically exchanges the
  old backup back into the source path when Linux `renameat2(RENAME_EXCHANGE)`
  is available, with a conservative two-rename fallback. If exact restoration
  or cleanup cannot be proven, the journal becomes `needs_reconciliation` and
  no uncertain path is deleted.
- Recovery loads nonterminal records under the same source lock. A source still
  at the expected revision rolls back safe leftovers. A source at the recorded
  prepared/published revision with the expected backup is reread, replays the
  dependent-state committer, and completes. Any ambiguous identities or missing
  recovery evidence become `needs_reconciliation`.

## Alternatives considered

### Save in place and record only an error log

Rejected. An error log cannot restore the original inode after a partial or
corrupt write and cannot distinguish pre-publication from post-publication
failure.

### Store journals as ad-hoc files beside each track

Rejected. SQLite already provides transactional durable state, bounded parsing,
and one recovery query. Only the prepared and backup audio paths need to live on
the source filesystem.

### Delete the backup immediately after verification

Rejected. Crash recovery would work, but the first destructive metadata commit
would have no usable undo artifact. Retention policy must be explicit before
automatic cleanup ships.

### Update list snapshots after marking complete

Rejected. That would allow a complete journal while Trackbench still presents
stale metadata. Dependent state is part of the logical commit and is replayable
during recovery.

### Support symlinks and hard links immediately

Rejected. Replacing one directory entry has different semantics from rewriting
the shared inode. The preview must not choose between those outcomes silently.

## Consequences

- The filesystem executor is reusable by a later UI and job scheduler without
  letting either own tag or recovery logic.
- Completed operations deliberately retain hidden sibling backups, consuming
  the old file's storage until an explicit undo/retention slice lands.
- Metadata commits can update all duplicate/CUE/chapter occurrences through one
  callback while the physical write still occurs once.
- Cross-process locking is cooperative; an unrelated program that ignores
  advisory locks can still race, so revision and preservation verification
  remain mandatory.
- Apply remains absent until Trackbench supplies the real dependent-state
  transaction, progress/cancellation presentation, retained-backup management,
  and recovery UI.

## Validation

- SQLite tests cover migration 6, exact raw-byte plans and ordered values,
  terminal/nonterminal reload, legal optimistic transitions, and rejection of
  stale or invalid transitions.
- Real-FLAC tests cover successful atomic publication, retained byte-identical
  backup, post-publication metadata/PCM validity, source topology and revision
  conflicts, cancellation while waiting for a source lock, concurrent
  serialization, dependent-state failure rollback, injected journal-transition
  failure rollback, and filesystem-metadata preservation.
- Recovery tests cover pre-publication cleanup, completion of an interrupted
  valid publication, replay of dependent state, and ambiguous-state escalation
  without deleting uncertain files.

## Revisit when

- Properties wires the executor into Apply and real list/cache persistence;
- backup retention and visible undo are designed;
- symlink/hard-link policy is chosen;
- batch journals need all-or-nothing versus per-source partial semantics;
- rename/move/copy and artwork operations reuse the lifecycle.
