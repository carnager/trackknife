# ADR-0050: First-character metadata capitalization

- Status: accepted
- Date: 2026-08-30
- Owners: Trackknife project
- Extends: ADR-0048 versioned previewed metadata transformation chains and
  ADR-0049 persisted exact-value metadata transformation chains

## Context

The transformation editor exposes whole-value lowercase and uppercase, but a
common cleanup operation—uppercasing only the first character—otherwise
requires a `tkfmt-1` expression. That makes a basic metadata operation obscure
and saves it as a generic formatting step instead of preserving its intent.
The word “capitalize” is also ambiguous: it can mean first character, every
word, locale-sensitive title case, or uppercasing followed by lowercasing.

## Decision

- Schema-1 transformation chains add a typed `capitalize_first` per-value
  action. The editor exposes it as **Capitalize first character**.
- The action applies locale-independent simple Unicode uppercase mapping to
  the first Unicode scalar value of each existing metadata value. It leaves
  every remaining character byte-for-byte unchanged.
- Empty values remain empty. Multiple values retain their count and order. A
  missing target is a no-op, matching the other per-value transformations.
- This is not word title-casing and does not lowercase any existing text.
- Persisted action code 10 identifies the action independently of C++ variant
  order. Reversible SQLite migration 10 widens the action-kind constraint; its
  down migration refuses to discard chains that contain code 10.

## Alternatives considered

### Require a formatting expression

Rejected for the normal workflow. The expression remains possible, but a
basic operation should be discoverable and should retain typed intent in saved
chains.

### Call the action “Title case”

Rejected. Word boundaries and locale-specific title rules are not defined by
this action, and that label would promise behavior it does not provide.

### Lowercase the remainder of each value

Rejected. It would destructively reinterpret intentional capitalization such
as `two WORDS`; users can compose lowercase and first-character capitalization
when that result is desired.

## Consequences

- Users can add, reorder, preview, save, and reload first-character
  capitalization like every other transformation step.
- The explicit label documents the narrow behavior at the point of use.
- An empty capitalization preview distinguishes values that already start with
  their uppercase form from missing target fields, rather than reporting only
  a generic absence of changes.
- A future title-case operation needs separately specified word-boundary and
  locale behavior rather than silently changing this action.

## Validation

- Qt-free tests cover non-ASCII first characters, unchanged remainder text,
  explicit empty values, and ordered multi-values.
- Repository tests round-trip action code 10 through migration 10.
- Offscreen Properties coverage verifies the exposed action label and updated
  transformation catalog.

## Revisit when

- a deterministic word-title-case policy and compatibility corpus are defined;
- the simple Unicode case-mapping contract changes under an explicit version.
