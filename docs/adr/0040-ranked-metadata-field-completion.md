# ADR-0040: Ranked metadata field-name completion

- Status: accepted
- Date: 2026-08-29
- Owners: Trackknife project
- Extends: ADR-0039 dynamic metadata field vocabulary

## Context

`Add field…` accepts arbitrary names, but remembering exact container-style or
MusicBrainz spellings slows keyboard tagging. A fixed dropdown would recreate
the giant vocabulary picker that the Properties workspace is intended to
avoid. Completion must help without turning its catalog into an allowlist or
making backend spelling a write-format promise.

## Decision

- The existing non-blocking `Add field…` command gains a completion popup.
  Typing and accepting an arbitrary name continues to work exactly as before.
- Candidates combine fields actually present in the current selection, the 20
  most recently accepted names in this Properties workspace, a small
  conventional catalog, and the initial MusicBrainz identity/sort-name set.
  The shipped catalog is a discoverability aid, not a list of writable fields.
- Candidate identity and matching use ADR-0033 canonical field names. Exact,
  prefix, substring, and ordered-subsequence matches rank in that order, so
  separator-insensitive input such as `alb art` finds `Album Artist` and
  abbreviated input such as `mb track` finds the recording identifier.
- Canonical duplicates collapse to one suggestion. Present-selection spelling
  wins over recent, conventional, and MusicBrainz catalog spelling; this keeps
  the workspace consistent with metadata the user can already see.
- At most 12 suggestions are shown. Ranking is a deterministic Qt-free pure
  function over the already bounded field vocabulary; the UI only converts the
  result into the completer model.
- Recent names are scoped to the open Properties workspace for now. Persisted
  field history belongs with saved field layouts rather than an unrelated
  settings side channel.
- Completion only selects a name. It stages no value, performs no file I/O,
  and does not decide a future format-specific native write mapping.

## Alternatives considered

### Replace arbitrary input with a fixed dropdown

Rejected. Trackbench must preserve and create arbitrary fields, and a complete
cross-format vocabulary is neither finite nor a write-capability claim.

### Use only prefix or substring matching in Qt

Rejected. Common abbreviated input omits separators and interior text. Keeping
the small ranking function in the metadata module also makes behavior
deterministic and independently testable.

### Persist global recent names immediately

Deferred. Saved field layouts need an explicit persistence and migration story;
the useful interaction can be proven with workspace-local recency first.

## Consequences

- Standard, MusicBrainz, existing arbitrary, and recently created names are
  reachable without browsing a large menu.
- Users may still type a spelling that is not suggested. Canonical duplicate
  handling remains ADR-0039's responsibility.
- Catalog additions change recommendations only; they do not change metadata
  identity, reader behavior, or writer capability.

## Validation

- Qt-free tests cover separator-insensitive exact matching, ordered-subsequence
  matching, MusicBrainz abbreviations, canonical deduplication, source priority,
  deterministic initial ordering, result limits, and no-match behavior.
- The real-FLAC offscreen Properties regression verifies conventional and
  present MusicBrainz completion plus workspace-recent arbitrary names while
  retaining the existing add/remove-field draft workflow.

## Revisit when

- saved field layouts define persisted recent/favorite names;
- locale-aware non-ASCII field-name matching has demonstrated corpus cases;
- format-specific writers define explicit native-name recommendations.
