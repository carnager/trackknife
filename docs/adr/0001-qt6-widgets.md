# ADR-0001: Use Qt 6 Widgets for the desktop UI

- Status: reinstated and refined by ADR-0012
- Date: 2026-08-23
- Owners: Trackknife project

## Context

Trackknife is a native Linux music player and library multitool with dense,
keyboard-oriented desktop workflows: very large track tables, trees and facets,
multi-file metadata editing, custom delegates, drag and drop, menus, shortcuts,
dockable tools, progress views, and detailed preview dialogs.

The application is an open-source personal project. Using Qt's open-source
LGPLv3 option and meeting its obligations is acceptable; a commercial Qt license
is unnecessary.

## Decision

Use Qt 6 Widgets as Trackknife's primary desktop UI toolkit.

- Build the main interface around Qt's model/view framework, custom
  `QAbstractItemModel` implementations, delegates, and proxy models.
- Prefer Qt Widgets over a QML-first application for the dense traditional
  desktop workflows.
- Keep the core domain, decoding, playback, ReplayGain, tagging, conversion,
  database, and filesystem operations independent of Qt UI classes.
- QML/Qt Quick may be embedded later for a genuinely suitable isolated visual,
  but it is not the primary application architecture.
- Consume Qt dynamically under LGPLv3 and include the required notices/license
  and relinking/replacement rights in distributed builds.
- Do not use GPL-only Qt modules unless Trackknife's own chosen license and
  distribution terms permit it explicitly.

This ADR chooses the toolkit, not Trackknife's own project license and not the
implementation language for the non-UI core.

## Alternatives considered

### GTK 4

GTK 4 has strong Linux integration and good bindings, but Qt's mature desktop
model/view, tables, trees, delegates, dock widgets, action system, and toolkit-
neutral desktop style fit Trackknife's power-user interface more directly.

### Qt Quick/QML as the primary UI

Qt Quick is attractive for animated and touch-forward interfaces. Trackknife is
primarily a keyboard/mouse desktop tool with data-heavy editing, where Qt
Widgets provides more mature ready-made behavior. QML remains available for
isolated components.

## Consequences

- C++ becomes the path of least resistance at the UI boundary.
- Large-list performance depends on custom models backed by paged/indexed core
  queries, not `QTableWidget` or one in-memory item object per cell.
- Modern appearance and interaction still require deliberate design; default
  widgets alone do not solve foobar2000's clunky workflow problems.
- Qt Multimedia does not become the audio engine merely because Qt is the UI
  toolkit.
- Packaging must preserve LGPLv3 compliance and dynamically replaceable Qt
  libraries.

## Validation

Before building the full UI, prototype:

1. a virtualized/paged track table with configurable title-formatted columns,
   sorting, multi-selection, and rapid updates;
2. a multi-file metadata editor showing common, mixed, missing, and individual
   multi-value states;
3. a cancellable background job feeding coalesced progress into the UI.

Validate responsiveness with at least one million logical rows and realistic
metadata lengths.

## Revisit when

- Qt Widgets cannot meet measured table/accessibility/performance requirements;
- LGPLv3 distribution obligations become incompatible with a future product
  direction;
- the application becomes primarily touch/mobile rather than desktop;
- maintaining the chosen core-language bridge dominates development cost.
