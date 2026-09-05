# ADR-0115: Optional local music library

- Status: accepted
- Date: 2026-09-05
- Owners: Trackknife project

## Decision

The user requested an optional local library with chosen folders, background
indexing, artist/album browsing, album and track search, automatic refresh, and
retained offline entries. This supersedes the earlier “no local index for now”
decision. M5 remains active; this is an explicitly requested workspace addition.

The Library view sits beside Folders in local context. MPD keeps its own
library, search, queue, and transport. Using folders and working lists never
requires adding a library folder.

The existing SQLite store gains a reversible migration for library roots and
a rebuildable metadata index. Raw paths are BLOBs. Embedded metadata remains
canonical. Removing a root only forgets its index; it never removes music.
Overlapping roots are rejected to avoid duplicate ownership and ambiguous
offline state. Symbolic links are not traversed by the indexer.

One scan worker walks the chosen folders and probes only changed files, using
file revisions before and after probing. A separate bounded query worker keeps
browsing responsive while scanning. Queries and tree children are paged; no
whole-library materialization, metadata parsing, or SQL runs on the UI thread.
Cancellation preserves completed work. A partial or failed traversal never
declares unseen files missing. Missing roots and files retain cached metadata
and become unavailable. A successful later scan restores availability.

Refresh runs on startup, on request, after local preparation/conversion, and
every 30 seconds while the application is open. Polling avoids reliance on
filesystem notification support on network mounts. This is revision-based
incremental indexing, not continual audio decoding. Progress reports visited,
indexed, and failed files. Scans have a one-million-entry safety bound.

Local metadata commits update indexed metadata in the same database transaction
as list/cache refresh. Relocations rekey indexed files in that transaction;
moving outside all configured roots removes the index entry while preserving
the ordinary working-list and playback relocation. Background scans revalidate
under the database write lock so stale probes cannot undo a committed change.
Per-root scan tokens prevent an older concurrent scan from publishing missing
file state after a newer scan has taken ownership. Committed moves mark the
destination as seen by its root's current scan.

Album identity uses a MusicBrainz release ID when available; otherwise it uses
album artist, album title, and parent folder, avoiding accidental merging of
different editions. Search is literal, Unicode lowercase, all-word matching;
album matches use album/artist text, track matches also use titles. This is not
a new query dialect. Artist, album, and track selections open ordinary local
working lists using the existing ingestion and shared track-view engine.

The first index catalogs physical audio files. Opening chapter/subsong files
continues to expand their logical tracks through the existing ingestion path;
separate indexed searches of cue/chapter/subsong titles are future work.
Artwork grids, advanced query syntax, and autoplaylists are outside this slice.

## Verification

Real-file tests cover indexing, unchanged-file reuse, search and paging,
unavailable roots and reconnection, raw path bytes, cancellation, root removal,
transactional metadata/relocation, and migration down/up. UI tests cover folder
configuration, browsing/search, opening local lists, and MPD separation.
