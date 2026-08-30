# ADR-0028: Bounded cue-sheet logical tracks

- Status: accepted
- Date: 2026-08-29
- Owners: Trackknife project
- Complements: ADR-0025 standalone local tool

## Context

Trackbench must present cue tracks as logical playable items while retaining one
physical audio source. That relationship affects identity, metadata provenance,
seek boundaries, gapless transitions, file operations, and persistence. Letting
the UI or playback service parse cue text independently would create competing
boundary rules and would make raw filesystem paths vulnerable to lossy text
conversion.

This is repository-owned behavior. It does not claim compatibility with every
cue-sheet producer or make FFmpeg's cue interpretation part of Trackknife's
persistence contract.

## Decision

- The Qt-free Formats library owns a bounded external cue parser and a separate
  logical-track planner. Source bytes, lines, files, tracks, indexes, and
  metadata fields all have explicit limits.
- `FILE` references retain the exact bytes between syntax delimiters. The parser
  does not assume UTF-8 and does not resolve, normalize, or open a path. A later
  source-containment step resolves it relative to the cue sheet.
- Directive names, file types, track modes, flags, and remark names use
  ASCII-only case normalization. User metadata and file-reference bytes are not
  normalized.
- The typed model retains global and per-track `TITLE`, `PERFORMER`,
  `SONGWRITER`, and `REM` data; `CATALOG`, `CDTEXTFILE`, `ISRC`, `FLAGS`,
  `PREGAP`, `POSTGAP`, and all indexes. Unknown directives are retained with
  their line and argument so unsupported input is visible rather than silently
  reinterpreted.
- Only `AUDIO` tracks become playable logical items in this slice. Every audio
  track requires `INDEX 01`. Its start is that boundary and its end is the next
  track's `INDEX 01` within the same `FILE`, including a data track's boundary;
  the final track remains open-ended until the physical duration is known.
  `INDEX 00`, explicit gaps, and other indexes remain available as source data
  but do not move the playable start.
- CUE times use 75 frames per second. Each boundary maps from the common file
  origin with integer floor division, `floor(cue_frame * sample_rate / 75)`.
  Adjacent ranges therefore share the exact same computed sample even at rates
  not divisible by 75; durations are never rounded independently.
- A known physical duration closes the final range. Empty, reversed, or
  out-of-source ranges are rejected rather than clamped.
- The resolver canonicalizes the external cue itself, resolves each playable
  `FILE` against its directory, and reuses the core containment check to require
  the final regular-file target to remain strictly inside that resolved
  directory. It opens each referenced playable source once to obtain its actual
  decode rate and duration before producing typed sample ranges.
- Trackbench folder/file/drop intake runs cue resolution off the UI thread. A
  successfully expanded sheet creates one row per audio track and suppresses a
  redundant whole-file row for any physical source discovered in the same
  intake batch. Explicitly opening the audio without its cue remains ordinary
  whole-file intake.
- Logical rows persist a versioned opaque identity derived from canonical cue
  path plus cue file/track indexes, independently from the canonical physical
  source path and optional sample range. SQLite migration 4 preserves all three
  fields without encoding raw paths as text.
- The serialized local player accepts segment-aware load and gapless-next
  commands. Its public snapshot reports positions and durations relative to the
  active logical segment while the decode core retains absolute physical sample
  boundaries.

## Alternatives considered

### Ask FFmpeg to expose cue tracks

Rejected as the durable contract. Backend behavior and metadata projection may
change between versions, and external cue path handling still needs
Trackknife's byte-preserving containment rules.

### Convert cue paths and metadata to UTF-8 during parsing

Rejected. Linux paths are raw bytes internally, and accepting a cue sheet must
not make a referenced source unreachable or change its identity.

### Round each track duration to samples

Rejected. Independent duration rounding can introduce a one-sample gap or
overlap between adjacent logical tracks. Converting absolute boundaries once
keeps the ranges contiguous.

### Treat `INDEX 00` as the next track's start

Rejected for the initial policy. `INDEX 01` is the stable playable boundary;
retaining the other indexes allows a later explicit pregap presentation policy
without silently changing existing logical identities.

## Consequences

- Cue logical items can share a physical reference without duplicating decode
  or future filesystem work.
- A directory containing `album.cue` and `disc.flac` presents the cue tracks
  once by default, while the ordinary audio file remains available when opened
  independently.
- Metadata inheritance is deterministic: track performer/songwriter overrides
  sheet values, while album title and album performer remain sheet-level.
- Unsupported directives survive parsing but have no behavior until a later
  decision defines it.
- ADR-0031 adapts exhaustive backend-exposed container chapter tables into the
  same logical source/range model. ADR-0032 subsequently adapts tracker
  subsongs and keeps alternate-stream selection explicit; neither is implicitly
  covered by this external-text parser.

## Validation

- Tests cover BOM and CRLF input, quoted and unquoted fields, global/track
  metadata, multiple files, a data-track boundary, unknown directives, and a
  file reference containing an invalid UTF-8 byte.
- Malformed ordering, invalid timestamps, duplicate indexes, missing audio
  boundaries, and source limits fail with typed errors.
- A 44,117 Hz regression proves adjacent fractional-frame boundaries meet at
  one sample, while physical-duration validation rejects out-of-source ranges.
- A real two-second WAV plus external cue proves canonical containment, exact
  one-second ranges, final-duration closure, and rejection of a `../` escape.
- An offscreen Trackbench regression scans the containing folder, observes two
  metadata-rich segment rows instead of an extra whole-file row, and restores
  their distinct logical identities and ranges from SQLite after restart.
- Playback coverage concatenates two segments of one physical source exactly;
  the serialized service also reports segment duration and position relative to
  the logical start.

## Revisit when

- another embedded-cue or codec-native subsong family enters the matrix
  (container chapters are resolved by ADR-0031 and tracker subsongs by
  ADR-0032);
- a user-facing pregap inclusion policy is designed;
- real-world fixtures require a currently retained but unsupported directive.
