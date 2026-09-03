# ADR-0107: Trackbench converter dialog and converted publication planning

- Status: accepted
- Date: 2026-09-03
- Owners: Trackknife project
- Extends: ADR-0105/0106 conversion core and scan, ADR-0069-family output
  path planning, ADR-0083 trust over confirmation

## Context

The converter needs its UI, and destination planning already exists: the
output-path planner evaluates tkfmt-1 layouts, sanitizes, contains below a
root, and previews conflicts. But it plans relocations of the source files
themselves — it deduplicates logical items per physical source, which is
exactly wrong for converting a cue album image into per-track files.

## Decision

- `plan_output_paths` gains an optional `ConvertedPublicationPolicy`:
  planned names carry the preset's target extension (and `%extension%`
  resolves to it), logical items sharing one physical source fan out to
  one target per item instead of merging, and the physical-alias guard is
  skipped — converted outputs are new files, the sources are never
  touched. Without the policy the relocation semantics are unchanged.
- "Convert files…" (Edit menu, enabled with the local selection exactly
  like Properties) opens the converter dialog: preset combo with probed
  availability (missing encoders are visible but disabled with the probe
  detail), destination root, tkfmt-1 folder and name expressions, and a
  parallelism spin. A debounced live preview replans on every edit and
  shows the planned relative targets; problems replace the summary line
  only when they exist (ADR-0083). Convert runs `scan_conversion` on a
  QtConcurrent worker with the polled progress-bar pattern, Stop cancels
  cleanly after in-flight items, closing while running cancels first, and
  the destructor synchronizes worker shutdown. Settings (preset, root,
  expressions, parallelism) persist across sessions.
- The dialog creates target directories up front so parallel workers
  never race directory creation; per-item failures land in the summary
  as problems-only lines after the run.

## Consequences

- Converting a selection — whole files or cue subtracks of one image —
  is one dialog with a live truthful preview and no review step; every
  correctness guarantee lives in the layers below (planner issues,
  atomic conversion, exact tag verification).
- Later M8 slices refine rather than restructure: mirror mode, artwork
  carriage, bit-depth policy, ReplayGain handling on signal change.
