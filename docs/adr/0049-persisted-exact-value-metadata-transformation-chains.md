# ADR-0049: Persisted exact-value metadata transformation chains

- Status: accepted
- Date: 2026-08-30
- Owners: Trackknife project
- Extends: ADR-0048 versioned previewed metadata transformation chains

## Context

ADR-0048 established a Qt-free schema-1 chain and a preview-before-stage safety
boundary, but Properties could build only an ad hoc chain. The next M5 slice
needs reusable named definitions and the common exact-value operations that do
not require matching or capture grammars. Persistence must retain action and
multi-value order without coupling durable data to a C++ variant layout.

## Decision

- Schema 1 adds four typed actions without changing the meaning of its existing
  actions: add exact values, copy a field, split values by an exact separator,
  and join values with an exact separator.
- Add appends every supplied value in order, including duplicates and empty
  values. It creates a missing target and performs no deduplication.
- Copy mirrors the source field's complete ordered value state. A missing source
  removes the target, so the target matches the source's missing state rather
  than retaining stale data.
- Split applies one non-empty exact separator independently to every current
  value, then flattens the components in source order. Leading, trailing, and
  adjacent separators produce explicit empty components; no trimming or
  delimiter interpretation occurs.
- Join concatenates all current values in order and writes exactly one value.
  Its separator may be empty. Split, join, and the existing per-value actions
  are no-ops for a missing target.
- Chain validation is available independently of selection planning. It checks
  every typed payload and compiled formatting expression using the same bounded
  schema rules; action literal, separator, and expression text is limited to
  1 MiB in addition to the existing chain limits.
- Reversible SQLite migration 9 stores at most 256 saved chains. A saved chain
  has a non-nil stable ID, a non-empty exact UTF-8 name that is unique as bytes,
  schema version, ordered actions, and ordered literal values. Normalized child
  rows preserve both orders through cascading replacement and deletion.
- Persisted action kinds use explicit schema codes 0 through 9. They are not a
  serialized `std::variant` index, and implementation reordering cannot silently
  reinterpret stored data. Formatting source and its full dialect identity are
  persisted; executable callbacks or host-language code are not.
- Load rejects malformed order, unknown action data, incomplete dialect state,
  and any chain that fails core validation. Upsert and delete are atomic. Exact
  duplicate names fail with a conflict and preserve the preceding transaction.
- The existing serialized persistence worker owns load, save/update, save-as-new,
  and delete calls. Properties exposes those operations in the transformation
  editor, loads a saved definition back into the reorderable step list, and
  still requires a fresh immutable transformation preview before staging.
- Import/export, match/replace/remove-matching actions, numbering, allowlists,
  cue/artwork actions, intermediate-step display, and the separate capture-
  pattern grammar remain later slices.

## Alternatives considered

### Persist one opaque serialized chain blob

Rejected. Ordered child tables allow strict structural validation and migration
without inventing a second object encoding, while keeping arbitrary UTF-8 text
as exact BLOB values.

### Serialize the C++ variant index

Rejected. Adding or reordering implementation alternatives would silently
change durable behavior, violating the versioned language contract.

### Leave the target unchanged when a copy source is missing

Rejected. A copy action is defined as mirroring field state; retaining an old
target would make missing and present-empty source states indistinguishable and
would leave stale metadata.

### Drop empty split components or deduplicate added values

Rejected. Both would reinterpret exact ordered metadata and prevent reversible,
predictable preparation chains.

## Consequences

- Common reusable cleanup chains now survive restart and can express lossless
  ordered add/copy/split/join operations.
- Saved names are deliberately exact and case-sensitive. A later presentation
  layer may offer friendlier conflict guidance without changing identity.
- The Properties step editor creates one literal for each set/add step, while
  persisted and core schema data continue to support ordered multi-value
  payloads.
- Saved definitions remain inert data. Loading or saving one does not stage a
  draft and never performs file I/O.

## Validation

- Qt-free transformation tests prove append order, duplicate and empty-value
  retention, missing-source copy removal, exact split empty components, join
  order, validation failures, and per-cell bounds.
- Repository tests round-trip every explicit action code and ordered payload,
  reject duplicate names transactionally, replace one stable identity, survive
  restart, cascade deletion, and report absent deletion.
- Offscreen Properties coverage saves a two-step chain, closes the editor,
  reloads the saved definition, previews its final cells, and stages it as one
  undoable draft operation.

## Revisit when

- an interoperable import/export envelope is specified;
- match rules or capture patterns require their own versioned grammars;
- numbering needs album/disc/selection scope semantics;
- saved-chain rename/history or cross-device synchronization is required.
