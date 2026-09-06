# ADR-0128: M3U8 local playlist interchange

- Status: accepted
- Date: 2026-09-06
- Owners: Trackknife project

## Decision

Continue the next roadmap item with explicit **File → Import M3U8 playlist**
and **Export list as M3U8** commands. M5 remains the active acceptance gate.
Import is available from either authority and creates a new named local list;
it never changes an existing list, the MPD queue, or playback. Export captures
all occurrences in the chosen local list, in order. MPD stored playlists remain
the next separate roadmap task; server identifiers are not translated into local
paths by this portable-file workflow.

**Trackknife decision:** this first interchange profile accepts UTF-8 M3U8,
with an optional UTF-8 BOM, LF/CRLF, ordinary comments, `#EXTM3U`, and
`#EXTINF:seconds,title`. The header is optional. Fractional durations become
milliseconds; unknown durations remain absent. EXTINF titles are fallback display
labels, not artist/title parsing instructions or tag edits. Readable embedded
metadata supplies the normal list columns. Unknown duration remains unknown when
EXTINF is absent; import does not invoke a decoder just to obtain a duration.

Relative file references are anchored to the selected playlist's directory,
without canonicalization or collapsing `..` across symlinks. Import preserves
order, duplicates, unavailable paths, and one whole-file occurrence per reference.
It never recursively imports nested playlists, expands CUE/chapter/subsong
references, or executes player options. Generic metadata reads run in the worker;
missing/unreadable sources keep their label (or filename) and cached duration.
Those fallback labels persist with the list, including offline occurrences.

Accept plain Linux paths and local `file:` URIs with empty or `localhost`
authority, decoding percent escapes as raw bytes. Reject NULs, invalid UTF-8
playlist text, malformed escapes, remote URIs/authorities, HLS directives, and
unsupported `#EXT` directives. Leading spaces/tabs before lines are ignored;
ambiguous filenames should use `./` or an escaped file URI. No legacy-encoding
guessing, Windows drive translation, or network request is performed. A malformed
or unsupported entry rejects the complete import with a line diagnostic.

Export emits EXTM3U and optional EXTINF titles/durations. Paths below the output
directory use relative spelling; other paths stay absolute. Ambiguous names,
line breaks, and invalid UTF-8 filename bytes use percent-escaped absolute local
file URIs. This preserves raw Linux filenames without lossy display conversion.
The file picker and action explain that the file contains references, titles,
and durations only: other cached metadata, artwork, list/view state, and queries
are not serialized. Each logical reference, sample range, or explicit
stream/subsong selector rejects the complete export with its one-based row;
there is no silent conversion to a whole file or partial export. Invalid display
labels that would inject playlist lines also fail before output creation.

The profile is independently implemented. VLC's
[M3U importer](https://raw.githubusercontent.com/videolan/vlc/master/modules/demux/playlist/m3u.c)
was consulted for UTF-8/BOM and common directive conventions, not copied. This is
not a claim of support for every player's extensions.

## Publication and cancellation

The save picker explicitly requests a **new file**. The worker serializes and
validates the entire snapshot before creating a sibling temporary file, writes
and fsyncs it, then publishes with an atomic no-replace hard link using a held
parent-directory descriptor. Existing files, directories, and dangling symlinks
are never overwritten. A filesystem that cannot hard-link fails safely. The
caller can choose a different filename after a conflict. Replacement/backup
workflows are intentionally deferred; no destructive operation or journal is
introduced by creating a new file.

Cancellation before publication removes the temporary file without creating a
destination. Publication is the commit point: cancellation arriving afterward
reports the completed export. Parent-directory fsync is best effort after
publication; a failure there does not imply the visible file was rolled back.
Temporary files are removed on ordinary failure/cancellation; a process crash may
leave a hidden `.trackbench-playlist-*` temporary file. Original media and existing
playlists are unaffected. Automatic crash cleanup is not claimed.

## Architecture and limits

The Qt-free `lists` library owns bounded parsing, serialization, and file adapters.
One private worker handles a transfer at a time, with an inline progress/Cancel
bar. Imports prepare all rows and metadata off the UI thread, then publish the
complete new list through the normal model/persistence path. Closing cancels and
joins the worker before saving the workspace; late imports cannot appear after
closure.

Export snapshots copy at most 128 rows or roughly 256 KiB per event-loop slice
with a 4 ms budget. Model edits/destruction during capture reject the snapshot;
artwork/current-row notifications do not. After capture, export owns an immutable
snapshot of the explicitly selected local list and can finish across tab switches
or later list edits. No background worker accesses a model or widget.

Limits are 32 MiB of playlist text, 64 KiB per line, 100,000 occurrences, 64 MiB
of resolved-reference/capture payload, and 128 MiB of estimated hydrated import
metadata. Oversized inputs fail without partial publication. Transfer loops
check cancellation; synchronous backend metadata reads and filesystem calls are
bracketed by cancellation, not forcibly interrupted. Existing whole-list model,
artwork, and persistence publication paths remain in use; million-row UI budget
acceptance is not established by these tests. No schema or external dependency
changes are needed.

## Verification

The M3U8 suite covers BOM/CRLF/EXTINF, relative resolution, duplicate and missing
references, Unicode/raw-byte/line-break filename round trips, fractional duration,
symlink-parent semantics using real files, invalid encodings/URIs/directives,
logical selection rejection, cancellation, exclusive publication including
existing and dangling-symlink destinations, FIFO rejection, temporary cleanup,
real FLAC metadata hydration, asynchronous round trips, stale capture rejection,
and shutdown suppression of late imports.

Workspace coverage imports while MPD is active, verifies unchanged existing local
and MPD lists, checks export authority gating, and closes/reopens the workspace to
verify duplicate unavailable paths, display labels, and durations survive.

Validation: the warnings-as-errors development build, all 60 development CTest
targets, repository C++ formatting, and `git diff --check` passed.
