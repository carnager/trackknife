# ADR-0140: Committed search-result tabs

Date: 2026-09-10

Status: accepted

## Context

Both authorities offer the same transient live search: typing in the
local library panel filters its Albums/Tracks tree after a debounce,
and typing in the MPD library field projects server hits into the
browse surface. Clearing the field discards the view. There is no way
to keep a result set around while searching for something else — a
workflow foobar2000 users expect, and one Trackknife's MPD side even
had before the library-integrated live search replaced it (roadmap
Area 1 tracks the restoration).

The tab machinery on both sides already fits: local tabs are ordinary
track lists with persistence, probing, and playback; ADR-0129
established server-keyed, session-only MPD tabs (stored playlists)
alongside them.

## Decision

Pressing **Enter** in a library search field commits the current query
as a durable result tab. The live search stays the transient default;
Enter is additive and repeatable.

### Local library

Enter resolves the full result set of the committed query — every
track of each matching album (the `search_album` word match), then
every remaining matching track (`search_track`), deduplicated by path
in that order, mirroring the tree's Albums/Tracks presentation — via
the panel's bounded worker, and hands it to the main window as a new
signal (`searchCommitted`). The window creates an ordinary scratch
list tab named `Search: <query>` and routes the paths through the
standard discovery/probe pipeline. The result is a full citizen: it
persists with the workspace, probes, sorts, plays, and exports like
any other list. It is a snapshot; it does not re-run the query.

### MPD

Enter commits the current finished search's track hits as a
server-keyed result tab named `Search: <query>`, following the
ADR-0129 stored-playlist tab pattern: one `MpdQueueModel` snapshot
view per query, keyed and reused on recommit (a second Enter for the
same query refreshes the existing tab instead of duplicating it),
session-only, closable, sharing the MPD queue view layout. Enter or
double-click on rows appends the selection to the live queue — the
same contract as playlist tabs — and the context menu offers
append/insert-next/replace-and-play. The tab holds the track hits the
live search returned (paged by the server search); expanding album
hits into complete releases inside the committed tab remains a
follow-up.

## Consequences

- Search results become workspace objects: keepable, comparable,
  queueable, and (locally) persistent, without changing the live
  search's behavior.
- The local tab is a snapshot by design — a saved *query* (that
  re-evaluates) is the separate Area 2 "saved searches" feature, and
  this ADR deliberately does not preempt its semantics.
- MPD result tabs join playlist tabs in the session-only category:
  they vanish on restart, matching the server-authority model.
- The old bench test assertion that Enter in the MPD search field does
  nothing is obsolete and replaced by commit coverage.
