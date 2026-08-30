# ADR-0053: Exact-value matching and selection-order numbering

- Status: accepted
- Date: 2026-08-30
- Owners: Trackknife project
- Extends: ADR-0048 versioned previewed metadata transformation chains and
  ADR-0049 persisted exact-value transformation chains

## Context

Schema-1 transformation chains can already set, append, remove, copy, format,
split, join, and change the case of values. Two common bulk-tagging tasks still
require awkward workarounds: changing or deleting only one value from a
multi-value field, and assigning a predictable sequence to selected files.

“Matching” can imply regular expressions, globs, normalization, locale-aware
comparison, or a separate capture grammar. Persisting any of those meanings
without an explicit contract would make saved chains unstable. Album-aware
numbering also needs decisions about grouping, `TOTALTRACKS`, discs, and
title-format expressions that are larger than the basic ordered operation.

## Decision

- Schema-1 chains gain three typed actions:
  `MetadataRemoveMatchingValuesAction`,
  `MetadataReplaceMatchingValuesAction`, and
  `MetadataNumberSelectedItemsAction`.
- Matching compares the complete valid-UTF-8 byte sequence. It is
  case-sensitive and performs no trimming, Unicode normalization, substring
  search, globbing, regular expression, or locale transformation. An explicit
  empty value is a valid match.
- Remove-matching deletes every equal value while retaining the order and
  duplicates of every nonmatching value. If no values remain, the field becomes
  missing. A missing target is unchanged.
- Replace-matching substitutes every equal value with the complete bounded,
  non-empty ordered replacement sequence. Each match expands independently;
  nonmatching values retain their exact position. An empty replacement value
  is still a present value, not field removal. The initial editor supplies one
  literal replacement, while the core and persisted representation preserve
  the ordered-list boundary.
- Selection-order numbering writes one decimal value to the target field for
  each planned item. The first receives `start`, then each following item in
  ascending captured file/item order receives the next integer. This is table
  order, not the order in which rows were clicked.
- `start` is bounded to 1–1,000,000,000. `padding` is bounded to 0–32 and is a
  minimum decimal width; it adds leading zeroes but never truncates a larger
  number. Numbering replaces the target's complete value list with one value.
- This slice does not infer albums, reset groups, write `TOTALTRACKS`, or assign
  disc numbers. Grouped numbering and title-format grouping remain a separate
  extension that must preview group boundaries and totals explicitly.
- All three actions participate in ordinary chain order. Later actions see
  their results, one-off use requires the exact transformation preview before
  staging, and checked automatic chains still operate only on the temporary
  pre-write-plan draft.
- Reversible SQLite migration 12 reserves stable persisted action codes 11
  (remove exact match), 12 (replace exact match), and 13 (selection-order
  number). Match text uses the existing exact BLOB argument; replacement values
  use ordered child rows; two nullable integer columns store numbering start
  and padding. Older action rows migrate without reinterpretation.

## Alternatives considered

### Treat the match as a regular expression

Rejected for schema 1. Regex dialect, escaping, invalid-pattern behavior, and
resource limits require a separately identified persisted contract. Exact
matching is useful on its own and cannot surprise a saved chain after an
engine upgrade.

### Make an empty replacement remove a value

Rejected. Metadata already distinguishes a missing field from a present empty
value. Remove-matching expresses deletion without overloading that distinction.

### Implement album grouping in the first numbering action

Rejected. Silent folder or tag heuristics would hide consequential grouping
decisions. The basic action provides deterministic sequencing while the later
grouped operation can expose grouping and `TOTALTRACKS` in its preview.

### Number by row-click order

Rejected. Click history is ephemeral and hard to review. Captured file order is
visible, deterministic, and stable across preview and automatic execution.

## Consequences

- Common cleanup and renumbering no longer require `tkfmt-1` workarounds.
- Saved matching actions have intentionally narrow semantics that remain
  independent of the future capture-pattern grammar.
- Exact empty and multi-value states remain lossless through evaluation,
  persistence, preview, staging, and the final write plan.
- The first numbering action is deliberately not a complete album-numbering
  workflow; its UI names the ordering rule instead of implying grouping.

## Validation

- Qt-free transformation tests cover case-sensitive exact removal, full-field
  deletion, ordered replacement expansion, padded consecutive numbering, and
  a later formatting step observing all three results.
- Validation tests reject an empty replacement sequence, zero start, and
  padding above the bound.
- Persistence restart coverage round-trips all three typed actions and their
  exact ordered and numeric payloads through migration 12.
- Offscreen editor coverage verifies all three exposed choices and carries a
  numbering step through save/reload, compact preview, one-transaction staging,
  undo/redo, checked automatic execution, and repeated immutable write-plan
  preview.

## Revisit when

- grouped album/disc numbering and explicit `TOTALTRACKS` are implemented;
- imported chains need an editor for multi-value replacement sequences;
- a versioned regex/glob action is justified by concrete workflows;
- the separate capture-pattern grammar is ready for implementation.
