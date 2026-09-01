# ADR-0086: Typed metadata proposal boundary

- Status: accepted
- Date: 2026-09-01
- Owners: Trackknife project
- Extends: ADR-0035 bounded drafts, ADR-0048–0070 transformation staging, and
  ADR-0083/0084 direct apply with grid color semantics

## Context

M5 work item 7 requires a metadata-provider boundary — proposed values,
identifiers, artwork references, provenance, and confidence — proven by
internal use before any online provider (M6 MusicBrainz) or public plugin
ABI. With the review dialogs gone (ADR-0083/0084), the tag grid's colored
draft is the single inspection surface, and ADR-0084 explicitly reserved it
for exactly this case: values the operator did not author must be visible
and reviewable before anything is written.

## Decision

### One typed, Qt-free contract

- `MetadataProposalSet` carries the provider's name and detail (provenance)
  and per-item proposals: ordered replacement field values with confidence
  in [0, 1] and a human-readable rationale, plus artwork references the
  provider may point at but never fetches or writes. MusicBrainz and other
  identifiers travel as ordinary fields — that is how they live in files.
- `MetadataProposalProvider` is a function over the draft-effective
  selection (`StagedMetadataSelection` + `StagedMetadataPatchSet` + item
  indexes + cancellation). Providers only observe and return; they never
  mutate, so network providers slot in without touching write safety.
- `metadata_proposal_preview` validates shape and limits, drops proposals
  below a confidence floor and proposals equal to the draft-effective state,
  and converts the rest into the existing stageable transformation preview.
  Accepted proposals therefore become one ordinary undoable draft
  transaction: colored in the grid, revalidated at Apply, and identical to
  hand-typed edits from that point on.

### Internal proof: selection consistency

- `propose_selection_consistency` derives album groups from the
  draft-effective selection and proposes only what the selection itself
  proves: a missing ALBUMARTIST when the group's visible album artists (or,
  failing that, every track's complete artist value list) agree exactly, and
  track totals when the group's track numbers are exactly the contiguous run
  1..N. Both carry confidence 1.0 and a rationale naming the album;
  disagreement or gaps propose nothing.
- "Already satisfied" and "unchanged" are judged against the *writable*
  state — the staged draft (in the logical or exact-native registry), else
  embedded-provenance baseline fields only. Cached-snapshot and
  stream-projected values can make a tag look present in the effective
  document while no writable tag exists; such phantoms never suppress a
  proposal. TRACKTOTAL and TOTALTRACKS are distinct logical spellings of
  the same meaning read by different consumers: like Picard, totals are
  proposed in both spellings, each skipped individually once its writable
  value matches. DISCTOTAL/TOTALDISCS is the analogous pair when disc
  totals are ever proposed; spacing/underscore variants already unify
  through canonicalization.
- Properties exposes it as **Suggest** beside the grid tools: it runs in the
  background over the selected files (all files when none are selected),
  stages the surviving cells as one transaction, and reports the count and
  provider name in the status line. Files that already agree report "No
  suggestions".

## Alternatives considered

### A dedicated proposal-review dialog

Rejected for this boundary. ADR-0083's contract holds: the colored draft is
the review surface, undo is one step, and nothing reaches a file without the
ordinary Apply revalidation. M6's side-by-side candidate comparison can
build a richer surface on the same types when multiple candidates exist.

### Providers that stage or write directly

Rejected. Observation-only providers keep the plan/journal safety chain the
single write path and make network providers (M6) structurally unable to
bypass staged preview.

## Consequences

- The M5 provider boundary exists and is exercised end to end by a real
  in-tree provider with tests, before any network code.
- M6 implements `MetadataProposalProvider` for MusicBrainz: values and
  identifiers stage through the same preview; artwork references route to
  the artwork operations; confidence and rationale already have a home.
- Confidence below the staging floor is currently dropped silently; ranking
  and side-by-side presentation of competing candidates are M6 UI work.

## Validation

- Qt-free tests prove album-artist agreement and contiguous-run totals
  proposals, the fill-only-missing behavior, draft-effective derivation,
  disagreement and gap silence, confidence gating, unchanged-value
  counting, and malformed-set rejection.
- An offscreen test proves Suggest stages six colored cells across three
  files as one transaction, one undo removes them all, restaging works, and
  an agreeing selection reports no suggestions.

## Revisit when

- M6 needs multiple competing candidates per field (add candidate lists and
  a comparison surface on these types);
- a public plugin ABI freezes this contract for external providers.
