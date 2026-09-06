# ADR-0119: Local playback modes and ReplayGain

- Status: accepted
- Date: 2026-09-06
- Owners: Trackknife project

## Decision

At the user's request, local context exposes Repeat, Random, Single, Consume,
and an Off / Track / Album / Automatic ReplayGain selector in the status bar
and Playback menu. These preferences persist separately from MPD. Changing
local options never sends MPD commands. This extends ADR-0023's sequential
progression policy; M5 remains the active milestone.

**Trackknife decision:** Repeat wraps the originating local list. Random
visits row occurrences once per cycle without rearranging the list; duplicate
files remain distinct occurrences. Previous retraces that cycle's history.
Repeat starts a fresh random cycle without immediately replaying the last row
unless the list has only one row. A lazy Fisher-Yates traversal generates each
choice without an eager whole-list shuffle. Insertion/removal/reorder starts a
fresh cycle anchored at the playing occurrence. Qt persistent indexes track
ordinary insert/remove/contiguous-move edits. Removing the playing occurrence,
closing its tab, or resetting its model ends progression after any already
buffered continuation; ambiguous duplicate paths are never consumed instead.

Single cycles Off / On / One-shot. It stops at track end; combined with Repeat,
it repeats that track. One-shot resets after the next natural completion
(including a single repeat). Manual Previous/Next bypass Single. Consume cycles
Off / On / One-shot and removes finished entries or entries skipped with
Previous/Next from their originating list, marking that list dirty through its
ordinary persistence path. Activating a row directly starts a fresh traversal.
Consume never deletes files. Single plus Consume removes the completed entry
and stops even with Repeat enabled; without Single, Consume advances through
remaining entries until the list is empty. Consume One-shot resets after one
removal. Local progression continues when another authority's tab is visible.

**Trackknife decision:** Automatic ReplayGain selects track gain when Random
is enabled, album gain otherwise. Album falls back to track gain when album
gain is absent; missing or invalid selected gain is unity. Off bypasses PCM
multiplication exactly. Defaults are all playback modes off and ReplayGain off.

The FFmpeg decoder exposes typed gain/peak values from its freshly opened
container and selected audio stream, preferring explicit stream metadata over
container metadata, with demuxer ReplayGain side data as a fallback for absent
fields. Numeric parsing is locale-independent, rejects non-finite/malformed
values, and limits gain to ±60 dB. Invalid explicit fields do not inherit a
more permissively parsed side-data value. This slice reads conventional
ReplayGain values; Opus header gain remains decoder-owned and R128 comment
normalization, sidecar/library-only gain, preamps, and alternate processing
policies remain unimplemented.

The audio worker applies gain to decoded floating-point PCM before the output
ring. A matching positive sample peak limits amplification to full scale;
absent peaks do not imply clipping protection, and this is not a limiter.
Each gapless source uses its own gain. Changes affect newly decoded PCM as
existing buffered audio drains, without seeking or changing the volume slider.
No tags, files, or library scans are changed. Commands are bounded, coalesced,
and retained across a following load. The real-time callback remains free of
parsing, allocation, locks, and I/O.

A continuation whose PCM has already entered the ring cannot be withdrawn.
Changing modes or editing a list near that boundary may therefore hear the
already buffered next track. Clearing/replacing a pending continuation must
preserve that committed source identity until its consumer crossing.

## Verification

Core tests exercise sequential boundaries, random occurrence uniqueness,
Previous/Next history, repeat cycles, empty/single/million-row lists, and
stable candidate selection. Real tagged FLAC files prove gain parsing,
track/album differences, exact bypass, peak clipping prevention, raw filename
bytes, invalid-value rejection, and per-source gain at a gapless boundary.
Audition tests cover command validation and gain policy surviving source loads.
Qt tests cover controls, settings restoration, Automatic mode, authority
isolation, and (when PipeWire is available) Single/Repeat combinations,
One-shot resets, duplicate Consume, Random completion, and background local
progression while the MPD tab is visible. Existing gapless/seek/transport tests
remain regression coverage.
