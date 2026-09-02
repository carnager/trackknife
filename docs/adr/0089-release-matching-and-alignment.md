# ADR-0089: Release matching and alignment

- Status: accepted
- Date: 2026-09-02
- Owners: Trackknife project
- Extends: ADR-0086 typed proposal boundary and ADR-0088 MusicBrainz client

## Context

With search and lookup landed, M6 needs the middle: deciding which release
version fits the selected files, assigning each local file to a release
track, and turning the assignment into staged proposals — all deterministic,
network-free, and conservative enough that partial certainty never writes a
wrong tag.

## Decision

### Ranking presents, never filters

- `rank_release_candidates` orders search candidates by the MusicBrainz
  search score plus a fixed corroboration bonus when a candidate's track
  count equals the selection's. Every version stays visible; ranking only
  sorts the picker.

### Alignment prefers evidence over inference

- `align_release_tracks` flattens the release's media (keeping disc
  position and per-medium track counts) and tries, in order: an exact
  (disc, track-number) permutation covering every local file; plain order
  when the counts match; then a conservative greedy assignment by
  ASCII-normalized title similarity (bounded Levenshtein ratio), duration
  proximity (±3 s strong, ±10 s weak, unknown neutral), and position
  agreement, admitting only pairs scoring ≥ 0.5. Assignments are never
  duplicated; a file that fits nothing stays unmatched at zero confidence.
- Per-track confidence weighs title 0.6, duration 0.25, position 0.15; the
  release confidence is the mean over local files with a penalty when the
  counts disagree.

### The bridge stages, the boundary protects

- `release_metadata_proposals` converts one aligned release into ADR-0086
  proposals: per-track title, artist (credit names with join phrases,
  falling back to the release credit), album, album artist, date, track
  number, per-medium total tracks, disc numbering only for multi-disc
  releases, and the identifier set (recording id as MUSICBRAINZ_TRACKID per
  convention, release-track, release, release-group, artist, and
  album-artist ids as multi-value fields). Tracks below the confidence
  floor receive nothing at all. Every proposal carries the alignment's
  confidence and a rationale naming the exact release version, and the
  whole set stages as one ordinary colored, undoable draft transaction —
  MusicBrainz data reaches files only through the unchanged direct-apply
  safety chain (and lands in both totals spellings via ADR-0087).

## Alternatives considered

### Optimal assignment (Hungarian algorithm) for the fallback

Rejected for now. Stable greedy over a thresholded pair list is
deterministic, transparent to explain per pair, and adequate for
album-shaped selections; optimality can replace it behind the same types
if real mismatch cases demand it.

### Unicode-aware title folding

Deferred. ASCII folding can only lower similarity, never fabricate a
match; a miss costs a suggestion, not correctness.

## Consequences

- The remaining M6 UI work is presentation only: a search box, the ranked
  version list, and staging the chosen candidate's proposals.
- Track-level artist differences (compilations) are honored via per-track
  credits; artwork references and AcoustID remain future extensions of the
  same types.

## Validation

- Fixture tests prove count corroboration in ranking, shuffled disc/track
  permutation alignment, order alignment, greedy fallback that leaves a
  stranger unmatched, per-medium totals and disc numbering in proposals,
  join-phrase artist credits, the full identifier set, confidence floors
  suppressing weak tracks, and typed rejection of mismatched item maps.

## Revisit when

- real-world mismatches justify optimal assignment or Unicode folding;
- Cover Art Archive proposals attach artwork references to the same items.
