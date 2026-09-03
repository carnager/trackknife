# ADR-0101: Folder bookmarks in the Sources panel

- Status: accepted
- Date: 2026-09-02
- Owners: Trackknife project

## Context

Local mode's Sources panel already carries a lazy multi-root folder tree
(`LocalFolderTreeModel`, roots persisted in `library/roots`), but there
was no fast way back to frequently used places — every session started
with collapsed roots and manual digging.

## Decision

- A compact bookmarks list sits between the panel heading and the tree
  (`bench-folder-bookmarks`), visible only in local mode and only when
  bookmarks exist. Entries show the folder name with the full path as
  tooltip; the list persists as `library/bookmarks` beside the roots.
- "Bookmark folder" joins the tree's directory context menu; a
  bookmark's own context menu removes it. Duplicates are ignored.
- Activating a bookmark reveals the directory in the tree: an
  asynchronous walker descends from the owning root one level at a
  time, fetching lazily and continuing when the rows arrive, then
  expands, selects, and scrolls to the target. A bookmark outside every
  library root becomes a new persisted root and is selected directly.

## Consequences

- A couple of clicks reach any bookmarked corner of the library without
  re-adding roots or manual expansion; the tree stays lazy.
- The offscreen test covers persisted load, name display, asynchronous
  reveal of a nested path, deduplicated adds from the tree menu, and
  removal updating both the list and the stored value.

## Addendum (2026-09-03): a real filesystem tree

Field feedback: the roots-only tree read as "the same manually
bookmarked folders as before" — not the requested browser. The panel
was redesigned: the tree now browses the whole filesystem from `/`
(hidden entries skipped, directories first, and only audio-relevant
files shown — a music workstation's browser, not a general file
manager), with folder and audio-note icons and the home directory
revealed at startup. The old `library/roots` entries migrate into the
bookmarks on first run, headed by the home directory, under a labeled
Bookmarks section; the File-menu action became "Bookmark folder…". The
bookmark reveal walker descends from `/` unchanged.
