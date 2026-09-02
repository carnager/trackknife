# ADR-0100: ReplayGain scan action

- Status: accepted
- Date: 2026-09-02
- Owners: Trackknife project
- Extends: ADR-0097–0099 loudness stack, ADR-0086 proposal boundary,
  ADR-0093 WYSIWYG apply

## Context

The measurement stack was complete but nothing user-facing called it —
the side panel still carried the inert M5-era "planned for M7" checkbox.
The trust contract dictates the shape: measured values must land as
visible colored drafts, never as a hidden write.

## Decision

### One scan action, staged like any provider

- The dead checkbox is replaced by "ReplayGain scan" with a grouping
  combo (Album by release — the default, Selection as one album, Track
  gains only, Group by expression with a tkfmt-1 field) in the side
  panel. The scan runs off the UI thread: the draft-materialized
  documents feed ADR-0099 grouping, the ADR-0098 graph measures with
  true peak on half the hardware threads, and the results become an
  ADR-0086 proposal set staged through the ordinary pipeline — colored
  drafts with italic "ReplayGain" provenance, one undo, automatic
  scripts re-staging afterwards, Apply writing through the qualified
  FLAC and WavPack writers.
- Values are RG2-formatted with the C locale's decimal point via
  std::to_chars — never the user locale's comma, which snprintf produced
  on a German system before verification caught it. Track and album
  gains carry "%.2f dB" against the −18 LUFS target with the measured
  LUFS in each field's rationale; peaks are linear "%.6f" sample peaks.
- Progress is the footer line with a live count and a Stop link
  (mutex-free: workers bump one atomic the UI polls); failures,
  sub-400 ms unmeasurable files, and incomplete album programmes go to
  the compact problems-only feedback dialog while every measurable file
  stages normally.

### REPLAYGAIN_* become conventional identities

- The four result fields join the conventional FLAC registry so each is
  one logical column and one canonical identity: a rescan updates the
  existing tag instead of growing a duplicate exact-native column beside
  a file's existing ReplayGain tags.

## Consequences

- The M7 flow is end-to-end for writable formats: scan → colored drafts
  → WYSIWYG apply, with the scan's captured revisions re-verified by the
  ordinary write-plan revalidation at apply time.
- Sidecar fallback for read-only formats and result interop tests
  remain the open M7 work.
