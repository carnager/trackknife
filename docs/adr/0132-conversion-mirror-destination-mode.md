# ADR-0132: Mirror-source-structure conversion destination mode

- Status: accepted
- Date: 2026-09-09
- Owners: Trackknife project
- Extends: ADR-0054 output layouts, ADR-0105/0107 converter, ADR-0131
  artwork carriage

## Context

Converted output paths are built exclusively from `tkfmt-1` expressions.
Re-encoding an already organized collection should be able to reproduce
the source folder structure beneath an explicit destination root without
re-deriving it from tags (roadmap area 3; the specification's "mirror
structure" destination mode). The open source-root-inference UX question
needed a first decision.

## Decision

- `ConvertedPublicationPolicy` gains an optional
  `mirror_source_root_raw_path`. When set, the planner derives each item's
  relative directory from the source path's location below that root and
  its basename from the source filename's stem, byte-exact, instead of
  evaluating the layout expressions. The mirror strings are raw OS path
  bytes: the expression-output UTF-8 gate does not apply, while
  sanitization, component/path bounds, containment, collision, and
  existing-target checks run unchanged. A source outside the root reports
  the new blocking `mirror_source_outside_root` issue; the root itself
  must be a normalized absolute path.
- **Source-root inference:** the exported
  `common_source_directory_raw_path` returns the deepest directory
  containing every source (byte-wise component comparison). The converter
  dialog infers the root this way per selection; an editable root is
  follow-up work.
- The converter dialog exposes the mode as a persisted "Mirror source
  folders" checkbox that disables the two expression fields; the preview
  and collision handling are unchanged because they already render the
  planner's relative results.
- Logical items sharing one physical source (cue subtracks) map onto one
  identical mirrored name and therefore fail as the existing
  `duplicate_target` conflict — mirror mode reproduces files, it does not
  split them. Expression naming remains the mode for cue material.

## Consequences

- A tag-independent conversion of an organized tree reproduces it exactly
  beneath the destination root, including non-UTF-8 names.
- Tests pin mirroring at the planner level (relative derivation, root
  containment, outside-root issue, stem extension swap, shared-source
  collision) and the dialog checkbox path end to end.
- Editable mirror roots, and any smarter inference than the deepest
  common directory, remain follow-up work.
