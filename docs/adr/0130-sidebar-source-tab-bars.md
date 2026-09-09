# ADR-0130: Sidebar source tab bars replace the dropdown and heading

- Status: accepted
- Date: 2026-09-09
- Owners: Trackknife project
- Extends: ADR-0115 optional local library, ADR-0129 stored-playlist
  workspace

## Context

The local sidebar switched between Folders and Library through a
`QComboBox`, costing an extra click per switch, and the MPD sidebar had no
switcher at all: the ADR-0129 Playlists list sat squeezed beneath the
library tree with a static heading. The user asked for one-click switching
in both authorities.

## Decision

- The sidebar header hosts one flat `QTabBar` per authority instead of the
  dropdown and the static heading label:
  - Local mode: **Folders | Library** (`bench-local-source-tabs`),
    persisted under the existing `local-library/view` settings key.
  - MPD mode: **Library | Playlists** (`bench-mpd-source-tabs`),
    session-only, defaulting to Library.
- Only the active authority's tab bar is visible; the old
  `bench-folders-heading` label and `bench-local-source-selector` combo box
  are removed.
- The MPD panel stacks two full-height pages: the existing search
  field/browse/search surface, and the stored-playlist list, which loses
  its bounded height and empty-hidden behavior. Entering the Playlists page
  re-requests `listplaylists` when connected.
- `Ctrl+L` and go-to-artist/album switch the MPD sidebar back to the
  Library page before focusing or revealing.

## Consequences

- Switching sources is one click (or one tab drag) in both authorities and
  the playlist list gains full height.
- Widget tests target the tab bars instead of the combo box and heading;
  the sidebar playlist assertions follow the page-based visibility.
