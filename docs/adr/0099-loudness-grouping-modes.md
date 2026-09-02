# ADR-0099: Loudness grouping modes

- Status: accepted
- Date: 2026-09-02
- Owners: Trackknife project
- Extends: ADR-0098 parallel loudness scan graph

## Context

Album gain is only meaningful over the right programme. M7 promises four
ways to define it — per track, the whole selection, release-aware, and a
`tkfmt-1` expression — and the scan graph already consumes the answer as
one optional album key per item.

## Decision

- `assign_loudness_groups` is a pure, deterministic function from
  metadata documents to per-item optional album keys, aligned with its
  input and fed directly into `LoudnessScanItem::album_key`.
- **track**: no keys — track gains only.
- **selection-as-album**: one constant key — the selection is one
  programme.
- **release**: MUSICBRAINZ_ALBUMID identifies the programme
  (`mbid:<uuid>`, so a multi-disc set with one release id is one
  programme); files without an id fall back deterministically to
  album + album artist (else artist) under a separator that cannot
  appear in tags; files with no identifying tags stay track-only rather
  than being lumped into a programme they do not belong to.
- **format expression**: a `tkfmt-1` expression compiled once in the
  grouping context and evaluated per document over effective canonical
  values — the same resolution lens the transformation dialect uses.
  Equal non-empty results group; an empty result stays track-only; a
  non-compiling or empty expression fails typed before any scan starts.
- Key namespaces (`mbid:`, `tag:`, `fmt:`, `selection`) never collide
  across modes.

## Consequences

- The result-surface slice composes: read documents, assign keys, scan,
  present — grouping is testable without touching audio.
- Tests pin all four modes, including the MusicBrainz id beating the
  tag fallback, unidentifiable files staying track-only, shared-id
  files sharing one programme, and typed failures for broken
  expressions.
