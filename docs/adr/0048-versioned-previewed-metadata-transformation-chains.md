# ADR-0048: Versioned previewed metadata transformation chains

- Status: accepted
- Date: 2026-08-30
- Owners: Trackknife project
- Extends: ADR-0008 `tkfmt-1`, ADR-0035 bounded metadata drafts, and ADR-0042
  revalidated metadata write plans

## Context

Properties can stage exact scalar and ordered-value edits, preview a freshly
revalidated physical write plan, and Apply preservation-qualified native-FLAC
changes. Repetitive preparation still requires one field operation at a time.
M5 calls for named ordered transformation chains where later actions observe
earlier results, including pure `tkfmt-1`-derived values, without creating a
second mutation path or allowing formatting expressions themselves to modify
metadata.

The first slice must establish the durable semantic boundary before adding the
full action catalog or saved-chain persistence. It must also remain honest
about exact multi-values, missing fields, explicit empty values, cancellation,
and the fact that a transformation preview is not a file-write preview.

## Decision

- A metadata transformation is Qt-free declarative data with an explicit chain
  schema version, user-visible name, and ordered typed actions. No executable
  host-language code or callback is part of a chain.
- Schema 1 initially supports setting one or more exact ordered values,
  removing a field, trimming ASCII whitespace from each existing value,
  Unicode simple lowercasing or uppercasing of each existing value, and setting
  one scalar value from a versioned `tkfmt-1` expression. The Properties editor
  exposes one literal value per set step in this slice; the core retains the
  exact ordered-value representation.
- A chain runs against the selected items' current projected draft, not only
  the captured baseline. Actions execute in listed order for each item, and
  every later action sees all earlier results. Per-value transforms preserve
  value count and order. Acting on a missing field is a no-op except for set or
  format actions.
- `tkfmt-1` gains a typed metadata-transformation host. It resolves the working
  document after prior actions; ordinary field access joins ordered values with
  `; `, while `$join` and `$getmulti` retain explicit multi-value access. The
  expression remains pure. Its scalar result becomes exactly one present value,
  including when that result is the empty string; removal requires a distinct
  remove action.
- A chain must be evaluated into an immutable preview before it can be staged.
  Preview rows retain the exact before/final value lists or missing state, the
  selected item, target field, and last action which wrote that target. Only
  final cells that actually change are emitted. The original chain and selected
  item indexes remain attached to the preview.
- Planning validates UTF-8, field canonicalization, schema and dialect identity,
  sorted unique item scope, and fixed limits of 100,000 items, 256 actions,
  100,000 addressed cells, 16,384 values per cell, 64 MiB of preview text, and
  1,024 bytes for a field or chain name. It is cooperative-cancellable and can
  run outside the UI thread.
- Staging rechecks every preview `before` value against the live projected
  draft, refuses duplicate or malformed cells, adds valid missing target fields
  to the session vocabulary, and applies all final cells through the existing
  sparse patch model as one bounded undo transaction. A stale preview changes
  nothing. Staging still performs no file I/O.
- After staging, transformations use the ordinary ADR-0042 fresh revalidation
  and ADR-0047 Apply path. They receive no shortcut around source conflicts,
  adapter capability, preservation verification, journaling, dependent-state
  refresh, or recovery.
- Saved-chain storage/import/export, exact add/copy/match/split/join actions,
  numbering, allowlists, cue/artwork actions, and intermediate-step display are
  later slices. Filename/path capture remains a separate versioned grammar and
  is never parsed as `tkfmt-1`.

## Alternatives considered

### Let formatting expressions assign fields

Rejected. It would make `tkfmt-1` stateful, make evaluation order observable in
hosts that only format text, and violate its versioned pure-language contract.

### Apply each step directly to the draft

Rejected. Users could not inspect the complete result, invalid expressions
would leave partial mutations, and one logical chain would require many undo
operations.

### Reuse the capture-pattern grammar for derived values

Rejected. Capture parses structured input into fields, whereas `tkfmt-1`
deterministically renders one scalar from an existing context. Combining them
would create ambiguous escaping and compatibility rules.

### Stage a preview even if the draft has changed

Rejected. Its displayed before/final relationship would no longer describe the
mutation being accepted. A fresh preview is cheap compared with silently
overwriting a newer edit.

## Consequences

- Trackbench now has the first useful bulk preparation chain while retaining a
  single draft, undo, write-preview, and Apply lifecycle.
- A transformation can address a field absent from the original union without
  restricting arbitrary field names.
- The first editor is intentionally an ad hoc chain builder. The schema is
  versioned now so saved named chains can be added without retrofitting
  semantics onto unversioned data.
- Preview reports the final net effect, not every intermediate value. The last
  writer column still explains which step produced each final cell.

## Validation

- Qt-free tests prove ordered actions, exact multi-value replacement, missing
  versus explicit removal, per-value transforms, derived values that observe
  both an existing draft and earlier chain steps, stable preview ordering,
  dialect/compile failure, limits, and cancellation.
- Offscreen Trackbench coverage builds a literal-set plus `tkfmt-1` chain,
  verifies the derived step sees the earlier title, stages two cells, undoes the
  complete chain with one command, redoes it, and obtains the ordinary ready
  physical write plan.

## Revisit when

- saved chains need a persistence and import/export schema;
- actions need typed source-field references, matching rules, separators, or
  numbering scope;
- an intermediate-step inspector is required for complex chains;
- capture-pattern planning is implemented as its own versioned parser.
