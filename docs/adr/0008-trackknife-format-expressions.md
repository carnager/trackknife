# ADR-0008: Use a Trackknife formatting language instead of foobar2000 compatibility

- Status: accepted
- Date: 2026-08-23
- Owners: Trackknife project

## Context

Trackknife needs one expression language for configurable library-tree levels,
track and queue presentation, and conversion/file destination names. The earlier
plan made classic foobar2000 title formatting a 1:1 compatibility target. That
would require reproducing undocumented behavior, including a text value with a
separate truth flag, optional-section rules, implicit field remapping, permissive
integer edge cases, and version-specific quirks.

Importing existing foobar2000 scripts is not a product requirement. The useful
requirement is a familiar, powerful, predictable language shared by Trackknife's
own views and file-planning tools.

MusicBrainz Picard demonstrates an open implementation of the familiar
`%field%` and `$function(arguments)` shape. Its language is a design reference,
not a new compatibility target: Trackknife does not need Picard's metadata-
mutating tagging-script functions or every historical behavior.

## Decision

Define and own a versioned `tkfmt-1` formatting-expression dialect. It uses:

- `%field%` for context-provided fields;
- `$function(argument,...)` for a documented, deliberately small built-in set;
- backslash escaping for syntax characters;
- ordinary empty/non-empty string conditions;
- empty text for a missing field;
- explicit functions for ordered multi-value metadata;
- deterministic, side-effect-free evaluation.

The same parser and evaluator serve three initial hosts:

1. Library trees store an ordered expression per hierarchy level. Hierarchy is
   structural configuration, not inferred by splitting rendered text.
2. Track, playlist, and queue views store expressions for columns, group labels,
   and stable sort keys.
3. Conversion and file operations evaluate an expression to a relative path.
   A separate operation planner validates and sanitizes path components, rejects
   traversal and absolute paths, detects conflicts, and previews the exact plan.

Formatting expressions cannot write tags, files, library state, playlists, or
the queue. Metadata transformations remain declarative operation steps with
their own preview and transaction lifecycle. Functions such as Picard's `$set`
and `$unset` are therefore outside `tkfmt-1`.

Persisted expressions record their dialect, dialect version, source, compiler
schema, usage context, and optional human name. Future incompatible behavior
uses a new dialect version rather than reinterpreting saved expressions.

The existing module may retain the internal `titleformat` name during M1 to
avoid a low-value directory/API rename while the public terminology changes to
“format expressions.”

## Consequences

- M1 becomes smaller and directly supports Trackknife's actual workflows.
- The separate foobar2000 truth flag, optional sections, implicit remapping,
  renderer color functions, compatibility probes, and black-box gate are
  removed from the product contract.
- Existing parser, immutable-program, limits, cancellation, dependency,
  Unicode, batch, fuzzing, and diagnostic work remains useful.
- The foobar-derived corpus is retained only as historical research and no
  longer gates releases.
- Picard scripts are not promised to run unchanged. Similar syntax must not be
  marketed as Picard compatibility.
- Multi-value tree expansion needs an explicit bounded result model; scalar
  formatting must never create an accidental Cartesian explosion.

## Alternatives considered

- Continue exact foobar2000 compatibility. Rejected because script migration is
  not required and the black-box burden does not improve the target workflows.
- Promise exact Picard compatibility. Rejected because Picard tagging scripts
  intentionally mutate metadata and carry behavior unrelated to Trackknife's
  read-only presentation and path-generation needs.
- Use unrelated templating syntax. Rejected because the Picard/foobar shape is
  concise, familiar to music-library users, and already fits the implemented
  parser architecture.

## Validation

- A repository-owned executable corpus specifies every `tkfmt-1` construct.
- Unit and fuzz tests cover parsing, escaping, lazy control flow, Unicode,
  ordered multi-values, limits, cancellation, and concurrent batch evaluation.
- Host integration tests cover tree-level construction, queue fields, and
  conversion path preview independently of filesystem mutation.

## Revisit when

- real presets require a richer value type or explicit multi-value expansion;
- a credible migration demand justifies a separate `picard-*` or `fb2k` importer
  or dialect;
- a built-in function would require nondeterminism, I/O, or hidden mutation.
