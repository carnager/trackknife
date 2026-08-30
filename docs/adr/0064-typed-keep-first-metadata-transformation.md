# ADR-0064: Typed keep-first metadata transformation

- Status: accepted
- Date: 2026-08-30
- Owners: Trackknife project
- Extends: ADR-0048 versioned previewed metadata transformation chains and
  ADR-0049 persisted exact-value transformation chains

## Context

Keeping a fixed prefix is a common tagging operation. In particular, date tags
often contain a complete value such as `2024-08-30` while a user wants only the
four-character year. `tkfmt-1` can express that operation with `$left`, but a
routine metadata edit should not require opening the advanced formatting
action or remembering expression syntax.

The exposed action still needs exact persisted semantics. “Characters” must
not mean bytes, short values must not become errors, and a multi-value field
must not be flattened while it is transformed.

## Decision

- Schema-1 transformation chains gain the typed
  `MetadataKeepFirstCharactersAction`.
- The action keeps at most the first configured number of Unicode scalar
  values from every existing value of its target field. It preserves value
  order, duplicates, empty values, and the shape of a multi-value field.
- A value shorter than the requested count remains exact. A missing target
  field remains missing. Invalid UTF-8 fails the complete transformation plan.
- The count is an integer from 1 through 1,000,000. The editor exposes it as
  **Characters to keep** and defaults to 4 without assigning date-specific
  meaning to the action.
- This is exact prefix extraction, not date parsing or normalization. For
  example, applying count 4 to `DATE=2024-08-30` produces `2024`; it does not
  validate that the prefix is a year.
- The action participates in ordinary chain order and uses the same immutable
  preview, one-transaction staging, checked automatic-chain composition, and
  final fresh write-plan boundary as every other metadata transformation.
- Reversible SQLite migration 17 reserves persisted action code 14 and stores
  the count in the existing first numeric argument. Older actions retain their
  meanings. Downgrade to schema 16 refuses while any code-14 row remains.

## Alternatives considered

### Require a `tkfmt-1` formatting action

Rejected as the only UI. `$left(%date%,4)` remains available for advanced
derived-value work, but prefix extraction is common and narrow enough to offer
as an explicit, discoverable action.

### Count UTF-8 bytes

Rejected. Byte truncation could split an encoded character and create invalid
metadata. Unicode scalar counting is deterministic and matches the existing
formatting engine's character-oriented prefix behavior.

### Add a year-only date action

Rejected. Trackbench must not silently guess a date grammar. A reusable prefix
action states exactly what it does and is useful for fields other than `DATE`.

## Consequences

- A saved tagging chain can reduce full dates to year prefixes without a
  hand-written expression.
- The action name and count remain visible in the saved-chain editor and final
  change preview.
- Grapheme clusters consisting of several Unicode scalar values may be cut
  between scalars. A future grapheme-aware operation would require a separate
  typed action and persisted contract.

## Validation

- Qt-free transformation tests cover Unicode input, multi-value fields, short
  values, and invalid counts.
- Persistence restart coverage round-trips action code 14 and its count through
  schema 17.
- Offscreen workspace coverage saves and reloads the exposed action, previews
  `2024-08-30` to `2024`, stages it with the rest of a chain, and carries it
  through checked automatic write-plan composition.

## Revisit when

- a date-aware validation or normalization action has concrete requirements;
- users need grapheme-cluster rather than Unicode-scalar prefix semantics;
- capture-pattern import/export needs a mapping for fixed-width fields.
