# ADR-0121: Preserve playback and tag drafts across artwork commits

- Status: Accepted
- Date: 2026-09-06

## Context

Tag publication reset the Local Queue model, invalidating the persistent
playing occurrence introduced by ADR-0119 even when another album changed.
Artwork publication advanced the Artwork section's revision but left the
Fields draft on its original revision, so fetching a cover after MusicBrainz
identification made Apply reject the pending tags.

## Decision

**Trackknife decision:** Committed metadata and path changes update existing
local rows with data-change notifications. They preserve occurrence identity,
selection, and playback order; actual structural replacements still reset.

A verified artwork-only commit in the same Properties workspace advances the
captured tag-source revision for every occurrence of that physical file. The
previous revision must exactly match every occurrence; a missing or inconsistent
revision rejects that source's advance atomically. Only committed sources from
a partially successful artwork operation advance. Failed sources keep their
original revision.

Text baselines, field addresses, sparse patches, provider provenance, and undo/
redo history remain unchanged. Revised source snapshots are sparse and shared;
published selections held by workers remain immutable. Write plans are
invalidated and still reread/revalidate every file before applying. This is
not permission to adopt external file changes or skip publication checks.
Artwork remains a separate journaled operation as specified by ADR-0080.

## Verification

Core tests cover duplicate occurrences, immutable snapshots, successive
revision advances, and rejection of stale/missing/inconsistent revisions.
Queue regressions check persistent indexes and live automatic advancement
after another track's metadata changes. A real-FLAC UI regression stages a
MusicBrainz provider draft, fetches and embeds a cover, exercises undo/redo and
selection refresh, then applies the tags and verifies the cover fingerprints.
