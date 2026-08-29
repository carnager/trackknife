# ADR-0011: Use a full Qt Quick application shell

- Status: superseded by ADR-0012
- Date: 2026-08-25
- Owners: Trackknife project
- Supersedes: ADR-0001 and the `QMainWindow`/`QDockWidget` portions of ADR-0003

## Context

The M0 Qt Widgets spike proved paging, bounded background updates, selection,
and workspace persistence, but it also exposed the amount of custom painting
and styling required to reach Trackknife's primary product requirement: a
modern MPD client that feels exceptionally pleasant rather than merely
functional. Very little production UI exists, while the C++ core, MPD adapter,
models, and formatting engine are independent of the presentation toolkit.

Trackknife needs dense virtualized queue and tag tables, excellent mouse and
keyboard behavior, queue/list tabs, saved split layouts, artwork, responsive
transitions, and a distinctive but restrained visual system. Arbitrary dock
composition is less important than a polished default workspace.

Qt Quick's `TableView` reuses visible delegates and consumes C++
`QAbstractItemModel` instances. `SplitView` provides persistent resizable panes,
and Qt Quick Controls provide menus, actions, tabs, dialogs, keyboard focus, and
accessibility primitives. A `QQuickWidget` hybrid would add an offscreen render
pass and disable Qt Quick's threaded render loop.

## Decision

- The product executable uses `QGuiApplication`, `QQmlApplicationEngine`, Qt
  Quick, and Qt Quick Controls as one full scene.
- Do not embed the product scene in `QQuickWidget` and do not embed ordinary
  Widgets inside the scene.
- C++ owns MPD sessions, source/list models, paging, selection commands,
  formatting, persistence, jobs, and all expensive work. QML owns composition,
  styling, lightweight delegate presentation, focus, and animation.
- Use lean recycled delegates with no per-row network, disk, metadata, or
  formatting work and no persistent state stored inside delegates.
- Ship a designed `SplitView` workspace with server navigation, queue/list tabs,
  optional details/artwork, and persistent transport. Save pane sizes and ship
  recoverable layout presets.
- Defer arbitrary free-form docking. If later user evidence requires it, add a
  declarative panel/layout system rather than reintroducing a mixed toolkit.
- Keep the legacy Widgets spike and format sandbox as developer-only targets
  during migration. They are not the product shell and may be removed after
  equivalent QML diagnostics and benchmarks exist.
- Retain the Qt 6.4 minimum during the first spike. Use custom delegates/column
  handles where newer convenience controls are unavailable; raise the baseline
  only through a packaging ADR with evidence.

## Alternatives considered

### Heavily style and custom-paint Qt Widgets

This preserves mature desktop tables and docking, but requires substantial
bespoke rendering to escape a conventional utility appearance. It optimizes for
arbitrary composition over the newly clarified product priorities.

### Mix Widgets and QML through `QQuickWidget`

This keeps some mature widgets but introduces two styling, focus, accessibility,
and rendering systems. Qt documents an extra render pass and loss of the
threaded render loop, which cuts directly against the performance reason to use
Qt Quick.

### Use QML for a decorative shell and perform row logic in JavaScript

This would move hot paths into a difficult-to-measure dynamic layer and weaken
the reusable core. QML remains a view language over typed C++ services/models.

## Consequences

- ADR-0001 is superseded; Qt remains the toolkit, but Widgets are no longer the
  product UI foundation.
- The useful parts of ADR-0003 remain: polished defaults, queue/list tabs,
  declarative presentations, saved layouts, and strict budgets. Dock-widget
  implementation details do not.
- Dense table editing, clipboard ranges, drag/reorder, accessibility, focus,
  and screen-reader semantics require explicit QML acceptance tests.
- The million-row C++ model and formatter caches remain reusable.
- Custom visual identity becomes much easier without sacrificing the C++ core.
- Packaging must include the selected Qt Quick/QML modules and styles.

## Validation

Before replacing the legacy spike as performance evidence, the QML shell must:

- display and scroll the one-million-row paged C++ model with bounded delegate
  and page residency;
- meet the tab-switch, frame, selection, workspace-restore, and input budgets;
- support row/range selection, keyboard navigation, column resize/reorder,
  queue drag/reorder, and tab operations with mouse and keyboard;
- restore `SplitView` state and shipped defaults safely;
- pass an accessibility/focus audit for menus, tabs, transport, tables, and
  dialogs;
- render correctly with representative Wayland scaling and software-renderer
  fallback diagnostics.

## Revisit when

- the dense tag grid cannot meet editing, accessibility, or performance gates;
- supported Linux graphics stacks make the scene graph materially unreliable;
- user testing demonstrates that arbitrary dock composition is more important
  than the designed workspace.
