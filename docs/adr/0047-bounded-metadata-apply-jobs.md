# ADR-0047: Bounded metadata Apply jobs and partial-result retry

- Status: accepted
- Date: 2026-08-30
- Owners: Trackknife project
- Extends: ADR-0042 revalidated write plans and ADRs 0044–0046 metadata
  commit, dependent-state, and recovery history

## Context

ADRs 0042–0046 established a complete path from an immutable preview through
one recoverable native-FLAC commit, durable dependent-state refresh, retained
backup history, and byte-exact undo. Properties nevertheless remained
preview-only. Making Apply reachable requires batch semantics that do not turn
a multi-file selection into an unbounded thread fan-out, hide runtime errors,
or imply that cancellation can reverse a source which has crossed its atomic
publication boundary.

A plan can be wholly ready and still encounter runtime failures: a source may
change after preview, storage may fill, a lock may fail, or dependent-state
persistence may reject the publication. Other independent sources may already
have committed by then. Treating the whole batch as one filesystem transaction
would be untruthful, while silently retrying the old plan would bypass the
fresh-revision guarantee.

## Decision

- Apply is offered only on a complete, immutable ADR-0042 plan whose every
  physical source is ready. A blocked preview has no Apply action. The click is
  the explicit commit boundary; draft editing never writes implicitly.
- Trackbench captures the current workspace on the UI thread at that boundary,
  then durably saves the snapshot on the persistence worker before admitting
  file mutations. This closes the ordinary debounced-save race and ensures the
  dependent-state transaction can find every current list occurrence without
  doing disk or SQL work on the UI thread.
- A Qt-free batch orchestrator admits sources through a bounded `std::jthread`
  pool. Trackbench uses two mutation workers initially; the core rejects zero or
  more than eight. The existing per-source physical/advisory locks remain the
  authority when paths alias or concurrent jobs meet.
- Each physical source is its own journaled transaction. Runtime failure is a
  structured per-source result and does not roll back unrelated sources which
  already completed. Results retain plan order and report pending, running,
  committed, failed, or cancelled state, escaped source identity, and an exact
  issue where applicable. Progress callbacks are serialized even though source
  committers may run concurrently.
- Cancellation stops admission of new sources. An admitted source receives the
  same cancellation token, but once its atomic publication boundary makes
  rollback unsafe it is allowed to finish coherently. Therefore a cancelled
  batch may truthfully contain both committed and cancelled sources.
- Properties presents a window-modal progress/result surface with a bounded
  row model, completed/total progress, per-source details, and a Cancel action.
  Closing while running requests cancellation and waits for safe source
  boundaries; it does not abandon workers.
- A successful source has already refreshed every durable occurrence before it
  is reported committed. The UI-thread completion observer then refreshes every
  matching in-memory row, schedules the normal workspace save, and reloads
  Metadata operations so its retained backup is immediately visible.
- Any committed source makes the original Properties baseline stale, so closing
  the result closes Properties. If nothing commits, the draft may remain, but
  its old plan is invalid. Partial failure, cancellation, and no-commit failure
  all require a fresh preview before retry; Trackbench never blindly replays
  the old plan or only its failed row subset.
- This decision exposes Apply only for the preservation-qualified native-FLAC
  text capability. It does not broaden format, artwork, or sidecar claims.

## Alternatives considered

### One worker for the whole selection

Rejected as the default. It is safe but needlessly serializes independent
prepare, verification, and publication work. The small fixed pool retains a
clear resource bound while the per-source executor preserves serialization
where it matters.

### One task or thread per source

Rejected. Large selections would make memory, descriptors, scheduling, and
cancellation behavior depend directly on selection size.

### Roll back every earlier success when a later source fails

Rejected. There is no atomic transaction spanning unrelated filesystems and
dependent-state transactions. Attempting compensating undo adds new failures
and can overwrite changes made after a source committed.

### Retry only failed rows from the original plan

Rejected. The remaining files and logical intents may have changed after the
preview. A fresh full preview is the clear, conservative conflict boundary.

### Mark cancellation complete immediately

Rejected. A source may be between preparation, publication, verification, and
dependent-state commit. The UI must wait until its executor reaches a proved
terminal or recoverable state.

## Consequences

- Native-FLAC text edits are now reachable through the complete preview,
  commit, recovery-history, and undo path.
- A batch is intentionally not all-or-nothing. The final result is the durable
  source-by-source truth and makes partial success explicit.
- The job-specific two-worker pool is bounded but not yet a shared cross-job
  scheduler. Artwork, file organization, ReplayGain, and conversion should
  converge on shared resource-class scheduling when those jobs become real.
- Retrying requires another user-visible preview. This costs another bounded
  reread but preserves the source-revision and logical-intent contract.

## Validation

- Qt-free tests use real FLAC files and the real commit/journal executor to
  prove multi-source success, stable ordered results, structured partial
  failure, and cancellation which prevents new source admission.
- Offscreen Trackbench coverage edits a real FLAC through Properties, obtains a
  ready preview, invokes Apply, observes the terminal per-source result,
  verifies the reread file metadata and in-memory list row, and confirms the
  retained backup appears in operation history. A separate three-source UI
  case cancels two admitted workers, verifies the third source is never
  admitted, and proves the staged draft remains available only for a fresh
  preview.
- The UI test deliberately reaches Apply before the ordinary persistence
  debounce; its durable prerequisite snapshot proves publication cannot race a
  missing list occurrence.

## Revisit when

- a shared job scheduler coordinates metadata, artwork, organization,
  ReplayGain, and conversion resource classes;
- a qualified format adapter has internal threading constraints which require a
  per-adapter concurrency limit below the batch limit;
- operation history supports an explicit “re-preview failed sources” shortcut
  without weakening full-source revalidation;
- settings expose mutation parallelism or retention policy.
