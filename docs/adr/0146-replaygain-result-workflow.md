# ADR-0146: ReplayGain result workflow — sidecar policy, retry, disc merging

Date: 2026-09-11

Status: accepted

## Context

Three pieces of roadmap Area 5's result-review surface are workflow
gaps rather than new machinery:

- The write target is always chosen automatically (tags when writable,
  sheet for CUE, sidecar otherwise). Users who never want their audio
  files modified — the "FPL-style" preference that motivated ADR-0141 —
  have no way to say so for writable formats.
- A scan over hundreds of files with three decode failures offers no
  way to re-measure just those three.
- Multi-disc albums tagged "Album (Disc 1)" / "Album CD2" split into
  one loudness programme per disc under release grouping, because the
  release key uses the album text verbatim.

All three ride existing machinery: the sidecar routing and commit
exist (ADR-0141/0143/0145), the scan already knows which items failed,
and grouping is a pure key function.

## Decision

### Sidecar-only loudness target

A persisted checkbox in the Properties ReplayGain section — "Store in
sidecar only" — makes the loudness sidecar the write target for every
non-CUE local source, writable or not. The planner takes a
`MetadataWritePlanOptions{sidecar_loudness}` flag and routes staged
conventional `REPLAYGAIN_*` fields into the per-file sidecar section,
using the captured logical identity or the whole-file entry. CUE
tracks keep their sheet: the REM convention is the interop standard
for them, and the sheet is metadata text, not audio. Ordinary
metadata fields are unaffected; only loudness has an alternative home.

### Retry failures

The scan outcome records the item indexes whose measurement failed or
was cancelled (not the structurally unmeasurable sub-400 ms tracks,
which a retry cannot change). When such items exist, the completion
status carries a "Retry N failed" link that re-runs the scan for
exactly those items with the same grouping settings.

### Disc-merged release grouping

A new grouping mode, "Album merging discs", derives the release key
after stripping a trailing disc designator from the album text — a
final "(Disc 2)", "[CD 1]", "- Disc 3", "Vol. 2", "Part 1", "CD2" and
the like, case-insensitively, with arabic numbering. A MusicBrainz
release id still wins unchanged when present; stripping applies only
to the tag fallback key. The existing modes stay untouched, and the
stripper is deliberately conservative: if removal would empty the
album text, the original is kept.

## Consequences

- "Never modify my audio files" becomes a real, persisted policy with
  everything downstream already proven: journaled sidecar merges,
  provenance projection, playback precedence.
- Failed measurements are one click from re-running instead of a fresh
  selection dance.
- Disc-suffixed albums measure as one programme without hand-written
  tkfmt expressions; the expression mode remains for exotic cases.
- Export and a provenance inspection view remain the open remainder of
  the result-review surface.
