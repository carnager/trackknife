# ADR-0042: Revalidated metadata write plan and conflict preview

- Status: accepted
- Date: 2026-08-29
- Owners: Trackknife project
- Extends: ADR-0035 bounded metadata drafts and ADR-0041 complete Draft projection
- Complements: ADR-0033 conservative metadata adapter capabilities

## Context

The Draft column now shows the exact in-memory result, but it deliberately uses
the baseline captured when Properties opened. A safe writer cannot rely on that
snapshot: sources may have changed or disappeared, several logical CUE/chapter/
duplicate occurrences may refer to one physical file, and the generic TagLib
reader currently proves neither writing nor preservation of unknown data.

The next boundary must make every blocker visible without turning a sparse
draft into an Apply capability. Fresh filesystem and metadata reads are also
forbidden on the UI thread.

## Decision

- `MetadataWritePlan` is a Qt-free immutable-preview value. It retains every
  staged occurrence/field intent and groups exact raw source paths in first
  staged-occurrence order. Every Properties occurrence that refers to an
  affected path is recorded so later cache/list consequences are explicit.
- Planning always covers the complete Properties draft, independent of the
  current file-row selection. Selection remains an editing scope, not a way to
  hide staged work from commit review.
- The planner invokes the active metadata reader exactly once for each distinct
  affected raw path. It runs on a worker, supports cooperative cancellation,
  and publishes only a generation-current result. The preview action is
  explicit rather than rerunning disk reads after each keystroke.
- Captured revisions from every staged occurrence of one path must be present
  and equal. The fresh read must report the same device, inode, size, and
  nanosecond mtime. Missing/inconsistent baselines, unreadable sources, and
  changed observations are blocking conflicts.
- Exact raw paths are the operation inputs for metadata writes because this
  operation derives no destination path. Distinct raw paths that freshly
  resolve to the same device/inode are not silently merged: they are blocked as
  physical aliases until a writer strategy defines symlink/hard-link behavior.
  Destination-root containment remains part of rename/move/copy planning.
- Patches to different fields of one exact physical source merge. Equal intents
  for the same field merge while retaining every logical occurrence. Different
  replacement/removal results for the same physical field are a blocking
  logical-edit conflict; the planner never chooses a winner.
- A staged field whose effective baseline provenance is not embedded is blocked
  as an unresolved target. Trackbench does not redirect CUE/chapter, sidecar,
  annotation, stream, or cached-snapshot intent into the physical file merely
  because the row has a playable raw path. A newly added field may target the
  embedded document once an adapter proves that mapping.
- Each change shows the field's freshly read Original values and every exact
  replacement/removal intent. Every source also carries its observed adapter,
  expected/observed revisions, logical occurrences, and structured blockers.
- A source is ready only when revalidation succeeds, intents are unambiguous,
  their target is resolved, and the adapter independently advertises both field
  writing and preservation of unknown data. The current generic reader
  advertises neither, so real previews are truthfully blocked.
- Properties exposes `Preview write plan…`. Its asynchronous result opens a
  virtualized one-row-per-intent table with Status, Physical source, Field,
  Fresh original, Planned result, and Logical scope. The preview contains only
  Close—no Apply, writer call, journal, or filesystem mutation.
- A preview is a snapshot. Editing the draft invalidates an in-flight generation
  and closes an open preview. Execution will require another revalidation after
  preview, a journal, a preservation-proven adapter, verification, and recovery.

## Alternatives considered

### Enable Apply for formats TagLib says it can save

Rejected. Backend save support is not evidence that arbitrary native objects,
artwork, MusicBrainz values, and audio essence survive Trackbench's mapping.

### Treat every logical occurrence as a separate physical write

Rejected. CUE tracks, chapters, tracker subsongs, and duplicate list rows can
share one source. Independent writes would overwrite one another and make the
last occurrence an accidental winner.

### Merge conflicting intents using selection order

Rejected. Selection order is presentation data, not conflict policy. Every
incompatible result remains visible and blocks the source.

### Refresh the plan automatically after every edit

Rejected. Revalidation performs filesystem and metadata I/O. An explicit
preview action avoids background read churn while draft projection remains
immediate and in-memory.

### Require a configured music root for exact-path metadata updates

Rejected for this operation. Properties edits only raw files explicitly opened
by Trackbench and derives no new path. Root containment is required when an
operation evaluates or traverses a destination, especially rename/move/copy;
revision and physical-identity revalidation protect this exact-path plan.

## Consequences

- Users can see why a draft is not writable before any format writer ships.
- Shared-source and external-change hazards are modeled once in the Qt-free
  domain instead of being rediscovered by each future adapter.
- A preservation-proven writer can consume the same source/change plan, but it
  must still revalidate, journal, execute, reread, and verify; preview readiness
  alone never authorizes mutation.
- Physical aliases remain conservative blockers until writer strategy and
  recovery semantics are proven.

## Validation

- Qt-free tests cover exact-path grouping, one reader call per source, complete
  intent retention, equal merges, conflicting logical edits, non-embedded
  targets, fresh Original values, missing/inconsistent/changed revisions,
  read failures, capability blockers, physical aliases, readiness, invalid
  input, and cancellation.
- The real-FLAC offscreen Properties regression stages an ordered-value change,
  explicitly invokes planning, waits for the worker, verifies the virtualized
  fresh Original/Planned result row, and confirms that writer/preservation
  blockers are visible and no Apply control exists.

## Revisit when

- the first exact format mapping and preservation round trip can make a source
  genuinely ready;
- a journaled executor needs a serialized plan identity and second revalidation;
- symlink/hard-link write strategy is proven;
- explicit sidecar and CUE mutation targets enter Properties.
