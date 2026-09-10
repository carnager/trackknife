# ADR-0139: CUE sheet ReplayGain carriage

Date: 2026-09-10

Status: accepted

## Context

ADR-0124 let the loudness scanner measure logical sources (CUE tracks,
container chapters, codec subsongs) but deliberately blocked the results
from becoming whole-file tags: a per-track gain written into the physical
file's tags would misdescribe every other track sharing that file. The
write planner marks such intents `unresolved_non_embedded_target`, so a
scan over a CUE-backed album measures correctly and then stores nothing.
Durable storage for logical-track loudness has been the open decision
blocking roadmap Area 5 ("sidecar or library record").

Research into prior art settled the CUE case. There is an established
de-facto standard, introduced by foobar2000 and read by CUETools,
DeaDBeeF, and others: ReplayGain values are carried in the CUE sheet
itself as `REM` comments —

- album values in the sheet header:
  `REM REPLAYGAIN_ALBUM_GAIN -5.53 dB` / `REM REPLAYGAIN_ALBUM_PEAK 0.994629`
- track values inside each `TRACK` block, before its `INDEX` lines:
  `REM REPLAYGAIN_TRACK_GAIN -3.46 dB` / `REM REPLAYGAIN_TRACK_PEAK 0.994629`

This is strictly better than a proprietary sidecar for CUE tracks: the
values travel with the sheet, other players understand them, and the
sheet is already the authoritative description of the logical split.
Trackknife's CUE parser already surfaces every `REM` field, and ingest
already projects sheet and track remarks into list-row metadata at
segment provenance — the read path largely exists.

A generic sidecar would still be needed for container chapters, codec
subsongs, and formats without qualified writers. That decision stays
open; nothing in this ADR constrains it.

## Decision

### Carriage format

Trackknife writes ReplayGain results for CUE logical tracks into the
`.cue` sheet using the foobar2000 `REM` convention exactly as above.
Values are C-locale formatted: gains as fixed two-decimal `dB` (same
text the tag path stages), peaks as fixed six-decimal numbers. Only the
four `REPLAYGAIN_TRACK_GAIN`, `REPLAYGAIN_TRACK_PEAK`,
`REPLAYGAIN_ALBUM_GAIN`, `REPLAYGAIN_ALBUM_PEAK` names participate;
`R128_*` fields have no CUE convention and remain blocked.

Album values are a property of the sheet header. When staged album
values disagree across tracks of one sheet, the sheet plan blocks with
`conflicting_logical_edits`; agreement is required, absence is fine.

### Rewriter and preservation proof

A Qt-free rewriter in `formats` (`rewrite_cue_replay_gain`) produces the
new sheet bytes from the original bytes plus a per-sheet update plan:

- An existing `REM REPLAYGAIN_*` line being updated is replaced in
  place; later duplicates of the same name in the same scope are
  removed. A value being cleared removes the line.
- A new album line is inserted before the first `FILE` directive; a new
  track line is inserted before the track's first `INDEX` line,
  reusing that reference line's leading whitespace and line terminator
  (header insertions use the terminator style of the first line).
- Every other byte of the sheet is preserved: BOM, encoding, line
  terminators, indentation, unknown directives, comments.

The rewriter proves preservation before returning: the output must
re-parse successfully, and the parsed sheet must equal the original
parse in every respect except the intended `REPLAYGAIN_*` remarks,
whose parsed values must equal the plan. Any mismatch fails the
rewrite; nothing is published.

### Plan and commit integration

`StagedMetadataSource` gains an optional CUE binding (sheet path, sheet
revision observed at capture, file and track index) that Bench
populates from the `cue-v1` logical reference. The write planner routes
staged `REPLAYGAIN_*` intents on a CUE-bound logical source into a new
per-sheet section of the plan (`MetadataWritePlan::cue_sheets`, one
entry per distinct sheet with per-track values and merged occurrence
lists) instead of blocking them. Non-ReplayGain fields and
ReplayGain fields on non-CUE logical sources keep today's
`unresolved_non_embedded_target` block.

Commit (`operations::commit_cue_replay_gain_sheet`) is revision-gated
against the capture evidence and atomic: prepared temp sibling, fsync,
in-place rename, parent fsync, with a final revision re-check
immediately before the replace. It deliberately ships without the
ADR-0059 undo journal in this first slice: the journal's persisted
`content_kind` schema admits only text and artwork records, so cue
records need a schema migration plus recovery and undo branches — a
tracked follow-up. The gap is bounded because the rewrite is proven
byte-preserving outside the intended REM lines and the publish step
cannot tear the sheet.

### Precedence at playback

For a CUE logical track, the sheet's `REM REPLAYGAIN_*` values are the
authoritative gains; the physical file's embedded whole-file tags
describe the full image, not the track, and are used only when the
sheet carries no values. Bench derives the effective
`formats::ReplayGainInfo` from the row's segment-provenance fields and
passes it as an explicit override through `LocalAuditionService` into
`LocalPlayback`, which applies it both at open and at gapless takeover
(a queued continuation must not fall back to the physical file's tags).
Preamps and peak limiting (ADR-0138) apply unchanged on top.

## Consequences

- CUE albums gain lossless, interoperable ReplayGain storage; scans
  over CUE-backed albums finally persist, and foobar2000 or DeaDBeeF
  see the same values.
- The sheet becomes a mutation target that is revision-gated, atomic,
  and byte-preserving outside the intended lines; undo-journal parity
  with audio-file mutations (schema migration, recovery, undo) is a
  tracked follow-up.
- Chapters, subsongs, and unwritable formats still lack durable
  loudness storage; the generic sidecar/library decision remains open
  and unconstrained.
- Multi-album sheets are not modeled: album values apply to the whole
  sheet, matching the convention's data model.
