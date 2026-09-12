# ADR-0150: tkq-1, the library query dialect

Date: 2026-09-12

Status: accepted

## Context

Area 2 needs a query language before anything else in it can exist:
structured filters, saved searches, and autoplaylists all persist and
re-evaluate queries. The roadmap blocks the choice on reconciling the
old query sketch (docs/query-language.md, framed as `fb2k-query` with
foobar2000 conformance references) against compatibility.md, which
rejects external query-dialect compatibility outright and demands that
any dialect be its own versioned specification. The feature matrix
already records the sketch as non-normative.

The reconciliation follows the ADR-0008 precedent exactly: tkfmt-1
kept the familiar foobar-style *surface* while making Trackknife's own
specification normative, with no promise that foreign scripts run.

Two substrate facts constrain v1. The index (migration 28) holds only
title/artist/album/date/disc/track/release id plus two lowercased
search blobs — genre, ReplayGain, MusicBrainz track ids, and every
technical property are invisible to queries, even though the scan
already probes each file and builds a full metadata document before
discarding both. And no statistics (play counts, timestamps) exist
anywhere, so time operators have nothing to query.

## Decision

### The dialect

Trackknife defines its own query language: dialect `tkq`, version 1,
compiled by a dedicated lexer/parser (never the tkfmt parser), with
this specification normative and no external compatibility promised.
The surface stays foobar-inspired because it reads naturally and is
familiar to the target audience:

```text
query      = "ALL" | simple-words | expression [ sort-clause ]
expression = or-expr ; OR < AND < NOT < parentheses
predicate  = lhs "HAS" string | lhs "IS" string | "*" "HAS" string
           | lhs ("GREATER"|"LESS"|"EQUAL") integer
           | lhs ("PRESENT"|"MISSING")
sort-clause = "SORT" ["ASCENDING"|"DESCENDING"] "BY" tkfmt-source
```

Keywords are uppercase and reserved; bare and double-quoted strings
with `""` escaping; an `lhs` containing `%`, `$`, or `#` is a tkfmt-1
expression predicate (quoted when it contains spaces) whose evaluated
text feeds the operator — bare truthiness is not a predicate. The
sort expression is tkfmt-1 source to end of input, compiled in the
sort context like list sorting (ADR-0127).

`simple-words` makes the common case effortless: when the token
stream contains no reserved keyword, parenthesis, quote, or `*`, the
query is an all-word match equivalent to `* HAS <words>`. Anything
else parses strictly and reports positioned diagnostics — a malformed
structured query never silently degrades into a word search.

### Semantics

Comparison text is normalized with the same simple Unicode
lowercasing the index already uses (`core::unicodeSimpleLower`);
original spelling is stored beside the normalized form and is never
destroyed. Multi-value fields: `IS` matches when any single value
equals the string; `HAS` when every word occurs in at least one value
(each word independently); `PRESENT`/`MISSING` test whether the field
carries any value; numeric operators compare the leading integer of
any value. `date GREATER|LESS|EQUAL n` compares the year prefix of
the indexed date. Technical pseudo-fields resolve to typed index
columns: `codec` (text), `samplerate`, `bitspersample`, `channels`,
`length_ms` (numeric).

Deferred beyond v1, recorded here so the spec stays honest: time
operators (`AFTER`/`BEFORE`/`SINCE`/`DURING`, `DURING LAST`) until
statistics exist in the index; diacritic folding; regular
expressions; path-targeted operators.

Bounds: source ≤ 4096 bytes, ≤ 256 AST nodes, ≤ 64 words per string;
embedded tkfmt programs run under the list-edit evaluation limits.

### Index substrate (migration 30)

Queries evaluate against the cached index only — never the filesystem
(ADR-0116). Migration 30 widens the substrate:

- `local_library_fields(raw_path, canonical_name, position, value,
  value_lower)` — one row per tag value in demuxer order, original
  bytes and normalized form side by side, keyed to the track row with
  cascade delete, indexed on `(canonical_name, value_lower)`. Bounded
  per file: at most 64 distinct field names, 32 values per name,
  values above 2048 bytes are not indexed.
- Typed technical columns on `local_library_tracks`: `codec_name`,
  `sample_rate`, `bits`, `channels`, `duration_ms`, retained from the
  probe the scan already runs. This also closes the index half of the
  ADR-0142 "probe-technicals retention" follow-up (the Find bar is
  untouched).

The migration is DDL-only. Existing rows gain field rows and
technicals on their next explicit Refresh; until then structured
predicates simply see those tracks as missing the fields, and the
documentation says so. Committed tag writes keep the field table in
step through the same transactional hook that already updates the
track row (`refresh_library_source`).

### Planner and entry points

Compilation (`compileTkq`) lives in a new Qt-free `src/query` module
and yields the AST plus the exact source, dialect name/version, and
compiler schema — the persistence contract of compatibility.md, so
later saved searches store precisely what they must. The planner in
persistence translates indexable predicates into SQL over the field
table and typed columns (always `available=1`); tkfmt expression
predicates evaluate per candidate row from the row's field and
technical data. AND-conjuncts split between the two sides; an OR/NOT
subtree containing a tkfmt predicate evaluates wholly as residual.
`LocalLibrary::filter` pages track rows (limit ≤ 200) and
`filter_paths` resolves the full match set under the existing 100k
cap, both cancellable.

### UI

A checkable query toggle beside the library search field switches the
field into tkq mode: the debounced text compiles, diagnostics appear
inline, and results populate the tracks group. Enter commits through
the unchanged ADR-0140 snapshot-tab path. Saved re-evaluating
searches, autoplaylists, a query builder, MPD translation, and
logical-track indexing are deliberately later packages.

## Consequences

- The dialect question is settled the same way formatting was: one
  spec, versioned, ours. docs/query-language.md becomes the normative
  tkq-1 specification and drops the foobar conformance framing.
- The field table makes every tag queryable, at the cost of index
  growth bounded by the per-file caps and of correct results only
  after a Refresh has repopulated old rows.
- `HAS` over the field table is an index range scan per field name,
  not a point lookup — acceptable at the library's 100k-path scale
  and always cancellable; a cost hint belongs to the future query
  builder.
- Word search stays byte-for-byte what it was; the toggle means no
  existing search behavior changes silently.
