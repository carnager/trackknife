# ADR-0113: Library A-Z / Latest ordering

- Status: accepted
- Date: 2026-09-03
- Owners: Trackknife project
- Extends: ADR-0102 library navigation, ADR-0112 library actions

## Context

An alphabetical artist tree answers "where is X" but not "what did I add
recently" — the second most common way into a library.

## Decision

- The library heading gains an exclusive A-Z / Latest toggle, shown only
  in MPD context and persisted in QSettings. A-Z is today's alphabetical
  order; Latest ranks root entries by their most recently modified music.
- The ranking derives from MPD itself: a sorted search
  (`modified-since` epoch, sort `-Last-Modified`, windowed to the newest
  2000 tracks) travels the stack as a first-class read command
  (`Client::newest_tag_values` → `database_newest` →
  `MpdProbeController::loadNewestRootOrder`), and the distinct root-tag
  values arrive newest-first. AlbumArtist falls back to Artist per track
  exactly like the tree itself.
- `ServerLibraryTreeModel` applies the ranking as a stable re-sort over
  the alphabetical order: ranked artists lead in rank order, everything
  beyond the window follows alphabetically, and later root loads keep
  the active mode. The ordering refreshes whenever the root reloads
  (connect, database update) while Latest is active.

## Consequences

- The newest-2000-track window bounds the query on large libraries; the
  tail beyond it stays alphabetical rather than pretending to be ranked.
- Tests pin the model semantics (ranked-then-alphabetical, reload
  retention, clearing) and the toggle's visibility and persistence; the
  wire command is covered by the fake-server client test pattern.
