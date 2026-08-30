# ADR-0068: Versioned metadata capture patterns

- Status: accepted
- Date: 2026-08-30
- Owners: Trackknife project
- Extends: ADR-0048 declarative metadata transformation chains and ADR-0049
  saved transformation definitions

## Context

Trackbench can format metadata into text, but it cannot yet perform the inverse
operation: extracting several metadata fields from a filename, path, formatted
string, or existing field. This language cannot be `tkfmt-1`. Formatting has
functions and conditional sections and produces one string, while capture
patterns recognize one whole input and can produce several ordered values.

Picking a greedy split would make patterns such as `%artist% - %title%`
silently depend on the first or last repeated delimiter. Persisted chains also
need enough dialect and source identity to retain their meaning after restart.

## Decision

### Independent `tkcapture-1` grammar

- Capture patterns persist dialect `tkcapture`, dialect version 1, and compiler
  schema 1. They are not accepted as title-format expressions.
- Literal text matches exactly and the match is anchored to the complete source.
- `%field%` captures an ordered value for the named semantic/freeform field.
  Repeating a field appends another value in pattern order.
- `%%` captures and discards one portion. A backslash quotes the following
  Unicode scalar, including a literal `%` or backslash. A trailing backslash,
  an unclosed capture, an empty named capture, or a pattern without a named
  capture is invalid.
- Captures may be empty. This preserves the metadata model's distinction
  between a missing field and one present empty value.
- The matcher accepts only valid UTF-8 input and considers only Unicode-scalar
  boundaries. It returns unmatched, unique, or ambiguous. Evaluation never
  selects among two possible boundary assignments, even when the differing
  portion would be discarded.
- Compilation and matching are bounded by input bytes, token count, capture
  count, and match work. The matcher stops after proving a second solution.

### Transformation sources and results

- One typed transformation action applies a capture pattern to one of four
  source kinds: filename, full raw path, a scalar `tkfmt-1` expression, or an
  existing metadata field.
- Filename mode removes the final extension. Each literal `/` in the compiled
  pattern includes one additional parent component, so a pattern can address
  the required suffix of a path without depending on an implicit library root.
  Full-path mode retains the exact path and extension.
- Field mode evaluates each ordered source value and appends captures in source
  value then pattern order. Formatted mode evaluates against the current
  working document. All captures are computed before any target is replaced,
  so using a target as the source is deterministic.
- Every named target is resolved through the adapter's explicit field mapping:
  conventional names become semantic targets and every other spelling retains
  an exact freeform native identity. Similar spellings are never inferred as
  aliases. Later chain actions see all captured results through the applicable
  identity. Unmatched input, ambiguous input, missing
  source fields, invalid path text, and resource-limit failures block preview
  with the action and item identity; no partial capture result is staged.
- Schema 20 adds persisted action kind 16. Its existing `target_field` payload
  stores the source argument (empty for filename/full-path), `argument` stores
  the capture pattern, the dialect columns store `tkcapture-1`, and
  `integer_argument` stores the source kind. Downgrade refuses while a kind-16
  row exists.

## Alternatives considered

### Reuse `tkfmt-1`

Rejected. Output formatting and input recognition have different syntax,
ambiguity, and result cardinality. Reinterpreting persisted `tkfmt-1` text
would also violate its versioning contract.

### Choose the first or last delimiter greedily

Rejected. Repeated delimiters are common in real filenames and either policy
can write plausible but incorrect tags. Explicit ambiguity is safer and makes
the preview reproducible.

### Limit one capture action to one target field

Rejected. The primary workflow extracts artist, album, date, number, and title
from one source. Splitting that into separately parsed actions could select
inconsistent boundaries and would conceal whole-pattern ambiguity.

## Consequences

- Transformation planning now supports actions with several target cells while
  retaining the existing final-cell preview and atomic staging boundary.
- Filename patterns are portable across library roots, but a full absolute path
  must be requested explicitly when the root itself is meaningful.
- A literal percent must be quoted as `\%`; `%%` always means an ignored
  capture in this dialect.
- Native transformation-chain interchange remains separate work; SQLite saved
  chains now retain the capture action losslessly.

## Validation

- Qt-free grammar tests cover escapes, empty values, Unicode boundaries,
  unmatched input, repeated-delimiter ambiguity, ignored captures, and limits.
- Transformation tests cover filename parent traversal, full path, formatted
  and multi-value field sources, ordered repeated targets, later-action
  visibility, and all-or-nothing failures.
- Repository tests round-trip every source kind and prove schema-20 migration.
- Offscreen UI tests add, preview, stage, save, reload, and re-preview one
  multi-field filename capture step.

## Revisit when

- users need typed normalization inside captures rather than a following chain
  action;
- Windows paths become an execution target;
- a chain interchange schema needs a public serialization of `tkcapture-1`.
