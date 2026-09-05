# Local music library

**Trackknife decision (ADR-0115):** local collection browsing is optional.
The Library source in local context indexes folders chosen by the user. The
Folders source remains available without an import or scan. MPD retains its
own library and search.

## Using the library

1. Select a local queue or list, then choose **Library** in the source selector.
2. Open **Folders…**, add one or more music folders, and let the background
   scan run. The footer shows progress; **Stop** cancels the scan.
3. Expand an artist to browse albums, and an album to browse files. Enter or
   the context-menu action opens the selection in an ordinary local queue.
4. Type in the search field to see separate **Albums** and **Tracks** results.
   Album matches use artist/album text; track matches also use titles. Each
   search word must match, ignoring Unicode letter case. Punctuation is literal.

Children and search results arrive in pages of 200; **Show more…** loads the
next page. Opening an artist or album resolves its complete available selection,
not only its currently visible children. Selections above 100,000 physical files
are rejected with an explanation. Unavailable entries remain visible, and
partially available selections report skipped files.

The folder list reports disconnected folders. Removing a folder forgets its
index entries without changing files, queues, or working lists. Overlapping
folders are rejected; add their common parent or separate non-overlapping roots.

## Indexing and consistency

Migration 28 adds library roots and file records to the existing SQLite store.
Paths remain raw BLOBs and are escaped losslessly for presentation. The index
is a cache of file metadata; it does not become the authority for tags.

One worker traverses and probes files; a second serves queries and folder
configuration. A scan compares device/inode/size/mtime revisions, probes changed
files, and verifies the revision again under a short database write transaction.
Only plausible audio extensions are probed; directory and file symlinks are
skipped. Scans stop after one million visited entries and report incompleteness.
Cancellation and incomplete traversal retain previously indexed entries.

A complete scan marks missing files unavailable. An inaccessible root marks
its cached files unavailable without deleting them; later reconnection restores
them. Refresh runs on startup, every 30 seconds while open, on **Refresh**, and
after in-app metadata/file operations and conversion. Only changed files need
metadata probing. Periodic refresh preserves expanded and selected entries.

Metadata and relocation commits update matching index entries in the same
transaction as persisted list/cache changes. A move between indexed roots
changes ownership; a move outside all roots removes the index entry. Failed
transactions and idempotent recovery follow the existing operation journal.

MusicBrainz release IDs identify albums when present. Otherwise album artist,
album title, and parent directory identify an album. Tree labels are evaluated
off the UI thread using shipped `tkfmt-1` expressions. Opened files use the
existing declarative track-view engine, metadata reader, and local transport.

## Current limits

The index catalogs physical audio files. Chapter/subsong expansion still happens
when opening a source; separate indexed searches of those logical titles and
external cue-sheet titles are not included. Advanced filters, autoplaylists,
custom library-tree expressions, and an artwork grid remain future work.

## Verification

`local-library` tests use real FLAC fixtures for background indexing and query
behavior, raw filenames, incremental refresh, offline/reconnection handling,
cancellation, root removal, tag/move transactions, migration reversal, and
opening UI search results in a local queue while preserving MPD separation.

Validated 2026-09-05: all 57 development CTest targets passed. Focused
ASan/UBSan tests for the library, list repository, and queue view passed;
the clang-tidy library/UI build and local-library test passed. The repository
formatting and SPDX checks passed. The offscreen search-and-open workflow was
also inspected visually. This does not claim a measured scan-time budget for
large collections or slow network mounts.
