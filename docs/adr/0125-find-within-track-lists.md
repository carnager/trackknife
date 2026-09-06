# ADR-0125: Find within the active track list or MPD queue

- Status: accepted
- Date: 2026-09-06
- Owners: Trackknife project

## Decision

**Trackknife decision:** provide an initially hidden native find toolbar for the
active local working list or MPD queue. Edit → Find in current list and Ctrl+F
open and focus it. Next/Previous buttons, Enter/Shift+Enter in the field, and
F3/Shift+F3 navigate matching occurrences, wrapping once at list boundaries.
Escape or Close dismisses find and returns focus to the list. New text searches
from the current occurrence inclusively after a 150 ms debounce; explicit
navigation starts after/before it. A match selects and reveals one existing row
without filtering, rearranging, marking dirty, or starting playback. No match
and an empty query leave selection intact.

Search is a literal substring within any cached title, artist, album, album
artist, date, track number, or source reference: a losslessly escaped local path
or the exact MPD URI. Repeated MPD tag values are searched individually. It uses
the core's locale-independent simple lowercase mapping, with no full case
folding, normalization, token grammar, or formatting/query dialect.
CUE/chapter/subsong titles and duplicate occurrences retain their existing row
identity. Hidden columns remain searchable. Arbitrary metadata, technical
values, and duration are outside this initial search surface; the input tooltip
lists its scope.

Find text is shared between track tabs for this session only. Switching tabs
cancels and hides find; reopening reuses the text without moving selection.
Properties tabs disable the track-list find commands. Both MPD and local tabs
enable them: read-only find does not require mutation authority, send server
commands, or resolve an MPD URI to a local file. Library search remains the
separate Ctrl+L action and never starts a filesystem scan implicitly.

## Responsiveness and consistency

The toolbar owns one worker pool with a single worker and at most one in-flight
batch. The UI captures detached text from at most 128 rows and 256 KiB per
batch, yielding after a 4 ms capture budget. Matching and raw-path escaping run
on the worker; it never accesses an item model. Progress reports visited rows.
Queries are limited to 1,024 UTF-16 code units. A row exceeding 64 KiB of total
searchable source text stops the traversal with an explicit limit message; MPD
tracks with more than 1,024 tag entries also report a limit rather than
performing an unbounded UI-thread projection. A read-only per-row accessor
captures detached MPD text without copying the entire queue. Any server queue
refresh that changes rows or text invalidates pending results, preserving stable
song-ID ownership without treating captured positions as permanent identities.
Invalid cached UTF-8 likewise reports an error rather than claiming no match. No
input is silently truncated.

Query replacement, close, tab changes, structural edits, metadata refresh, and
selection changes cancel active work. Generation checks reject queued stale
results, including a completed worker whose Qt completion signal has not yet
been delivered. Edits show “List changed — search again”; finding does not
automatically overwrite the user's new selection. Artwork and playing-marker
notifications do not invalidate text searches. No persistence migration or new
dependency is needed.

## Verification

Dedicated offscreen tests cover duplicate/logical occurrences, next/previous
wraparound, Unicode case handling, cached fields, raw non-UTF-8 paths, no-match
and empty-list behavior, 10,000-row traversal with event-loop delivery,
replacement queries, cancellation, tab changes, remove/undo/reorder, metadata
refresh, ignored playing-marker notifications, and explicit oversized-row
feedback. MPD regressions cover duplicate URIs with distinct song IDs, repeated
tags, exact URI text, and remote removal/metadata refresh during a search.
Workspace tests exercise shortcuts in both authorities and disable find only
when a non-track Properties tab is active. Find operations preserve model
contents, history, and playback markers.

Validation on 2026-09-06: the development warnings-as-errors build and all 58
CTest tests passed. After adding selection-extension cancellation, the dedicated
find suite and both-authority workspace regression passed again. Formatting,
SPDX headers, and whitespace checks passed.
