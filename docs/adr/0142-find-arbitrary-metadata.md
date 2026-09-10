# ADR-0142: Find across arbitrary metadata and technical values

Date: 2026-09-10

Status: accepted

## Context

ADR-0125's Find deliberately searched only the cached display
projection — title, artist, album, album artist, date, track number —
plus the escaped path/URI. Anything else on a row was invisible to it:
comments, composers, ReplayGain values, ENCODER tags, sidecar
projections, MPD `audio_format`, durations. The roadmap tracked lifting
that exclusion.

Everything needed is already on the rows: a local row carries its full
multi-value `MetadataDocument` (every provenance, including the
ADR-0141 sidecar projections and probed stream tags) and its duration;
an MPD row carries the server's complete tag list, URI, duration, and
`audio_format`. No new I/O or model plumbing is required — only a wider
haystack in the existing bounded pipeline.

## Decision

Find matches the substring against every value a row carries:

- **Local rows**: all values of every metadata field, at every
  provenance, in document order; the six cached display projections
  (covering filename-fallback titles of unprobed rows); the duration
  formatted exactly as the Length column shows it; and the escaped raw
  path as before. Field *names* are deliberately not matched — Find
  stays a value search, and name-based queries belong to the Area 2
  filter language.
- **MPD rows**: every server tag value (the six-name filter is
  removed), unknown structural pair values, the URI, `audio_format`,
  and the formatted duration.

The ADR-0125 contract is unchanged: bounded batches captured on the UI
thread, matching on one worker, 64 KiB per row and now a uniform
1024-value cap per row in both authorities — exceeding either reports
a visible limit error, never a silent "No matches". Invalidation
already covers metadata-only updates because row refreshes emit
role-less `dataChanged`.

## Consequences

- Searching "opeth" in a COMMENT, an MP3 ENCODER string, a ReplayGain
  value, "44100:16:2", or "3:45" now works in both authorities, with
  the same navigation, wraparound, and cancellation behavior.
- Multi-value fields match on any value; provenance does not gate
  matching (a stale-invisible sidecar value is never projected, so it
  cannot match).
- Local codec/bit-rate/sample-rate searching still needs those values
  retained on rows (probe-technicals retention), which remains a
  separate follow-up; today only what probing already projects as tags
  is searchable.
