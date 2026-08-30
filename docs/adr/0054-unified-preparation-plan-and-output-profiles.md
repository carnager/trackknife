# ADR-0054: Unified preparation plan and reusable output profiles

- Status: accepted
- Date: 2026-08-30
- Owners: Trackknife project
- Extends: ADR-0042 revalidated metadata write plans, ADR-0047 bounded metadata
  Apply jobs, and ADR-0052 tabbed tagging workspace

## Context

Trackbench's tagging workspace already computes a final metadata draft, previews
physical writes, and applies them through a recoverable journal. File naming,
moving, ReplayGain, and later conversion would be substantially less useful as
independent dialogs: users commonly want final metadata to determine paths,
want loudness results stored with the same preparation pass, and want one place
to understand every effect on a selected file.

Picard-style Save/Rename/Move toggles provide a useful interaction model, but
Trackbench also needs sidecar fallback, source-revision conflicts, cross-device
moves, logical tracks, reusable converter destinations, and explicit
preservation claims. Those requirements need one Trackbench-owned operation
contract rather than UI checkboxes that directly invoke unrelated services.

## Decision

### One preparation plan

- The `Tags · N tracks` workspace grows into the primary preparation workspace.
  A visible Operations section offers independent **Save tags**, **Rename
  files**, **Move files**, and **ReplayGain** choices as their implementations
  become qualified.
- The workspace builds one typed `PreparationPlan`. Metadata edits and checked
  saved scripts first produce a final in-memory document; optional ReplayGain
  analysis contributes typed results; path expressions then evaluate against
  that final document; one immutable review presents all enabled effects.
- The final review groups effects by physical source and shows metadata
  old/new values, ReplayGain measurement and storage destinations, and exact
  raw source-to-target paths. Blocking conflicts disable Apply for the complete
  plan rather than allowing a hidden partial combination.
- Apply remains explicit. Its label and summary identify the enabled
  operations. A checkbox is not exposed as usable until its planner,
  cancellation, conflict checks, journal, recovery, dependent-state update,
  and relevant real-file tests exist.

### Independent operation semantics

- **Save tags** controls whether the final computed metadata document is
  persisted. When it is off, draft and script results may still provide the
  visible evaluation context for naming, but the preview must label those
  metadata changes as used for planning and not written.
- **Rename files** evaluates the selected output layout's basename expression.
  Without Move, only the basename changes and the source remains in its current
  directory.
- **Move files** selects an explicit destination root and evaluates the output
  layout's relative-directory expression below it. Without Rename, the source
  basename is retained. With both enabled, the destination is the selected
  root, evaluated relative directory, and evaluated basename.
- **ReplayGain** is a typed analysis operation, not a metadata transformation
  action or executable script. Checking it selects an algorithm, track/album
  grouping mode, peak policy, and embedded/sidecar persistence policy. Group
  boundaries are visible before decoding starts; cancellable analysis then
  feeds the combined final review. Results remain tied to the analyzed source
  revision.
- Save tags does not gate explicit ReplayGain persistence. A ReplayGain plan may
  use its qualified embedded mapping or sidecar fallback even when ordinary tag
  edits are not selected for writing.

### Reusable output contracts

- A versioned `OutputLayoutProfile` owns a name, `tkfmt-1` dialect, relative
  directory expression, basename expression, and sanitization-policy version.
  It never owns an absolute root or a fixed output extension.
- A separate `DestinationProfile` owns a named explicit raw-OS-path root and
  its containment policy. Keeping roots separate makes layout profiles portable
  and prevents imported expressions from silently selecting a filesystem
  destination.
- Existing-file rename/move preserves the source extension. The converter owns
  its output extension according to the selected container and may reuse the
  same layout and destination profiles without mutating them.
- Converter jobs inherit the workspace defaults but may override layout or
  destination per job. Signal-changing conversion does not copy stale
  ReplayGain blindly; it removes or rescans it according to converter policy.

### Publication and recovery

- Combined tag and path operations prepare the final verified file directly at
  its destination sibling/staging location. Trackbench does not rewrite the
  source and then perform a second unjournaled move.
- A content-preserving same-filesystem path-only operation may use an atomic
  rename after conflict checks. Cross-filesystem moves use verified copy,
  durable publication, and source deletion only after destination verification.
- The journal records source and target raw paths, revisions, every enabled
  content operation, sidecars, publication state, and dependent-state intent.
  Multi-source completion is recoverable and accurately reported, not claimed
  to be globally filesystem-atomic.
- After verified publication, every matching list occurrence, playback queue,
  local metadata cache, statistics record, and future library reference follows
  the path change as one logical dependent-state transaction.

## Alternatives considered

### Separate dialogs for tags, organizing, ReplayGain, and conversion

Rejected as the primary workflow. It repeats selection and conflict review,
encourages multiple physical rewrites, and prevents users from seeing how final
metadata affects destination paths.

### Store the absolute target root inside a naming preset

Rejected. Naming conventions should be reusable across machines, storage
roots, and converter jobs. Absolute raw paths have a different lifecycle and
authority boundary.

### Model ReplayGain as a transformation-chain step

Rejected. ReplayGain requires PCM decoding, grouping, progress, cancellation,
source-revision binding, and sometimes sidecar persistence. Transformation
chains remain deterministic, side-effect-free metadata evaluation.

### Apply tags first and move the rewritten source afterwards

Rejected. It performs unnecessary source mutation, creates an unjournaled gap
between operations, and complicates recovery. One prepared final publication is
safer and more efficient.

## Consequences

- Trackbench gains one coherent preparation vocabulary that can grow without
  repeatedly redesigning Properties.
- Output layout and destination profiles become shared product concepts rather
  than converter-only settings.
- A single final preview is possible, although ReplayGain still has a visible
  grouping preflight and a cancellable analysis phase before that review.
- Implementation must extend the existing metadata-only journal before
  rename/move can be enabled; UI resemblance alone is not a capability claim.

## Validation required

- Pure path-planner tests cover dialect/version pinning, raw-path containment,
  invalid UTF-8 source paths, sanitization, collisions, aliases, case-folding
  hazards, extension ownership, and rename/move toggle combinations.
- Real-file tests cover same- and cross-filesystem publication, injected
  failures at every journal state, exact undo/recovery, sidecar relocation, and
  all-occurrence dependent-state updates.
- Offscreen workspace tests cover independent operation choices, computed but
  unwritten metadata context, combined per-file review, blockers, cancellation,
  and explicit Apply.
- Converter tests prove profile reuse with a producer-owned extension and
  per-job overrides.
- ReplayGain tests prove pre-analysis grouping, stale-result rejection,
  embedded/sidecar selection, and inclusion in the combined review.

## Delivery sequence

1. Add versioned output-layout and destination profiles plus a pure destination
   planner and collision preview.
2. Extend the journal, recovery, undo, and dependent-state transaction for
   qualified rename/move; then expose those operation choices.
3. Generalize the current metadata preview/Apply boundary into the combined
   preparation review without regressing metadata-only use.
4. Plug the M7 ReplayGain job into the same plan after its scanner and storage
   adapters are qualified.
5. Reuse profiles from the M8 converter with explicit per-job overrides and
   converter-owned extension/signal-changing policy.

## Revisit when

- artwork and cue-sheet mutations join the preparation plan;
- the converter needs merge/multi-output layouts that cannot map to one source
  item and one relative output path;
- a destination backend other than a plain local filesystem is proposed;
- user testing justifies named whole-operation presets in addition to separate
  layout and destination profiles.
