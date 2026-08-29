# ADR-0003: Ship a complete workspace with configurable dock panels and views

- Status: implementation reinstated and refined by ADR-0012
- Date: 2026-08-23
- Owners: Trackknife project

## Context

Trackknife needs foobar2000-class control over how library, playlist, and queue
items are displayed, but unrestricted UI construction is not the primary goal.
The application must work beautifully out of the box, remain approachable, and
still expose deep customization and future plugin panels. Performance and
fluidity are primary product requirements.

Qt 6 Widgets supplies movable, floating, nested, and tabified dock widgets with
layout persistence.

## Decision

- Ship a polished default workspace with library on the left and a tabbed queue/
  playlist track workspace on the right/main area.
- Use a shared track-view engine for library results, playlists, autoplaylists,
  and queue, while retaining their different semantics and actions.
- Ship grouped albums with covers as the friendly default plus plain columns,
  compact queue, folder/tree, and artwork-grid presets.
- Make display values, grouping, summaries, and sorting customizable through the
  shared versioned formatting language.
- Define view/panel configuration declaratively rather than requiring arbitrary
  executable UI scripts.
- Use `QDockWidget` panels that users can move, float, split, close, and tab;
  persist named layouts and provide a reliable reset to shipped defaults.
- Allow plugins to register new panel types and related providers through a
  future versioned API.
- Treat the responsiveness and performance rules in `docs/ui-workspace.md` as
  acceptance requirements.

## Alternatives considered

### Fixed, non-configurable two-pane UI

Simpler, but unnecessarily discards Qt's strong docking support and limits power
users without improving the default experience.

### Empty layout builder like a UI construction kit

Powerful but produces a poor first run, shifts product design work to users, and
makes support/performance harder. Customization is layered over good defaults
instead.

### Arbitrary imperative UI scripting as the primary extension model

Difficult to sandbox, migrate, optimize, and support. Declarative view
definitions plus registered plugin panels cover the intended use more safely.

## Consequences

- Library and queue presentations share rendering/caching infrastructure and
  `tkfmt-1` expressions rather than diverging implementations.
- Panel instances and view definitions require stable IDs and schema versions.
- Custom models/delegates and asynchronous caches are foundational work, not UI
  polish to add later.
- Plugins must meet the same performance and isolation contract as built-ins.
- The shipped layout and presets become tested product artifacts.

## Validation

- Usability test the untouched default layout for import, browse, play, queue,
  search, tag, and ReplayGain tasks.
- Benchmark view presets and layout operations against the fixture sizes and
  provisional budgets in `docs/ui-workspace.md`.
- Round-trip dock/tab layouts, missing plugin panels, preset copies, and reset.
- Verify the same title-format column/group definition in library, playlist, and
  queue contexts with only documented context-field differences.

## Revisit when

- Qt docking cannot deliver a stable or accessible workspace on supported Linux
  desktops;
- measurements show that a particular panel/presentation needs an isolated
  rendering technology;
- real plugin use cases require a different extension boundary.
