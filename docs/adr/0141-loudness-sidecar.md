# ADR-0141: Loudness sidecar files

Date: 2026-09-10

Status: accepted

## Context

After ADR-0139, CUE logical tracks persist ReplayGain in their sheet,
but the remaining ADR-0124 targets still measure into nothing durable:
container chapters and codec subsongs have no per-track embedded
mapping, and formats without a qualified writer cannot store tags at
all. The write planner blocks these with
`unresolved_non_embedded_target`. The open Area 5 decision — "sidecar
or library record" — needs a format.

The FPL discussion sharpened the goal: ReplayGain that works without
modifying audio files. foobar2000 achieves this with playlist-cached
values; our model wants a durable, portable, file-adjacent record with
explicit provenance instead of an opaque cache.

Two facts make the integration cheap: `FieldProvenance::sidecar`
already outranks every other provenance in the document model's
effective-value precedence, and ADR-0139 built the explicit ReplayGain
override path through local playback.

## Decision

### Location and format

One sidecar per physical audio file, directly beside it:
`<filename>.tkmeta` (full name plus suffix, e.g. `album.flac.tkmeta`).
The content is strict, human-inspectable JSON with a version gate:

```json
{
  "tkmeta": 1,
  "source": {"size": 123456, "modified_seconds": 1757500000,
             "modified_nanoseconds": 0},
  "loudness": {
    "reference_lufs": -18.0,
    "entries": [
      {"start_sample": 0, "end_sample": 8820000,
       "track_gain_db": -3.46, "track_peak": 0.994629,
       "album_gain_db": -5.53, "album_peak": 0.994629}
    ]
  }
}
```

An entry's identity is the logical source inside the file: optional
`stream_index`/`subsong_index` (decoder selection) plus optional
`start_sample`/`end_sample` (segment). A whole-file entry carries none
of the four. All values are numbers; the codec is a bounded, hand-
rolled, fail-closed JSON subset in the metadata module (Qt-free): 64
KiB size cap, entry cap, unknown keys and unsupported versions are
errors, doubles serialize with C-locale `to_chars`.

### Staleness

`source` records the audio file's size and mtime (seconds +
nanoseconds) at write time — deliberately *without* device/inode, so a
sidecar remains valid across `cp -p`, backup restores, and moves that
preserve timestamps. A sidecar whose `source` disagrees with the audio
file is stale: readers ignore its values, and the next write discards
the stale entries wholesale before merging fresh ones.

### Write path

`StagedMetadataSource` gains an optional logical identity (selection +
segment), captured by Bench alongside the ADR-0139 cue binding. The
write planner routes staged `REPLAYGAIN_*` intents on logical tracks
*without* a cue binding into a per-audio-file sidecar plan section —
per-entry fields, no cross-entry agreement needed because album values
are stored per entry. `R128_*` stays blocked pending the Opus policy,
and whole-file tracks keep using tags; a sidecar fallback for
unwritable formats is a follow-up, not silent behavior.

Commit (`operations::commit_loudness_sidecar`) is revision-gated on
the *audio* file (capture evidence, like every metadata write) and
atomic: read the existing sidecar (an unparseable one is a conflict,
never clobbered), drop stale entries, merge the planned ones
(`remove_field` clears values; an entry with no values left is
dropped), then publish via temp + fsync + rename + parent fsync — or
delete the sidecar when nothing remains. Undo-journal parity shares
the ADR-0139 follow-up.

### Read path and precedence

The probe worker reads `<file>.tkmeta` once per physical source; when
fresh, each row (whole-file, chapter, subsong) gets its matching
entry's values projected as `REPLAYGAIN_*` fields at
`FieldProvenance::sidecar` — which already wins the effective-value
precedence, so Properties and any consumer see sidecar values above
embedded tags with visible provenance. Local playback derives its
explicit override in that order: sidecar values first, then CUE REM
values, then the decoder's embedded tags. No I/O happens at play time;
the projection travels with the row and persists with the list cache.

## Consequences

- Chapters, subsongs, and segments finally keep their measured
  loudness, and it plays back correctly — without touching the audio
  file. The sidecar travels with the file and stays legible in any
  editor.
- The schema is versioned and extensible (future payloads add sibling
  sections under new keys with a version bump), but this ADR only
  defines loudness.
- Stale-by-mtime is a heuristic: a tag rewrite of the audio file
  invalidates its sidecar until the next scan. That is the safe
  direction — silence, never wrong gains.
- CUE tracks keep their sheet carriage (ADR-0139); sidecars do not
  apply to them, avoiding two writable homes for one value.
- The library-record alternative was rejected as the primary store:
  the library is a cache by contract (ADR-0116); the sidecar is the
  durable record and the library/list caches inherit it through the
  ordinary row pipeline.
