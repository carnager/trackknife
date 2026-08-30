# ADR-0039: Dynamic metadata field vocabulary

- Status: accepted
- Date: 2026-08-29
- Owners: Trackknife project
- Extends: ADR-0035 bounded metadata drafts
- Complements: ADR-0038 file-selection-driven metadata Properties
- Extended by: ADR-0040 ranked metadata field-name completion

## Context

Properties can edit any field already present in the selection and exposes
preferred missing fields, but arbitrary field creation still has no visible
command. Users also need an explicit field-removal control instead of having to
discover Delete. Both actions must retain the selected-file scope and use the
same sparse draft semantics as existing fields.

The selection may contain 100,000 occurrences. Extending its field vocabulary
must therefore not copy every immutable source document, and an older
background subset projection must remain safe if it finishes after a field is
added.

## Decision

- `Add field…` and Insert open a non-blocking name prompt. A valid arbitrary
  name is canonicalized by the ordinary metadata field rules, appended to the
  current Properties session, selected in the Field/Original/Draft table, and
  leaves focus on its Draft cell. The new field is initially missing on every
  selected occurrence.
- A canonical duplicate selects the existing row instead of creating an alias.
  Empty canonical names and names longer than 1,024 UTF-8 bytes are rejected
  visibly. The existing 4,096-field selection bound still applies.
- Extending the vocabulary is copy-on-write: selection copies share an
  immutable occurrence/baseline store while copying only the bounded field
  vectors and indexes. Published selections and in-flight background work stay
  immutable.
- The top file table preserves its selected rows while the backing model gains
  the new hidden field column. Field insertion must not change individual or
  bulk edit scope.
- `Remove field` and Delete stage explicit `remove_field` patches for the
  selected field rows across the currently selected files. Removing a field
  that was absent in the baseline cancels any staged addition for those files.
- A removed field row remains in the session vocabulary. Other files may still
  contain it, and keeping the row makes removal, revert, and undo predictable.
- These commands only change the in-memory draft. They add no Apply action or
  file-write capability.

## Alternatives considered

### Use a fixed dropdown of supported fields

Rejected. Trackbench preserves arbitrary metadata and cannot make a shipped
vocabulary the limit of what users can create.

### Rebuild the complete selection when adding a field

Rejected. Copying every source and sparse cell map for a vocabulary-only change
would violate the large-selection interaction budget and complicate lifetime
safety for background summaries.

### Delete the row from the table immediately

Rejected. Row disappearance would conflate removing a field from selected
files with removing a name from the session vocabulary, obscure partial-scope
results, and make revert harder to discover.

## Consequences

- Arbitrary fields can be added and removed without leaving the Picard-like
  Original/Draft workspace.
- Existing scalar and exact ordered-value editors, undo/redo, discard, and the
  future complete preview need no special patch type for newly created fields.
- Ranked completion enriches the same Add-field command without changing its
  draft semantics.

## Validation

- Qt-free tests prove copy-on-write vocabulary extension, canonical duplicate
  handling, limits, missing cells, and ordinary patch projection for a new
  field.
- The real-FLAC offscreen regression adds an arbitrary field, bulk-edits it,
  verifies the hidden backing column and preserved file selection, cancels the
  addition through Remove field, and removes an existing field from one file.

## Revisit when

- saved layouts make recently used field names persistent;
- field aliases need an explicit native-name mapping policy;
- a complete preview decides whether session-only missing rows should be
  collapsible.
