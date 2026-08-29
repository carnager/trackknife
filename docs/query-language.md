# Library query language

## Goal and status

Trackknife needs fast simple search, structured filters, saved queries, and live
autoplaylists. foobar2000 query syntax is the proposed compatibility baseline
because it composes naturally with title formatting and is already familiar to
the target audience. Unlike title formatting, strict 1:1 query compatibility is
still a proposal rather than an established requirement.

Keep the parser versioned as `fb2k-query`. Do not combine it with the title-
format parser: query syntax has separate quoting, keywords, precedence, types,
and indexing needs.

## Simple search

A string containing no structured operator performs a library-wide word search.
All words must occur somewhere in indexed metadata and/or the path. Define and
test tokenization, punctuation, Unicode normalization, diacritic handling, and
word boundaries; these are major user-visible compatibility details.

## Structured expressions

### Text

```text
artist HAS radiohead
artist IS "Miles Davis"
* HAS "blue train"
ALL
```

- `field HAS string`: every word from the string occurs within at least one
  value of the named field.
- `field IS string`: at least one value equals the string.
- `* HAS string`: search all indexed fields/path and remain composable.
- `ALL`: the complete library.

For multi-value fields, `IS` operates on each value, not on a display-joined
string. This is why direct indexed field access must remain distinct from an
evaluated title-format expression.

### Numeric

```text
rating GREATER 3
tracknumber LESS 10
discnumber EQUAL 2
```

Operators are `GREATER`, `LESS`, and `EQUAL`, using integral comparison and the
compatibility conversion rules established by reference tests.

### Existence

```text
genre PRESENT
catalognumber MISSING
```

Presence must distinguish a missing field from a field whose value happens to
display as `?`.

### Time

The foobar-style inventory includes:

```text
time1 AFTER time2
time1 BEFORE time2
time1 SINCE time2
time1 DURING time2
time DURING LAST 30 DAYS
```

Singular and plural seconds, minutes, hours, days, and weeks are supported.
Exact semantics of `SINCE`, `DURING`, partial dates, local time zones, and
daylight-saving boundaries require reference fixtures. Trackknife should store
statistics timestamps in UTC and evaluate human calendar expressions in an
explicit user zone.

### Logic and grouping

```text
artist IS A AND date GREATER 2000
genre IS Jazz OR genre IS Fusion
NOT genre PRESENT
(artist IS A OR artist IS B) AND rating GREATER 3
```

Keywords are uppercase in the reference syntax and comparisons are normally
case-insensitive. Parentheses control evaluation. Determine precedence with
tests even if the UI formatter always emits explicit parentheses.

### Sorting

Append one sort clause to the complete filter:

```text
ALL SORT BY %album artist%|%date%|%album%|%discnumber%|%tracknumber%
genre IS Jazz SORT DESCENDING BY %date%
```

Supported forms are `SORT BY`, `SORT ASCENDING BY`, and
`SORT DESCENDING BY`. The sort expression is title formatting and should be
compiled once. Sorting belongs after filtering and must be stable, with a
deterministic track-identity tiebreaker where persistence requires it.

## Title-format expressions inside queries

The reference treats a left-hand field expression containing `#`, `$`, or `%`
as title formatting. Expressions containing spaces/functions should be enclosed
in double quotes to avoid confusion with query parentheses:

```text
"$strcmp(%artist%,%album artist%)" EQUAL 1
"$info(codec)" IS FLAC
```

Direct field access is preferable and optimizable. It preserves multi-value
semantics and can use database indexes. An arbitrary formatting expression is a
per-row computation unless the planner can prove/index it. The UI should warn
when a saved query forces a full library scan.

## Grammar sketch

```ebnf
query       = "ALL" | expression [ sort_clause ] | simple_text ;
expression  = or_expr ;
or_expr     = and_expr { "OR" and_expr } ;
and_expr    = unary { "AND" unary } ;
unary       = [ "NOT" ] primary ;
primary     = "(" expression ")" | predicate ;
predicate   = lhs text_op string
            | lhs numeric_op integer
            | lhs existence_op
            | time_expr time_op time_expr
            | time_expr "DURING LAST" count unit ;
sort_clause = ("SORT BY" | "SORT ASCENDING BY" |
               "SORT DESCENDING BY") title_format_source ;
```

This sketch is not normative. Quoted strings, escaped quotes, title-format
boundaries, keywords embedded in strings, and time expressions need a formal
lexer and conformance tests.

## Planner and index requirements

Parse into a typed AST, then plan separately:

- direct field equality/existence uses normalized multi-value indexes;
- full-text search uses a dedicated full-text index over selected fields/path;
- technical properties use indexed typed columns where common;
- statistics use numeric/timestamp indexes;
- title-format predicates fall back to candidate evaluation;
- boolean nodes should reorder safe indexed predicates for selectivity without
  altering errors or visible semantics;
- results stream in pages and remain cancellable.

Store normalized lookup forms separately from original tag text. Never destroy
the original spelling merely to make search case-insensitive.

## Autoplaylists

An autoplaylist is a named, read-only membership view backed by:

- query source and dialect version;
- parsed/compiled revision;
- optional fixed sort expression and direction;
- visible invalid/error state;
- update policy and last successful evaluation.

Membership updates when relevant library fields, technical info, statistics,
or paths change. Users cannot manually add/remove items without converting the
view to a static playlist or changing its query. They can enqueue or copy its
current results.

Incremental invalidation should derive field dependencies from the query AST and
title-format AST. A query depending only on `GENRE` does not need reevaluation
when artwork changes.

## Query builder UI

Power users need raw syntax; other users need a structured builder. Both edit
the same AST/source and show:

- live result count and sample rows;
- syntax errors with spans;
- fields and operators appropriate to field type;
- index/full-scan cost hints;
- explicit grouping;
- a title-format sandbox for calculated predicates/sort keys;
- save as search, autoplaylist, or reusable filter.

## Conformance cases

Test casing, quoting, operator-like words inside values, whitespace, precedence,
multi-values, empty/missing fields, numeric prefixes, date boundaries, title-
format predicates, sort stability, simple-search tokenization, and large-library
query plans. Reference results must state the foobar2000 version used.

