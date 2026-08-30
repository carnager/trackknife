# ADR-0031: Container chapters as logical tracks

- Status: accepted
- Date: 2026-08-29
- Owners: Trackknife project
- Complements: ADR-0028 bounded cue-sheet logical tracks

## Context

ADR-0028 established the persisted distinction between a logical item, its
physical source, and an exact decoded sample range. Trackbench also needs to
recognize chapters embedded in containers without making the Qt layer inspect
FFmpeg structures or turning backend timestamps into its persistence contract.

Not every chapter table is an audio-track partition. Video chapters may omit an
intro or credits, overlap, or carry only navigation points. Expanding only the
valid-looking entries would hide part of the physical audio source. Trackbench
therefore needs an explicit conservative policy for when chapters replace the
ordinary whole-file row.

## Decision

- The Qt-free Formats probe projects FFmpeg `AVChapter` entries for the selected
  best audio stream. It exposes only Trackknife-owned IDs, ordered metadata, and
  decoded sample boundaries; no FFmpeg type crosses the adapter.
- One physical source may expose at most 10,000 chapters. Exceeding that bound
  fails the probe with a typed limit error.
- Chapter timestamps are translated from their declared rational time base
  relative to the selected audio stream's declared start. Each absolute
  boundary is scaled once into the stream's sample rate with integer floor
  rounding. Adjacent entries must reuse the same mapped boundary.
- Expansion is all-or-nothing. The table must have a known source duration,
  start at sample zero, contain only non-empty end-exclusive ranges, remain
  exactly adjacent without gaps or overlaps, and end at the known source
  duration. A malformed or partial table yields no logical chapters; Trackbench
  retains the playable whole-file row rather than hiding audio or clamping a
  boundary.
- Trackbench's existing bounded background probe stage creates one logical row
  per projected chapter. It replaces the provisional whole-file row in place
  and inserts the remaining rows directly after it, preserving list order. If
  expansion would exceed the 100,000-row intake bound, the whole-file row stays
  and the status bar reports the limit.
- Logical identity uses the versioned tuple `container-chapter-v1`, raw physical
  source bytes, selected audio-stream index, backend chapter ID, and chapter
  order. Persistence continues to store that opaque identity independently
  from physical raw path and sample range.
- Chapter metadata overrides matching container/best-stream metadata. An absent
  chapter title becomes `Chapter N`; track number similarly falls back to its
  one-based order. Album falls back from an explicit album tag to the
  container title, and album artist falls back to the container artist. This
  keeps audiobook/disc chapter rows grouped without treating those fallbacks as
  rewritten file tags.
- Segment-aware playback, gapless-next chaining, seeking, status reporting, and
  list persistence use the same typed sample-range path already established for
  external cue tracks.

## Alternatives considered

### Expand every chapter independently

Rejected. Skipping one malformed entry or accepting a gapped chapter table can
make valid audio unreachable from the resulting list while still looking like
a successful import.

### Keep the whole-file row beside chapter rows

Rejected as the default. It duplicates the same audio in ordinary exhaustive
chaptered albums and makes sequential playback repeat the source. Files whose
chapter table is not an exhaustive partition remain one whole-file row.

### Persist raw FFmpeg timestamps

Rejected. The player operates in decoded sample ranges, and persisted backend
time bases would couple list behavior to FFmpeg interpretation across versions.

### Treat multiple audio streams as chapters

Deferred. Stream selection has distinct language, codec, edition, and identity
semantics; this ADR covers only chapter partitions of the selected audio
stream.

## Consequences

- Matroska, MP4, and other demuxers that expose exhaustive chapters enter the
  same logical-track model as external cues without adding a second playback or
  persistence path.
- Containers with navigation-only or malformed chapters remain fully playable
  as ordinary files.
- Chapter projection currently follows FFmpeg's demuxer interpretation. Unlike
  the repository-owned external CUE parser, it does not claim a separate
  container-format compatibility implementation.
- ADR-0032 subsequently adds tracker subsongs while retaining alternate audio
  streams as explicit, user-selected sources rather than automatic expansion.

## Validation

- A repository-owned 9,600-sample Matroska/FLAC fixture contains two adjacent
  native chapters plus scoped metadata. Probe coverage requires exact ranges
  `[0, 4800)` and `[4800, 9600)` and ordered chapter tags.
- Opening both ranges must decode to the byte-exact concatenation of the
  complete source. This fixture exposed coarse millisecond timestamp drift in
  segment trimming; decoded PCM is now counted contiguously after the first
  timestamp anchor.
- An offscreen Trackbench regression observes the provisional file become two
  metadata-rich logical rows and restores both identities and sample ranges
  from SQLite after restart.

## Revisit when

- an explicit alternate-stream chooser or another codec-native subsong family
  enters the source model (tracker subsongs are resolved by ADR-0032);
- real containers require an explicit partial-chapter presentation mode;
- physical move/rename operations begin rewriting opaque logical identities.
