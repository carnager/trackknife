# ADR-0062: Revision-qualified active-playback relocation

- Status: accepted
- Date: 2026-08-30
- Owners: Trackknife project
- Extends: ADR-0021 serialized local audition, ADR-0059 revision-qualified
  all-occurrence source relocation

## Context

Same- and cross-filesystem publication already advance every persisted local
list occurrence and metadata-cache key before completion. Trackbench can still
have the published source open in its current decoder, prepared as the next
gapless decoder, or waiting in the local-audition command queue. Leaving those
process-local references at the source name makes the player disagree with the
workspace after Rename, Move, recovery replay, or undo.

An open Linux file descriptor remains usable when its directory entry is
renamed or unlinked. Reopening the decoder would therefore create an avoidable
transport discontinuity and could lose an exact subsong, segment, position,
buffer, or output state. Raw path equality alone is not enough to update the
binding because a different file can later reuse the name.

## Decision

### Revision-bearing decoder bindings

- Local audition observes a `LocalSourceRevision` before opening each current
  or gapless-next decoder. It confirms the path still exposes the same revision
  after open. A changed identity is a conflict; a path that disappeared only
  after a successful open is accepted because the decoder already owns the
  observed file descriptor.
- Published audition snapshots carry the revision beside both `raw_path` and
  `next_raw_path`. A gapless takeover advances path, revision, selection, and
  segment together at the consumed chain boundary.
- Load, clear, stop, seek, output suspension, buffer-policy changes, rejected
  continuations, and source replacement clear the matching revision whenever
  they clear a binding. A path is never presented with stale revision evidence.

### Serialized relocation barrier

- `relocate_source_and_wait` accepts exact source and target raw paths plus the
  observed source and published target revisions. It is a synchronous barrier
  for the non-UI mutation worker and serializes with decoder and transport work
  on the existing local-audition worker.
- Already queued load and gapless-next intents naming the source are rewritten
  under the command mutex before the barrier is appended. They carry the
  published target revision and refuse a replaced target when they execute.
- The worker changes a current or prepared-next binding only when both its raw
  source path and captured source revision match. A binding already at the
  exact target revision is an idempotent no-op. The same path with a different
  revision is left untouched and reported as a conflict.
- Re-keying changes only path/revision bookkeeping. The current and prepared
  decoders, PCM ring, PipeWire stream, position, play/pause state, selection,
  segment, and gapless timeline are not reopened, flushed, sought, or
  reconnected. This applies equally when cross-filesystem publication later
  unlinks the original inode.
- The result reports current, prepared-next, and queued-intent changes,
  revision conflicts, and exact replay. An unrelated or empty player is a
  successful no-op so startup recovery can reuse the same callback.

### Dependent-state composition

- `commit_source_relocation_and_wait` is the publication-callback boundary. It
  runs the audio barrier first and then the existing idempotent durable
  list/cache relocation while the target binding is visible.
- If durable relocation fails, it applies the exact inverse audio relocation
  before returning the original failure. The file-publication executor can
  then restore or remove the target according to its journaled rollback path
  without leaving a process-local player binding at that target.
- Recovery or journal replay invokes the same sequence. Audio replay is a
  no-op when the target revision is already bound, and the durable operation ID
  retains ADR-0059 idempotency. Local audition itself gains no persistent
  journal: after process restart there is no decoder binding to recover.
- Both synchronous calls reject use on the UI thread by contract. The lower
  barrier also rejects waiting on its own audio worker. Ordinary transport
  commands remain bounded, asynchronous UI calls.

## Alternatives considered

### Reopen the decoder at the target path

Rejected. It would quiesce output and reconstruct state for a directory-entry
change even though the existing descriptor remains exact and readable.

### Update bindings by path alone

Rejected. A stale player binding or a different file at a reused path must not
be redirected to the newly published file.

### Commit persistence before updating audio

Rejected. Cross-filesystem publication removes the source immediately after
the dependent callback succeeds. Updating audio first gives persistence
failure a reversible process-local compensation while both safe filesystem
states are still controlled by the executor.

### Persist playback relocation in SQLite

Rejected. Decoder and PipeWire state are process-local. Durable path history
already belongs to ADR-0059; after restart there is no old player object to
repair.

## Consequences

- Current and gapless-prepared local playback follow successful publication,
  recovery replay, and same-filesystem undo without a transport restart.
- File-publication callbacks must use the composed boundary when Trackbench's
  local player is alive. ADR-0063 provides the bounded job boundary; workspace
  wiring must supply the composed callback and apply committed results to
  visible list models.
- Commands already admitted before the barrier follow publication. New user
  commands remain the responsibility of the operation coordinator and current
  visible model; the UI must not submit a stale pre-publication snapshot as a
  new play intent while its mutation is committing.
- This decision does not define cross-filesystem undo, multi-source admission,
  combined tag/file publication, or workspace controls.

## Validation

- The local-audition integration test renames a real current WAV while its
  decoder is open and proves exact path/revision advancement without changing
  format, selection, segment, or resetting position.
- Exact callback replay is a no-op; stale source evidence and a mismatched
  target revision are reported without redirecting the binding.
- A real prepared gapless-next decoder follows its file rename without being
  reopened and retains the published target revision.
- An injected durable callback failure sees the target binding and then proves
  exact inverse audio compensation before the simulated filesystem rollback.

## Revisit when

- the bounded file job defines its UI-command admission barrier;
- local playback becomes restart-resumable;
- conversion publishes a replacement whose decoded timeline is not guaranteed
  equivalent to the source.
