# ADR-0106: Conversion metadata carriage and the parallel scan

- Status: accepted
- Date: 2026-09-03
- Owners: Trackknife project
- Extends: ADR-0105 conversion core, ADR-0097 loudness scan pool pattern

## Context

A converted file without its tags is not a usable output, and M8 promises
whole lists converted quickly. Both belong below the UI so later slices
(destination planning, the converter dialog) only compose.

## Decision

- `convert_audio_file` accepts the item's effective metadata document and
  writes it at mux time: Vorbis-comment containers (FLAC, Ogg, Opus)
  receive the exact native key spellings with true multi-values;
  MP3 maps the common canonical fields onto proper ID3 frames
  (title/artist/album/album_artist/track/disc/date/genre/composer/
  comment) and passes everything else through as TXXX frames. Before the
  atomic rename, every requested field is reread from the finished file
  with the project metadata reader and must match exactly — the same
  honesty the qualified tag writers prove, extended to conversion.
- `scan_conversion` runs items over a bounded pool (1–16 jthread pull
  workers, one decode/encode pipeline each) mirroring the loudness scan:
  per-item failures isolate, source revisions are observed before and
  re-verified after each item (a source that changed mid-conversion
  fails the item and its already-published output is removed),
  cancellation stops cleanly after in-flight items, and destinations
  colliding inside one scan resolve atomically — first publication wins,
  later ones fail as typed conflicts through `RENAME_NOREPLACE`.
  Within-item progress is throttled to roughly one report per second of
  source audio so workers never serialize on the progress lock.

## Consequences

- The converter UI slice needs only: plan destinations (reusing the
  ADR-0069-family output-path planner), build scan items, run one
  `scan_conversion`, and render per-item states/issues — no new
  correctness machinery.
- Tests pin tagged conversion reread across every preset, parallel
  isolation of a missing source and an in-scan destination collision,
  worker-count validation, and pre-cancelled scans converting nothing.
