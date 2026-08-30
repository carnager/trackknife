# ADR-0055: Versioned output profiles and pure path planning

- Status: accepted
- Date: 2026-08-30
- Owners: Trackknife project
- Extends: ADR-0054 unified preparation plan and reusable output profiles

## Context

ADR-0054 establishes reusable naming layouts, separate destination roots, and
one combined preparation review. Before rename or move can appear as a usable
workspace operation, Trackbench needs stable persisted profile contracts and a
deterministic planner that can be exercised without filesystem mutation.

Path generation crosses two different text boundaries. `tkfmt-1` source and
generated names are UTF-8 product data, while existing Linux source and
destination paths are raw OS bytes. Collision and containment claims also
depend on an explicit filesystem observation rather than whatever happens to
exist while a pure function is tested.

## Decision

### Versioned profiles

- `OutputLayoutProfile` schema 1 stores a UTF-8 name, the complete `tkfmt-1`
  dialect/compiler version, one relative-directory expression, one non-empty
  basename expression, and the `linux-v1` sanitization policy identity.
- `DestinationProfile` schema 1 stores a UTF-8 name, one normalized absolute
  non-root raw OS path, and `lexical-beneath-root-v1` containment. It contains
  no naming expression.
- Profile names are unique by exact byte value. Upsert uses a stable UUID;
  deleting an absent identity reports not-found.
- Reversible SQLite migration 13 stores expression, dialect, compiler, policy,
  and schema versions explicitly. Destination roots use BLOB storage so invalid
  UTF-8 path bytes survive restart unchanged. A repository owns at most 256
  profiles of each kind.

### Evaluation and extension ownership

- Planning accepts selection-order item indexes, the final metadata document,
  a captured source revision, and the raw source path. Inputs are bounded,
  sorted, and unique by item index.
- Expressions compile in the `path_generation` context. Metadata fields and
  `$info(path|directory|filename|filename_ext|extension)` are available. Raw
  technical values that are not valid UTF-8 are missing to the expression;
  they are never repaired or misdecoded.
- Rename evaluates only the basename and keeps the current directory. Move
  evaluates only the relative directory below the selected destination and
  retains the existing raw filename. Enabling both evaluates both pieces.
- Existing-file operations append the source extension to the generated
  basename exactly, including its case. The future converter owns its extension
  separately.

### `linux-v1` sanitization

- `/` in the relative-directory result denotes a component boundary. `/` in a
  generated basename is replaced with `_`.
- NUL bytes are replaced with `_`. Empty and `.` components become `_`; `..`
  becomes `__`. Empty relative-directory output means directly below the
  operation root.
- No Unicode normalization, transliteration, whitespace trimming, hidden-file
  rewriting, or portable-platform reserved-name policy occurs. Raw and
  sanitized directory/basename output are both retained for preview, with an
  explicit changed flag.
- An expression whose raw directory result begins with `/` is a blocking error
  even though sanitization also produces a contained diagnostic target.
- Components and complete target paths are byte-bounded. The initial defaults
  are 255 bytes per component and 4,095 bytes per target path; a filesystem
  adapter may provide stricter limits.

These rules are persisted behavior. Any changed interpretation requires a new
sanitization-policy version.

### Pure conflict plan

- The Qt-free planner performs no I/O. Its caller supplies an explicit snapshot
  of normalized absolute existing paths and their file/directory/other kind,
  plus whether comparisons for that snapshot fold ASCII case.
- Targets must be strict lexical descendants of the current directory for
  rename-only or of the destination root for move. This is a lexical guard,
  not a symlink, bind-mount, writability, or filesystem-capability claim.
- The plan blocks invalid source paths, expression errors or invalid output,
  size-limit failures, containment failures, conflicting destinations for
  logical items sharing one raw source, inconsistent revisions for that
  source, distinct raw paths with the same observed device/inode, duplicate
  targets, existing targets, non-directory parents, and targets occupied by
  another selected source.
- A case-only change is retained as a visible non-blocking warning. The later
  fresh filesystem preflight decides whether it is executable on the target
  filesystem.
- Logical items sharing one exact raw path collapse into one planned physical
  source only when they produce the same target and carry the same revision.
  Their item indexes remain attached to the plan.
- A plan is ready only when it contains a physical source and no blocking
  issue. Cancellation and global validation failures return typed errors rather
  than partial plans.

## Alternatives considered

### Inspect the live filesystem inside the planner

Rejected. It makes tests timing-dependent and still cannot authorize a later
mutation. Observation belongs in a bounded adapter; the immutable snapshot is
an explicit planner input and execution must observe again under lock.

### Normalize or transliterate every generated name automatically

Rejected for `linux-v1`. Such rewriting hides material filename changes and
can create new collisions. More portable policies may be added under distinct
versioned names and must expose their raw-to-sanitized effect.

### Treat a source path string as physical identity

Rejected. CUE rows may intentionally share one path, while hard links can give
one inode several paths. Captured device/inode identity and full revisions are
part of the plan boundary.

## Consequences

- Naming and destination choices can now survive restart and be shared by the
  preparation workspace and future converter without sharing an absolute root
  implicitly.
- The planner can drive an exact source-to-target preview, including visible
  sanitization, before any file-operation executor exists.
- This slice does not enable Rename or Move in the UI. Fresh source/target
  observation, symlink and mount resolution, permissions, same/cross-filesystem
  publication, journal recovery/undo, and dependent-state reconciliation are
  required first under ADR-0054.

## Validation

- Qt-free tests cover independent rename/move choices, final-metadata
  evaluation, raw invalid-UTF-8 move-only filenames, extension preservation,
  visible sanitization, lexical containment, component limits, case warnings,
  shared-source disagreement, revision disagreement, device/inode aliases,
  source-target dependencies, existing/non-directory targets, duplicate
  targets, invalid profiles, and cancellation.
- Persistence tests cover create, exact-name conflicts, invalid-root rollback,
  stable-ID updates, raw-byte restart round trips, deletion, and not-found.
- Migration 13 must pass reversible up/down schema probes.

## Revisit when

- a portable or user-defined sanitization policy is implemented;
- real filesystem probes establish normalization/case behavior beyond the
  explicit ASCII comparison policy;
- converter output needs multiple files from one logical source;
- companion files and cue-sheet path rewrites join the plan.
