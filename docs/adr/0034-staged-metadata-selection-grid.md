# ADR-0034: Staged metadata selection states and read-only properties grid

- Status: accepted
- Date: 2026-08-29
- Owners: Trackknife project
- Complements: ADR-0033 typed local metadata read model
- Revised in part by: ADR-0036 aggregate metadata fields workspace
- Presentation superseded by: ADR-0038 file-selection-driven metadata Properties

## Context

ADR-0033 established an ordered, provenance-aware metadata document, but the
list view still exposed only a few display projections. M5 needs one coherent
multi-file workspace before it can add editing. The read surface must preserve
arbitrary and repeated values, make absence and disagreement explicit, remain
responsive for large selections, and avoid implying that any format is already
safe to write.

The selected rows are occurrences, not unique physical files. A CUE album may
therefore contain several logical tracks backed by one raw path, while a list
may contain the same track more than once. Collapsing those rows would break
selection order and make per-occurrence edits ambiguous.

## Decision

- `Trackknife::Metadata` owns a Qt-free immutable selection projection. Each
  selected occurrence retains its raw path, optional observed source revision,
  and baseline metadata document. The grid never deduplicates occurrences;
  distinct physical-source count is summary information only.
- Fields form a sparse union. A bounded preferred vocabulary appears first,
  including fields absent from every row, and arbitrary fields follow in
  first-seen selection/document order. Canonical lookup uses ADR-0033 rules.
- A field has exactly one selection state:
  - **missing** when no selected row contains an effective value;
  - **partial** when at least one, but not every, row contains it;
  - **mixed** when every row contains it but the exact ordered value vectors
    differ;
  - **common** when every row contains the same exact ordered value vector.
  Provenance does not by itself make equal values mixed, but remains visible on
  each cell.
- Trackbench exposes Properties from the Edit menu, the track context menu, and
  `Alt+Return`. It opens a non-modal read-only workspace. Selected occurrences
  are rows, metadata fields are columns, column headers expose aggregate state,
  and a lower inspector shows every exact ordered value plus provenance for the
  current cell. Joined cell text is display-only and never defines multi-value
  parsing.
- The grid uses a sparse `QAbstractTableModel`, no per-cell widgets, fixed
  default row geometry, interactive rather than content-scanning column
  sizing, and no line wrapping. Selection aggregation runs through a
  concurrent boundary after the dialog shell opens. Snapshot capture yields
  between short UI-thread slices so a large selection cannot delay that shell
  behind one monolithic document-copy batch.
- Projection is limited to 100,000 occurrences and 4,096 union fields. Limit
  failures remain visible in the non-modal shell rather than constructing a
  partial grid.
- This slice has no edit trigger, Apply button, patch object, or write path.
  Cached rows without source revisions are explicitly identified. A later
  mutation must freshly read and revalidate every physical source rather than
  treating this projection or a restored list snapshot as commit authority.

## Alternatives considered

### A per-track modal property form

Rejected. It hides selection-wide disagreement and turns bulk work into
repeated clicking.

### One aggregate row per field

Rejected as the primary surface. It makes common state compact but hides which
occurrence owns each exact value. The track-by-field matrix keeps alignment
visible; the inspector handles repeated values without overloading cell text.

### Enable editing before a preservation-proven writer exists

Rejected. An editable-looking surface or Apply action would promise mutation
semantics that no format adapter can yet uphold. Sparse staged patches,
preview, revision revalidation, journal, and preservation tests arrive before
writes are enabled.

### Materialize a dense item-by-field matrix

Rejected. Missing metadata is common and the product corpus reaches 100,000
logical tracks. Sparse cells plus a virtual model bound memory to present
effective fields rather than the Cartesian product.

## Consequences

- M5 now has a truthful bulk inspection surface on which keyboard editing,
  saved field layouts, and sparse staged patches can be layered without
  replacing the model.
- Logical tracks sharing a physical file remain separate rows while the summary
  communicates the smaller source count.
- A field that is absent on some rows is `partial` even when its present values
  also disagree. Per-row cells and the inspector retain the evidence; richer
  compound summaries may be added without changing the four persisted states.
- Opening Properties does not perform tag parsing or file I/O. It consumes the
  metadata already supplied by Trackbench's background read path.

## Validation

- Qt-free tests cover preferred and arbitrary field order, exact value-vector
  comparison, provenance-independent common values, all four states, canonical
  aliases, duplicate raw paths, empty selections, and explicit limits.
- An offscreen Trackbench test opens two real FLAC fixtures, selects both,
  invokes Properties, and verifies non-modal/read-only behavior, mixed,
  partial, and missing headers, an arbitrary field, a MusicBrainz field, and
  exact repeated values with embedded provenance in the inspector.

## Revisit when

- sparse staged patches and undo are introduced;
- saved field layouts are specified;
- fresh-read conflict presentation and the first preservation-proven writer
  are ready;
- large-selection snapshot capture gains a reusable immutable workspace
  snapshot service.
