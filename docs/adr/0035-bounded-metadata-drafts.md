# ADR-0035: Bounded metadata drafts and scalar grid editing

- Status: accepted
- Date: 2026-08-29
- Owners: Trackknife project
- Complements: ADR-0034 staged metadata selection grid
- Extended by: ADR-0037 exact ordered metadata value editor and ADR-0039
  dynamic metadata field vocabulary
- Result projection implemented by: ADR-0041 background metadata Draft
  projection

## Context

ADR-0034 made multi-track metadata inspectable but intentionally read-only.
The next M5 layer needs editing semantics that can later feed a complete plan
and preview without mutating the immutable read baseline or implying that a
format is writable.

Metadata is not a scalar dictionary. A cell may be absent, contain one value,
or contain an ordered vector of repeated values. Empty text, field removal,
and replacement by an empty-looking value cannot be silently conflated, and a
display separator must never become a parser.

## Decision

- `Trackknife::Metadata` owns a Qt-free sparse patch set over an immutable
  `StagedMetadataSelection`. A patch addresses one selected occurrence and one
  existing union field by selection index. Paths are source data, not draft
  identity, so duplicate occurrences and logical tracks remain independent.
- A patch is either `replace_values` with an explicit non-empty ordered vector,
  or `remove_field`. Restaging the exact baseline vector, or removing a field
  absent from the baseline, collapses to no patch. Removal is never inferred
  from a zero-length vector inside the domain model.
- Patch enumeration is deterministic by occurrence and field. The patch set
  can project the resulting field state and present count with the same exact
  common/mixed/missing/partial rules as ADR-0034. ADR-0041 runs that projection
  over immutable snapshots in a cancellable worker rather than recomputing a
  large selection on the UI thread after every keystroke.
- Drafts are bounded to 100,000 cells, 16,384 values per replacement, and 64
  MiB of replacement text. Direct scalar editing additionally limits one value
  to 4 MiB. UI undo history is bounded to 256 transactions and 64 MiB of patch
  text.
- Metadata cells support native inline editing by keyboard or double click.
  A one-value cell opens with that exact value selected. A missing cell opens
  empty. A repeated-value cell opens empty with an explicit “replace N exact
  values” prompt; typing commits one scalar replacement and does not parse the
  joined display text. Exact multi-value editing remains a separate next slice.
- Delete stages explicit field removal for selected metadata cells.
  `Ctrl+Z`/Redo operate on draft transactions, `Ctrl+Backspace` reverts selected
  cells to baseline, and Discard draft clears the complete draft and its undo
  history.
- Draft cells use a restrained highlight and italic text. While ADR-0041's
  projection is pending, Draft explicitly says that it is preparing the
  preview; the completed column reports the exact resulting common, mixed,
  missing, or partial state plus its staged-cell evidence.
- The value inspector shows exact Original and Draft rows with provenance or
  “staged draft” source. Closing a dirty workspace requires explicit discard.
- File writing remains disabled. There is no Apply button, adapter call,
  filesystem mutation, or update to Trackbench list metadata. A later complete
  immutable plan must re-read/revalidate physical sources, resolve multiple
  logical-occurrence edits that target one source, prove format preservation,
  journal execution, and verify results before commit becomes available.

## Alternatives considered

### Edit the baseline documents in place

Rejected. It would destroy the exact comparison source, make revert and
preview reconstruction expensive, and allow UI state to masquerade as freshly
read file authority.

### Treat an empty replacement as removal

Rejected. Absence and a present empty-looking value have different intent and
may map differently across formats. Destructive intent remains an explicit
patch kind.

### Parse joined cell text using semicolons or another separator

Rejected. Separators are valid metadata data, and display joining is lossy.
Repeated values stay ordered vectors and require an exact-value editor.

### Recalculate every edited column over all selected rows synchronously

Rejected. It would turn a keystroke into a potentially 100,000-row UI-thread
batch. ADR-0041 instead debounces a cancellable background projection and
rejects stale generations.

### Enable Apply with the draft model

Rejected. A sparse patch is only user intent. It is not a revalidated operation
plan and carries no format-specific preservation proof, journal, or recovery
story.

## Consequences

- Users can exercise fast spreadsheet-like scalar editing and destructive
  field intent without any risk to files.
- Repeated-value replacement is explicit but currently scalar through the grid;
  an exact ordered-value editor is still required for adding/reordering several
  values.
- Drafts disappear when discarded or when a dirty Properties workspace is
  explicitly closed. They are not persisted as list state.
- Complete Draft result projection operates on the same patch type rather than
  adding UI-only mutation semantics.

## Validation

- Qt-free tests cover no-op collapse, ordered multi-value replacements,
  explicit removal, deterministic ordering, field-state projection, revert,
  clear, invalid cells, and patch/value/text limits.
- The real-FLAC offscreen Properties regression edits a scalar through the
  keyboard editor, verifies staged roles/header/count and exact Original/Draft
  inspector rows, removes a repeated-value field with Delete, exercises
  undo/redo, discards the draft, and confirms that no Apply control exists.

## Revisit when

- the first preservation-proven writer consumes ADR-0042's complete plan;
- draft persistence across application restarts has a demonstrated user need.
