# ADR-0117: Local library actions and drag-and-drop

- Status: accepted
- Date: 2026-09-05
- Owners: Trackknife project

## Decision

The user requested dragging local library entries into tab contents and
context actions similar to the MPD library. Both libraries reuse the same
branch interaction, row presentation, and three inline action controls.
Presentation callbacks provide local labels and availability without giving
the local view MPD identities or commands.

Local artists, albums, and tracks support Append to current list, Insert next
in current list, Replace list and play, and Open in new tab. Insert next uses
the playing row when playback belongs to the target list; otherwise it uses
the selected row, or the beginning of an unselected list. The first three
actions also appear inline. Click or Enter toggles branches; Enter on a track
appends it. Right-click preserves an existing multi-selection and offers
Expand/Collapse on branches. Unavailable selections have disabled intake
actions, while partially available selections retain their readable files.

Drags copy a captured selection into the indicated row of a local queue/list.
An in-process typed MIME payload holds an asynchronous resolver, not a path
list or a database query executed during drag creation/hover. It cannot be
interpreted as MPD rows or as a request to move physical files. The target
captures its document identity and an insertion-row anchor before resolving
the selection. Switching tabs never redirects intake; closing the target or
removing/resetting its anchor discards the pending insertion.

Resolution includes all available indexed tracks, independent of loaded
pages. Selections are visited in tree order, selected ancestors subsume their
descendants, and overlapping paths are inserted once. Each request accepts at
most 1,000 selected entries and resolves at most 100,000 file references before
deduplication, bounding both UI capture and worker work. Raw path bytes stay
intact. Existing local intake handles discovery, logical-track expansion,
metadata, artwork, persistence, and playback. Replacement occurs only after
intake produces nonempty, uncancelled results.

ADR-0116 remains in force: library filesystem scans require Refresh. Browsing,
actions, and dragging resolve cached index records without starting a scan.

## Verification

Real-file tests cover append/next/replace/new-tab actions, overlapping selection,
album drops at an insertion row, changing search/active tab during resolution,
closing the destination, MPD rejection, and 205 files behind an unexpanded
artist including a non-UTF-8 filename. Shared queue tests verify copy semantics,
no resolution during hover, and rejection of unregistered or counterfeit local
payloads. Existing MPD library and workspace tests exercise the shared view.
