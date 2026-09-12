# ADR-0152: Technical summary in the Properties workspace

Date: 2026-09-12

Status: accepted

## Context

Properties shows and edits tags but says nothing about the audio
itself — codec, sample rate, bit depth, channels, bitrate, duration —
although every foobar2000 user expects exactly that beside the tag
grid. The library index now retains technical columns (ADR-0150), but
the index is the wrong source for this view: it covers only indexed
library roots, it can be stale relative to a just-converted file, and
Properties routinely opens over ad-hoc rows that were never scanned.
List rows do not retain probe technicals either (the known ADR-0142
follow-up), and rows restored from a saved workspace never re-probe.

## Decision

Properties displays a read-only technical summary line beneath the
file list, fed by its own bounded background prober rather than any
cached copy:

- Selecting files (or none, meaning all) collects their distinct
  physical paths in first-seen order. Paths missing from the dialog's
  per-path cache queue for a single background worker that runs the
  ordinary media probe; results — or a failure marker — land in the
  cache and refresh the label. At most 512 distinct paths are probed
  per dialog; beyond that the label says so. Probing is cancelled when
  the dialog closes and never blocks the UI thread.
- The label aggregates across the analyzed selection: a property whose
  known values all agree shows the value; disagreement shows "mixed";
  properties the probe could not determine are left out. Codec, sample
  rate, bit depth (derived from the sample format), channels, and
  bitrate (best stream, container fallback) aggregate this way.
  Duration sums per selected item — a logical track contributes its
  sample range converted at its stream's rate, a whole file its
  probed duration — and appears once every involved path is analyzed.
  While probes are outstanding the label counts them.
- The bit-depth derivation moves into the formats module
  (`bits_per_sample_hint`) so the library scan and the dialog share
  one rule.

Fresh probes over cached copies is deliberate: the summary is then
correct for unindexed files, for logical tracks, and immediately after
conversions or moves, at the cost of re-probing when the dialog
reopens — bounded, background, and cheap next to the metadata reads
the dialog already performs.

## Consequences

- Reopening Properties re-probes the shown files instead of trusting
  any snapshot; the cache lives only as long as the dialog.
- Row-level technicals retention (Find-bar codec search, technical
  columns in track views) remains the separate ADR-0142 follow-up;
  this ADR neither blocks nor implements it.
- The summary is presentation only — nothing feeds write plans, and
  the label's absence (probe failure) never blocks tag editing.
