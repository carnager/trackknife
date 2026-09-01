# ADR-0082: Pre-resolved relocation reconciliation

- Status: accepted
- Date: 2026-09-01
- Owners: Trackknife project
- Extends: ADR-0059 revision-qualified all-occurrence source relocation,
  ADR-0074 composed metadata/path publication, and ADR-0075 reused-target
  cache handling

## Context

A successful A→B publication leaves revision-qualified relocation history so
an older debounced workspace save cannot resurrect A. A user or external tool
can later move the same file back to A. On one filesystem, that reverse rename
preserves every member of Trackbench's current source-revision tuple.

When Properties freshly reviewed another A→B publication, its mandatory
pre-Apply workspace save therefore resolved the live A occurrence through the
older history and persisted it at B. The filesystem executor could still
publish the reviewed file correctly because A existed and B did not, but the
dependent transaction rejected it because no persisted occurrence remained at
A. The review reported every source ready and the failure appeared only after
publication began.

The B occurrence is not an unrelated collision in this state. It is the exact
revision-matching occurrence that the repository itself pre-resolved from A.

## Decision

- The relocation transaction first retains its existing behavior: enumerate
  every local occurrence at the source and require the exact captured previous
  revision.
- If no source occurrence exists, it also enumerates local occurrences already
  at the target. This fallback is accepted only when at least one exists and
  every occurrence has the exact previous revision. The existing logical-row,
  field-limit, and combined-publication guards apply unchanged.
- An accepted pre-resolved set receives the verified published revision and,
  for a changed destination artifact, the exact reread metadata document. The
  operation records its ordinary positive affected-occurrence count and
  remains idempotent.
- A target occurrence is still a hard collision when any source occurrence is
  present. A target with a different or missing revision also fails closed.
  When neither source nor target has an occurrence, the transaction continues
  to report missing durable state.
- A pre-resolved path-only operation retains an already-owned target metadata
  cache when no newer source cache exists. A source cache supersedes it when
  present; a combined publication replaces it with the verified destination
  document. Historical unowned target caches retain ADR-0075 behavior.
- No schema migration is required. Schema 24 already records the affected
  count, exact paths and revisions, cache action, metadata-refresh intent, and
  operation identity needed for replay.

## Alternatives considered

### Make all relocation and metadata state session-only

Rejected. File publication crosses crash boundaries, and a completed physical
move must not leave restored lists, playback intent, or metadata pointed at the
old source after restart. The state is recovery and convergence evidence, not
an audio-file backup.

### Accept a relocation with zero persisted occurrences

Rejected. Properties operates on working-list rows and persists its captured
workspace before mutation. Zero occurrences at both paths means that invariant
failed and must not be hidden. The observed case has exact occurrences at the
target and can be proved without weakening the transaction.

### Delete all completed relocation history immediately

Rejected. A save captured before publication can arrive after the dependent
transaction. Revision-qualified history is what makes that delayed write
converge instead of restoring stale paths or metadata.

## Consequences

- Repeating a reviewed move after the same physical files were moved back
  outside Trackbench no longer produces an Apply-time all-source failure.
- Real target collisions and missing workspace persistence remain explicit.
- SQLite recovery evidence remains durable, but an older relocation can no
  longer make its own exact pre-resolved target irreconcilable with a fresh
  publication.

## Validation

- Repository coverage creates A→B history, submits a newly opened A snapshot
  with the same revision, proves it is pre-resolved to B, then applies another
  combined A→B publication and verifies the new revision, metadata, and exact
  replay.
- The real native-FLAC bounded preparation test starts with the persisted row
  pre-resolved at an absent target while the file is physically at the source,
  then proves changed metadata publication, source removal, target creation,
  and durable metadata/path reconciliation.
- Existing active-target collision and different-revision path-reuse tests
  continue to fail closed.

## Revisit when

- `LocalSourceRevision` gains rename-sensitive evidence such as change time;
- relocation history receives bounded session-aware compaction;
- another path-keyed durable consumer joins the dependent transaction.
