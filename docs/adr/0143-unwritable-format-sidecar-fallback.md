# ADR-0143: Sidecar fallback for unwritable formats

Date: 2026-09-11

Status: accepted

## Context

A whole-file track in a format without a qualified writer (WAV, AIFF,
APE, and every other adapter that reports no proven field writer or
unproven preservation) blocks the entire tag plan with
`writer_unavailable`/`preservation_unproven`. That is correct for
ordinary metadata — Trackknife never writes tags it cannot prove safe —
but it left ReplayGain results with no home at all: the scan measured,
the apply blocked, nothing persisted.

ADR-0141 built exactly the right home. The loudness sidecar already
stores whole-file entries (empty identity), projects onto rows at
sidecar provenance, and feeds the playback override. Only the routing
was missing.

## Decision

When the planner's reader pass finds a source whose adapter cannot
take a safe tag write, and the source is otherwise clean (revisions
agree, no conflicts), the staged conventional `REPLAYGAIN_*` changes
divert out of the tag plan into the source's loudness-sidecar section
as a whole-file entry — merged with any entries the same file's
logical tracks already contributed. Everything else stays exactly as
blocked as before:

- Non-ReplayGain fields keep the visible
  `writer_unavailable`/`preservation_unproven` block; a mixed draft
  therefore still blocks as a whole, with the problems listed.
- Conflicting or exact-native-addressed ReplayGain edits do not
  divert; they block with today's messages.
- A source whose staged changes all divert leaves the tag plan
  entirely; its revision gating continues in the sidecar commit.

The diversion is automatic rather than a dialog because there is
exactly one safe destination for these values and the outcome is
visible, not silent (ADR-0083): the apply summary counts the sidecar,
the rows refresh with sidecar-provenance values that Properties
displays, and the `<file>.tkmeta` beside the source is plain text.
Writable formats are unaffected — their ReplayGain continues into
ordinary tags.

## Consequences

- Scanning WAV/AIFF/APE albums finally persists and plays back
  correctly, without modifying the untouchable files — completing the
  "ReplayGain without touching files" story for every local format.
- The sidecar becomes authoritative-by-necessity for these formats;
  should a qualified writer arrive later, embedded tags would win only
  a fresh scan away, and stale sidecars already yield to file changes.
- `R128_*` fields still block everywhere pending the Opus policy.
