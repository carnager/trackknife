# ADR-0037: Exact ordered metadata value editor

- Status: accepted
- Date: 2026-08-29
- Owners: Trackknife project
- Complements: ADR-0035 bounded metadata drafts and ADR-0036 aggregate fields workspace
- Presentation revised by: ADR-0038 file-selection-driven metadata Properties

## Context

Scalar cell editing cannot safely represent repeated metadata values. Joining
values with a semicolon, slash, newline, or another delimiter is lossy because
every delimiter is also valid user data. The editor must preserve exact order,
duplicates, and explicit empty strings while working for both an aggregate
field replacement and one occurrence in the Tracks drill-down.

The Properties workspace remains non-modal and write-free. A focused value
editor must not create a second draft, perform file I/O, or enter a nested Qt
event loop.

## Decision

- `Edit values…` and `Ctrl+Enter` open a non-blocking, window-modal child of the
  Properties workspace for the current aggregate field or individual metadata
  cell.
- The editor is a structured ordered list. Each row is exactly one string.
  Add, remove, move up, move down, and inline row editing preserve order and
  duplicates without interpreting any character as a separator.
- An empty row is one explicit empty value and is labelled as such outside edit
  mode. Accepting zero rows is disabled. Field removal remains the explicit
  Delete operation in Properties, so an empty list is never inferred as
  destructive intent.
- Opening a common or individual value list copies its exact current ordered
  vector into the editor. A mixed, partial, missing, or individually diverged
  aggregate starts without guessed values and explains that the new list will
  replace the field across the complete selection.
- Accepting from Fields stages the vector on every selected occurrence.
  Accepting from Tracks stages it only on the selected cell. Both paths use the
  same ADR-0035 patch set, transaction bounds, undo/redo history, draft styling,
  and discard behavior as scalar edits.
- The editor is limited to 16,384 rows. The patch layer continues to enforce
  its 64 MiB text bound and the aggregate path's 100,000-addressed-cell bound.
- Joined values elsewhere remain presentation only. Empty values are visibly
  labelled in the field table, track matrix, inspector, and tooltips.
- This adds no Apply action or metadata writer.

## Alternatives considered

### Parse semicolon-separated text

Rejected. Semicolons are valid metadata content, escaping conventions vary by
application and container, and a joined display string cannot recover the
original vector.

### Treat blank text as field removal

Rejected. A present empty string and an absent field carry different intent.
Removal remains an explicit operation.

### Block in a nested dialog event loop

Rejected. The child editor uses `open()` and completion signals, keeping the
Qt event flow consistent with the non-modal Properties workspace.

## Consequences

- Repeated artist credits, genres, MusicBrainz identifiers, and arbitrary
  custom fields can be edited without silent delimiter corruption.
- Direct F2/type editing remains the faster scalar path; exact editing is a
  deliberate structured action.

## Validation

- The real-FLAC offscreen Properties regression replaces a mixed aggregate
  artist field with two reordered exact values and verifies both track cells
  plus shared undo.
- The same regression edits one repeated custom field, preserving an explicit
  empty first value, a semicolon inside another value, exact ordering, and
  per-track undo.

## Revisit when

- field creation allows an exact-value editor to create an absent union field;
- a preservation-proven writer turns drafts into revalidated operation plans.
