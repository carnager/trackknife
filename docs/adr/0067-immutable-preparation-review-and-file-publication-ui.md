# ADR-0067: Immutable preparation review and file-publication UI

- Status: accepted
- Date: 2026-08-30
- Owners: Trackknife project
- Extends: ADR-0054 unified preparation plans, ADR-0055 output-path planning,
  ADR-0056 file preflight, and ADR-0063 bounded file-publication Apply
- Partially superseded by: ADR-0069, which excludes all manual and automatic
  draft metadata from path evaluation when Save tags is off

## Context

Trackbench already had the core pieces for safe path-only rename and move: a
final-metadata path planner, live filesystem preflight, journaled same- and
cross-filesystem executors, durable list/cache and active-player relocation,
same-filesystem undo, startup recovery, and bounded batch Apply. The Properties
workspace still presented only the metadata write plan. Rename and Move stayed
disabled, file-publication journals were absent from startup history, and the
UI had no single immutable object that retained both tag and path effects.

This decision originally allowed path expressions to see the final manual and
automatic draft when tag persistence was off. ADR-0069 supersedes that
behavior because it produced paths that did not represent the stored tags.
Conversely, Trackbench does
not yet have an executor that prepares changed file content directly at the
final destination. Running the existing metadata commit and then the existing
path executor would create two independently recoverable mutations and violate
ADR-0054's one-artifact combined-publication requirement.

## Decision

### One immutable review boundary

- `PreparationPlan` owns the selected Save tags, Rename, Move, and ReplayGain
  choices; the number of final metadata-context changes; an optional freshly
  revalidated metadata plan; and an optional pure output-path plan plus its
  retained live preflight.
- Checked automatic chains run against the manual draft first. A pure draft
  materializer then projects one final `MetadataDocument` per selected item for
  path evaluation without mutating the staged selection or inventing a native
  writable representation.
- The review presents exact metadata changes and exact source/target paths in
  separate tabs under one shared immutable plan. It distinguishes **Naming
  only** metadata from tag writes and shows raw generated folders, basenames,
  sanitization, filesystem publication kind, and blockers.
- Apply is offered only when every enabled executable effect is ready. Every
  attempt consumes and invalidates the review; retry requires a fresh plan and
  preflight.

### Qualified workspace exposure

- Rename is enabled only with a selected saved output-layout profile. Move
  additionally requires a selected saved raw-path destination profile. Both
  remain structurally unavailable without the Trackbench file-publication
  service and outside local-file Properties.
- Path-only Rename/Move is executable. Per ADR-0069, it uses only the captured
  revision-qualified source tags when Save tags is off.
- Save tags plus Rename/Move is executable when there are no tag changes. If
  the final metadata context contains changes, the combined review remains
  visible but Apply is blocked until one executor can prepare and verify the
  changed content directly at the reviewed destination.
- ReplayGain remains disabled until M7 qualifies its analysis and storage
  effects.

### Publication, recovery, and history

- File Apply persists the captured workspace first, then sends the retained
  preflight through the two-worker bounded publication job. Its dependent-state
  callback composes revision-qualified active-player relocation with the
  idempotent all-occurrence list/cache transaction.
- Ordered progress, cancellation, partial results, and exact per-source issues
  have a dedicated result surface. Committed relocations update every visible
  occurrence and playback intent before the operation history reloads.
- Startup runs both same- and cross-filesystem recovery state machines. Their
  dependent callbacks use the same player plus durable list/cache boundary as
  normal Apply, and recovered relocations refresh visible state.
- The preparation-operation history includes recent terminal and incomplete
  file-publication evidence. Ambiguous records open automatically with undo
  disabled. A completed original same-filesystem publication offers the
  existing linked reverse-publication undo unless a non-rolled-back reversal
  already exists.
- Completed cross-filesystem moves remain visible but do not advertise undo.
  Cross-filesystem undo needs a separate product and storage policy because the
  original source has been intentionally removed.

No schema migration is required. The existing file-publication journal remains
the durable authority; the SQLite implementation adds only a bounded recent-
history query for presentation.

## Alternatives considered

### Write tags first and move the resulting source

Rejected. Two separately journaled mutations could expose an intermediate path
or content state and could not truthfully provide one combined rollback and
recovery boundary.

### Ignore staged values when Save tags is off

Originally rejected here, then accepted by ADR-0069 after live testing showed
that naming-only synthetic values made filenames misrepresent the source tags.

### Expose Apply but leave file recovery headless

Rejected. A successful move and an interrupted move must both remain visible
after restart. Reconciliation evidence and the qualified undo path are part of
the action's safety contract, not optional diagnostics.

## Consequences

- Rename and Move are usable for path-only preparation with the complete
  ADR-0055–0063 safety chain and no UI-thread filesystem mutation.
- One review type can grow to artwork, ReplayGain, and combined destination-
  artifact publication without changing the explicit Preview/Apply contract.
- Metadata drafts and automatic-chain diagnostics do not enter a path-only
  operation while Save tags is off.
- The operations surface retains its historical object identities for UI-test
  compatibility even though its user-visible scope now includes file paths.
- Combined changed tags plus paths, cross-filesystem undo, companion-file
  operations, and batch-level grouping remain explicit future work.

## Validation

- Qt-free tests prove draft materialization preserves unsupported objects and
  exact-native identity, and prove readiness for path-only, metadata-only, and
  blocked combined-content plans.
- ADR-0069 replaces the original staged-title case with offscreen coverage that
  proves manual and automatic synthetic titles are ignored while Save tags is
  off and the actual source title reaches Apply.
- File-journal tests prove terminal and reversal records remain visible in
  newest-first bounded history.
- Offscreen Trackbench tests prove startup presents file reconciliation and a
  completed same-filesystem move can be moved back through the unified history,
  updating both the filesystem and visible list source.

## Revisit when

- a destination-artifact writer can combine metadata, artwork, ReplayGain, and
  path publication;
- cross-filesystem undo gains an explicit retention and capacity policy;
- companion files or a user-visible batch identity enter the preparation plan.
