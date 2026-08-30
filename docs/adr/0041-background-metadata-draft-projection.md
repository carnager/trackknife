# ADR-0041: Background complete metadata draft projection

- Status: accepted
- Date: 2026-08-29
- Owners: Trackknife project
- Extends: ADR-0035 bounded metadata drafts
- Complements: ADR-0038 file-selection-driven metadata Properties

## Context

The aggregate Draft column could show exact values only when every selected
file carried the same explicit patch. A per-file exception instead fell back
to “N staged cells · draft values vary,” even when applying that exception made
the final values common, missing, or precisely partial. That count was truthful
but not a complete preview of the in-memory result.

Computing every result after every keystroke on the UI thread is forbidden: a
Properties workspace may contain 100,000 occurrences and 4,096 sparse union
fields. A background job also needs an immutable draft snapshot so continued
editing cannot race the projection.

## Decision

- `StagedMetadataPatchSet` copies share an immutable state. A later mutation
  detaches the live state when a projection still owns the previous version.
  Dispatching a worker snapshot therefore does not copy the bounded 64 MiB
  patch payload on the UI thread.
- The metadata core projects a sorted, unique selected-item scope sparsely. It
  walks each occurrence's present baseline fields plus that occurrence's patch
  range rather than materializing the item-by-field Cartesian product.
- Every projected field reports the exact resulting common/mixed/partial/
  missing state, present-item count, staged-item count, and exact ordered values
  when the result is common. Explicit empty strings, duplicates, replacement,
  and removal retain ADR-0035/0037 semantics.
- Properties debounces projection for 40 ms and runs it outside the UI thread.
  A generation guards both file-scope and draft changes. Stale work receives a
  cancellation request, its result is discarded, and at most one projection is
  in flight; the newest generation is queued after obsolete work stops.
- With no patches, Draft reuses Original immediately. A known uniform bulk edit
  remains visible while the complete projection is pending. Other changed rows
  say that the draft preview is being prepared instead of presenting a guessed
  state.
- Once ready, Draft shows exact common values, “various” for mixed results,
  present counts for partial results, and explicit removal for a staged result
  that is missing. Tooltips identify the complete projected state and staged
  cell count.
- This is a complete **in-memory result-state preview**, not a file-operation
  plan. It performs no fresh source read, revision revalidation, format mapping,
  preservation proof, filesystem mutation, or Apply action.

## Alternatives considered

### Recompute the selected matrix synchronously

Rejected. Large selections would make typing, undo, and file-scope changes
block the UI thread.

### Keep the staged-count fallback until writers exist

Rejected. Users need to know whether current edits converge or diverge before
the later write planner exists, and the sparse patch domain already contains
enough information to answer accurately.

### Let several stale projection jobs run concurrently

Rejected. Generation checks protect correctness but not resource use. One
cooperatively cancelled job plus one latest pending generation keeps worker and
snapshot memory bounded.

### Let the worker read the live patch set under a lock

Rejected. A long shared read would either race editing or make UI mutations wait
for the worker. Immutable snapshots keep those lifetimes independent.

## Consequences

- Per-file exceptions no longer hide behind a count: Draft can reveal exact
  convergence, real mixed results, partial presence, and complete removal.
- Snapshot dispatch is constant-time. If editing overlaps an active snapshot,
  the first live mutation detaches that bounded patch state; later mutations in
  the same generation remain on the detached live state.
- Complete source/file conflict preview remains separate and still blocks any
  write capability.

## Validation

- Qt-free tests cover sparse selected-item projection, exact common values,
  staged counts, partial/mixed/missing results, sorted/unique bounds,
  cancellation, and copy-on-write snapshot isolation.
- The real-FLAC offscreen Properties regression proves that one per-file edit
  can converge a mixed field to an exact common Draft value, that another edit
  produces the correct partial result, and that rapid undo/scope changes cannot
  publish an obsolete generation.

## Revisit when

- performance measurements near the 100,000-patch and 64 MiB bounds require a
  structurally shared patch map rather than copy-on-write detachment;
- the first preservation-proven writer consumes ADR-0042's fresh-read plan;
- preview jobs need progress beyond the current preparing/ready states.
