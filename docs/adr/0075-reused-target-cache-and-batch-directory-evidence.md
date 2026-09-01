# ADR-0075: Reused-target cache and batch-directory evidence

- Status: accepted
- Date: 2026-08-31
- Owners: Trackknife project
- Extends: ADR-0045 provenance-aware metadata state, ADR-0059
  all-occurrence source relocation, ADR-0063 bounded file Apply, and ADR-0074
  composed metadata/path publication

## Context

A real combined native-FLAC batch reused filenames that had previously been
destinations of a completed path-only operation. No current list occurrence or
file occupied those targets, but their revision-qualified metadata caches still
existed in SQLite. The combined dependent transaction treated the historical
cache as an active owner and rolled the first publication back.

That rollback occurred after the executor had created and synced the reviewed
album directories. ADR-0063 recorded shared-directory ownership only after the
complete source publication succeeded, so later members conservatively treated
those exact executor-created directories as external changes. One legitimate
dependent-state conflict therefore cascaded into unrelated topology failures.

The metadata source cache is not an audio-file backup. It is durable correctness
state containing the last verified fields and source revisions, used to keep a
delayed workspace save or recovery replay from restoring stale metadata. Its
presence alone must not reserve a filesystem pathname forever.

## Decision

- A persisted local list occurrence at a relocation target remains an active
  collision and blocks the complete transaction.
- When no persisted target occurrence exists, an older target metadata cache is
  historical, not authoritative ownership. The relocation transaction removes
  it before either rekeying the current source cache or installing the verified
  published document. The removal, occurrence relocation, replacement cache,
  revisions, and idempotency record remain one SQLite transaction.
- Revision-qualified relocation history remains intact. A delayed snapshot of
  the current source still follows the new source-to-target record and receives
  the replacement cache. A different physical revision at an old path is not
  redirected.
- Each file executor may report the exact reviewed missing-directory chain
  immediately after `create_planned_directories` has successfully created,
  opened without symlink following, and synced it. The bounded Apply group
  records this evidence while holding its topology lock.
- That directory evidence survives a later preparation, dependent-state, or
  source-publication rollback within the same batch. Related sources may use
  the established directories. The failed file itself and its target still
  follow their normal rollback rules.
- A directory that appears before the executor proves its own creation remains
  an external topology conflict. Merely observing a directory after failure is
  not sufficient evidence.
- No schema migration is required. Existing schema-22 databases gain the
  corrected target-reuse behavior without deleting caches eagerly at startup.

## Alternatives considered

### Treat every historical cache as permanent path ownership

Rejected. It makes a previously used destination pathname impossible to reuse
even when no file or current Trackbench occurrence owns it.

### Delete every metadata cache on restart

Rejected. The cache participates in durable metadata publication and recovery;
making it session-only would weaken stale-save and crash-boundary guarantees.
Bounded garbage collection of proven-unreferenced history remains possible.

### Trust any directory observed after a failed source

Rejected. An external process could have created or replaced the path. Trust is
granted only by the executor at the point where its descriptor-relative
creation routine has succeeded.

### Require a fresh review after any member failure

Rejected as the batch default. Ordered partial results are intended to let
unrelated sources finish when their reviewed assumptions remain proven.

## Consequences

- Reusing a prior publication destination no longer requires manual SQLite
  cleanup and does not lose the newly verified metadata document.
- One dependent-state failure no longer produces a cascade of false
  shared-directory conflicts.
- Audio-file targets and active persisted occurrences retain strict no-replace
  collision behavior.
- Durable caches still survive restart. A future retention policy may prune
  records only after proving that no list occurrence, recovery record, or
  in-process stale snapshot can require them.

## Validation

- A repository regression creates a historical target cache through a real
  refresh and relocation, removes its last occurrence, reuses the target for a
  combined publication, and proves replacement metadata plus stale-snapshot
  convergence.
- A path-only bounded batch injects dependent-state failure after the first
  source creates a shared missing directory; the first source rolls back and
  the second commits through that exact directory.
- A real native-FLAC combined batch proves the same behavior through changed
  destination artifacts and rereads the second source's committed title.
- Existing target-occurrence collision coverage continues to fail closed.

## Revisit when

- metadata-cache retention and garbage collection are designed;
- persisted batch identities allow directory evidence to span process restart;
- companion files introduce directory ownership beyond the primary artifact.
