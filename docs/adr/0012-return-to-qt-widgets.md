# ADR-0012: Return the product shell to Qt 6 Widgets

- Status: accepted
- Date: 2026-08-25
- Owners: Trackknife project
- Supersedes: ADR-0011
- Reinstates and refines: ADR-0001 and the implementation portions of ADR-0003

## Context

The Qt Quick shell made visual iteration easy, but hands-on use of the first
live MPD workspace repeatedly exposed friction in the exact interactions that
define Trackknife: dense mouse/keyboard tables, native selection, default-button
dialogs, compact transport controls, anchored menus, desktop spacing, and
system-theme integration. Fixing these required custom QML behavior around
facilities Qt Widgets already provides.

The desired interaction reference is Cantata: a compact, conventional native
desktop client with common actions visible and few unnecessary clicks. It is
not a request to copy Cantata's artwork or source. Trackknife's C++ MPD session,
controllers, and `QAbstractItemModel` implementations remain independent of the
presentation choice, so changing now does not disturb protocol or core work.

## Decision

- Use a pure Qt 6 Widgets product executable based on `QApplication` and
  `QMainWindow`.
- Use `QTableView`, `QTreeView`, `QTabWidget`, `QDockWidget`, `QToolBar`,
  `QAction`, `QMenu`, and ordinary dialogs for the default workspace.
- Prefer the active desktop/system Qt style. Custom delegates and restrained
  painting are allowed where they improve music-specific information, but the
  application does not replace native behavior merely to create a skin.
- Keep the shipped default dense, coherent, and complete. Docking and layout
  customization layer over that default and are not required setup.
- Keep MPD, formatting, persistence, audio, metadata, and long work outside UI
  classes. Widgets consume typed controllers and `QAbstractItemModel` objects.
- Do not mix the product shell with `QQuickWidget`. A future isolated Qt Quick
  visual requires its own measured need and architecture decision.
- Remove the QML scene and Qt Quick runtime dependencies after the Widgets shell
  reaches the current feature surface.

## Consequences

- Native menus own their anchoring, dialogs regain standard default-button and
  focus behavior, and tables use the mature desktop model/view selection and
  accessibility paths.
- System styles provide KDE/GNOME integration without a second custom control
  theme. A modern result depends on disciplined layout, icons, density, and
  information design rather than wholesale restyling.
- The existing queue, browser, output, and paged models remain reusable.
- Workspace geometry, toolbars, docks, and panels persist through versioned
  `QMainWindow` state.
- Qt QML, Quick, and Quick Controls are no longer application build or package
  requirements.

## Validation

- Preserve the live MPD queue, server search/folder browsing, stored-playlist
  reads, outputs, ReplayGain, transport, playback modes, connection dialog,
  queue mutations, and queue/list tabs in the Widgets shell.
- Keep common playback and queue actions one click or one shortcut away.
- Continue the million-row paging, selection, tab, dock, restore, sanitizer,
  static-analysis, keyboard, and native-menu tests.
- Verify behavior under representative KDE and Wayland scaling by hand.

## Revisit when

- A specific isolated visual cannot meet measured requirements with Widgets and
  a custom delegate;
- Trackknife becomes primarily touch/mobile rather than keyboard/mouse desktop
  software;
- native model/view or accessibility paths fail a reproducible acceptance gate.
