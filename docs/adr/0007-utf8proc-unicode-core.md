# ADR-0007: Use utf8proc for deterministic core Unicode operations

- Status: accepted
- Date: 2026-08-23
- Owners: Trackknife project

## Context

Title formatting, metadata lookup, queries, sorting, and filename previews need
Unicode-aware operations without coupling the core to Qt. C and C++ locale
facilities do not provide deterministic UTF-8 normalization, casing, grapheme,
or display-width behavior. Compatibility also requires distinguishing simple
one-code-point casing from full case folding and normalization.

## Decision

Use the system `libutf8proc` library, version 2.9 or newer, behind
Trackknife-owned core interfaces. It is dynamically linked and is not exposed
in public Trackknife headers.

Each caller must choose its operation explicitly; no implicit normalization or
locale-sensitive comparison is allowed. `tkfmt-1` `$eqi()`, `$lower()`, and
`$upper()` use utf8proc simple one-code-point mappings without normalization or
multi-code-point full folding. ADR-0008 supersedes the narrower compatibility
profile that was originally selected for foobar2000 `$stricmp()`.

## Alternatives considered

- ICU is more comprehensive but substantially larger than the operations the
  current core needs.
- Qt string APIs would couple core behavior to the GUI toolkit and Qt version.
- C/C++ locale APIs are environment-dependent and insufficient for UTF-8.
- Maintaining custom Unicode tables would add avoidable correctness and update
  risk.

## Consequences

- Core Unicode behavior is deterministic for a recorded utf8proc/Unicode-data
  version and remains independent of Qt.
- Packagers need `libutf8proc >= 2.9`; Ubuntu 24.04 satisfies this baseline.
- Unicode-data upgrades can change edge-case results and therefore require
  corpus runs and release-note review.
- Search and collation may intentionally use richer operations than the stable
  simple mappings specified for formatting expressions.

## Validation

- Unit tests cover simple casing, absence of full folding and normalization, and
  invalid UTF-8 rejection.
- The `tkfmt-1` corpus records the specified Unicode behavior and utf8proc
  version baseline.
- Sanitizer, fuzz, and Ubuntu-baseline builds exercise the linked library.

## Revisit when

- required full case folding, normalization, or collation exceeds utf8proc's
  current use in `tkfmt-1`;
- the Ubuntu baseline can no longer supply the selected version.
