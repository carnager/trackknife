# ADR-0147: ReplayGain result export and loudness provenance view

Date: 2026-09-11

Status: accepted

## Context

The last two pieces of the Area 5 result-review surface: measured scan
results cannot leave the application, and with three loudness carriers
(tags, CUE sheet, sidecar — ADRs 0139/0141/0143/0146) plus unsaved
drafts, there is no place that answers "where does this track's
effective gain come from right now?" per track.

Both are pure presentation over data that already exists: the scan
worker holds every measurement before it becomes drafts, and the
staged selection's cells carry the winning provenance of every field
while the patch set knows the unsaved overlay.

## Decision

### CSV export

The scan worker retains one result row per measured item — track
label, escaped file path, integrated LUFS, track gain/peak, album
key, album gain/peak, and the outcome state (analyzed, unmeasurable,
failed, cancelled). After a scan, the completion status offers an
"Export results" link next to the retry link; it saves a UTF-8 CSV
(quoted RFC-4180 escaping) through a file dialog. The rows are a
snapshot of the *measurement*, deliberately independent of whether the
drafts were edited or applied afterwards. A fresh scan replaces them.

### Loudness provenance view

A "Loudness sources…" button in the Properties ReplayGain section
opens a read-only, non-modal table: one row per selected (or all)
track, one column per conventional `REPLAYGAIN_*` field, each cell
showing the effective value and its origin — `draft` for an unsaved
patch, otherwise the winning provenance of the staged cell (embedded,
sidecar, segment for CUE REM values, …), or `—` when absent. The view
is a projection of the staged selection and patch set; it performs no
I/O and mutates nothing.

## Consequences

- The result-review surface is complete: measure, review provenance,
  retry failures, choose the storage home, export the numbers.
- The provenance view makes the ADR-0139/0141 precedence tangible per
  track instead of being an architecture fact.
- The export is a measurement record, not a library report; exporting
  the *stored* state of arbitrary files remains out of scope.
