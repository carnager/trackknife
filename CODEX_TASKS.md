# Trackbench: validation, UI decomposition, and follow-ups

Read `AGENTS.md`, `MILESTONES.md` (M5 is active), and `docs/README.md` before
changing anything. Follow every rule in AGENTS.md, notably: SPDX headers on new
files, GPL-3.0-only, no UI-thread blocking, bounded workers with cancellation,
and consequential decisions recorded as ADRs under `docs/adr/`.

## Recent context

Recent work on `main` you must not regress:

- The Track properties tagging dialog and the tagging script editor
  (`src/bench/metadata_properties_dialog.cpp`) were redesigned: checklist side
  panel, manager dialogs for naming layouts/destinations, live auto-preview in
  the script editor (400 ms debounced), grouped step-kind combo where the
  action kind lives in item data (not the row index).
- The `trackknife` compatibility shell was retired (ADR-0071): `src/app` is
  gone, `src/ui` is now a small shared library, and the `ui-smoke` test
  launches `trackbench --screenshot`.
- Widget object names (`bench-*`) are load-bearing: the offscreen UI tests in
  `tests/bench_main_window_test.cpp` find widgets by them. Never rename one
  without updating the tests in the same commit.

## Task 1 — sanitizer and static-analysis validation (do this first)

`README.md` and `docs/README.md` state that clang-tidy and full sanitizer
validation remain to be rerun for the current continuation point. Run:

1. `cmake --preset asan && cmake --build --preset asan && ctest --preset asan`
2. `cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan`
3. `cmake --preset tidy && cmake --build --preset tidy && ctest --preset tidy`

Fix any findings. Rules:

- Fix root causes; do not suppress warnings or add sanitizer ignore entries
  unless the finding is a proven false positive, and justify any suppression
  in the commit message. `tests/lsan.supp` exists for known leaks — extend it
  only for third-party leaks outside project code.
- If a data race or UB finding requires a design change, stop and describe it
  instead of guessing.
- When everything passes, update the "remains to be rerun" sentences in
  `README.md` and `docs/README.md` to state what was validated.

## Task 2 — split the two bench UI monoliths (mechanical, no behavior change)

`src/bench/metadata_properties_dialog.cpp` (~5,000 lines) contains roughly a
dozen classes. Move each into its own translation unit under `src/bench/`,
keeping the existing namespaces, object names, and behavior byte-for-byte:

- `MetadataExactValueModel` + `MetadataExactValueDialog`
- `MetadataTransformationPreviewModel`
- `MetadataRuleScriptImportDialog`
- `MetadataTransformationDialog`
- `MetadataScalarDelegate`
- `MetadataWritePlanModel`, `OutputPathReviewModel`, `PreparationPlanDialog`
- `MetadataApplySourceModel`, `MetadataApplyDialog`
- `FilePublicationApplySourceModel`, `FilePublicationApplyDialog`
- `MetadataPropertiesDialog` stays in `metadata_properties_dialog.cpp`

Then decompose `src/bench/bench_main_window.cpp` (~6,400 lines) by concern
into separate files (e.g. transport/playback, MPD controls + search, panel
layout engine, list tabs + track views, metadata operations/history). Prefer
extracting cohesive helper classes over free-floating partial classes; keep
`BenchMainWindow` as the composition root. Do NOT redesign anything in this
task — pure code motion plus the minimal headers/CMake updates.

Constraints:

- Update `src/bench/CMakeLists.txt`; keep target names and the
  `Trackknife::BenchUi` alias unchanged.
- After each extraction step, the full dev suite must pass:
  `cmake --build --preset dev && ctest --preset dev` (43/43).
- Commit in reviewable steps (one commit per extracted cluster), not one
  giant commit.

## Task 3 — small UX follow-ups (only after tasks 1–2 are green)

1. Persist the Track properties dialog and script editor geometry and
   splitter positions across sessions, using the same persistence service the
   main window already uses for panel layouts (see `ListPersistenceService`
   usage in `bench_main_window.cpp`). Restore on open; never block the UI
   thread on the load.
2. Empty-state placeholder painted inside the scripts list
   (`bench-metadata-transformation-list`) when it has no rows ("No saved
   scripts yet"), replacing the separate status-label text for that case.
3. In the naming-layout manager dialog (`bench-output-layout-manager`), add a
   live example line under the form: render the folders + filename
   expressions through the existing tkfmt engine against the first selected
   track's metadata, updating debounced as the user types. Reuse the
   project's formatting services; do not invent a new evaluator. Show
   validation errors inline in that line.
4. Copy pass on the remaining dialogs: "Preparation review" window and the
   apply dialogs still contain internal vocabulary ("staged changes",
   "physical source", "Logical scope"). Reword user-visible strings to plain
   language without changing object names; update test string assertions in
   the same commit.

Each item: add or extend tests in `tests/bench_main_window_test.cpp`
(offscreen, object-name driven, QTRY-based waits — follow existing patterns),
and keep the suite at 100%.

## Out of scope

- Do not start ReplayGain (capability-gated to M7), MusicBrainz (M6), or the
  committed search/stored-playlist tab migration.
- Do not touch persistence schema or migrations unless task 3.1 genuinely
  requires a new table — if so, follow the existing numbered migration pattern
  (`src/persistence/migrations/`, up+down, schema version bump) and say so.
- No new dependencies.

## Definition of done

Dev, asan, tsan, and tidy presets all build and pass ctest; docs updated where
they claim validation state; commits are small and each leaves the tree green.
