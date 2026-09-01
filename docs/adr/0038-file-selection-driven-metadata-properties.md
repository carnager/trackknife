# ADR-0038: File-selection-driven metadata Properties workspace

- Status: accepted
- Date: 2026-08-29
- Owners: Trackknife project
- Supersedes: ADR-0036 Fields/Tracks layout choice
- Complements: ADR-0035 bounded metadata drafts and ADR-0037 exact ordered value editing
- Extended by: ADR-0039 dynamic metadata field vocabulary
- Draft projection extended by: ADR-0041 background metadata Draft projection
- Artwork presentation extended by: ADR-0077 read-only Properties artwork
  section

## Context

ADR-0036 made the field-oriented Original/Draft table primary but retained a
separate Tracks page and exact-value inspector for individual edits. Hands-on
use showed that this duplicated the scope concept: users first chose files to
open Properties, then had to choose between Fields and Tracks to say whether an
edit was bulk or individual.

A file list and a field table already express both axes. Selecting one file is
an individual edit; selecting several files is a bulk edit. The lower table can
project common, mixed, partial, and missing states for precisely those rows
without exposing a second track-by-field UI.

## Decision

- Properties has one vertically split workspace. A read-only, extended-selection
  file list is above the virtualized Field/Original/Draft table.
- The selected file rows are the sole edit scope. One selected row exposes that
  occurrence's exact Original and Draft values; several selected rows expose
  aggregate states and apply edits to those rows; no selected rows disables
  editing.
- Properties initially selects every row passed into the workspace. Changing
  the file selection updates the summary and lower projection after a short
  debounce.
- Arbitrary-subset baseline summaries use the same exact ordered-vector rules as
  ADR-0034. Single-row and complete-selection summaries reuse bounded cached
  data; other multi-row summaries run off the UI thread and reject stale
  generations.
- Scalar edits, exact ordered-value edits, field removal, revert, undo/redo, and
  discard all operate through ADR-0035's one sparse patch set. The exact-value
  editor always targets the current field across the currently selected files.
- Staged exact values are reconstructed when file scope changes. A selected
  single file therefore never degrades to an aggregate placeholder merely
  because the user selected another row and returned.
- The full track-by-field model remains an internal sparse backing model for row
  identity and patch addressing. The Tracks tab, per-cell editor surface, and
  separate exact-value inspector are removed from Properties.
- This remains an in-memory draft surface. It adds no Apply action, writer, or
  metadata mutation outside the explicit staged selection.

## Alternatives considered

### Keep Fields and Tracks tabs

Rejected after hands-on feedback. The tabs make users choose an editing mode
that is already determined by file selection and split related evidence across
two surfaces.

### Put per-track values into a second lower inspector

Rejected. With one file selected, Original and Draft already contain the exact
ordered values. Repeating them in another table adds navigation without adding
scope or safety.

### Rebuild arbitrary subset summaries synchronously

Rejected. A selection may contain 100,000 occurrences and 4,096 union fields.
Debounced generation-safe background projection preserves the workspace's UI
budget while still making small and complete selections immediate.

## Consequences

- Individual and bulk tagging use one consistent interaction: select files,
  then edit fields.
- The compact lower table keeps the Picard-like Original/Draft comparison while
  removing the mode switch and very wide metadata matrix.
- Future filename capture, transformation, and provider proposals can use the
  same selected-file scope and preview surface.
- ADR-0077 later adds sibling Fields and read-only Artwork sections below the
  same selector without restoring a second metadata editing mode.

## Validation

- Qt-free tests cover arbitrary one-row and multi-row summaries plus invalid
  subset indexes.
- The real-FLAC offscreen regression verifies the single split, initial
  select-all behavior, single-file exact values, multi-file aggregate states,
  scoped scalar/exact/removal edits, selection changes with staged values,
  undo, revert, and a disabled editor for an empty file selection.

## Revisit when

- grouped file presentation is needed for very large album-oriented selections;
- explicit sidecar or CUE targets need more selection-scope presentation.
