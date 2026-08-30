# ADR-0032: Explicit decoder selection and tracker subsongs

- Status: accepted
- Date: 2026-08-29
- Owners: Trackknife project
- Complements: ADR-0028 bounded cue-sheet logical tracks and ADR-0031
  container chapters as logical tracks

## Context

Trackbench's logical-source model already separates opaque identity, physical
raw path, and an optional decoded sample range. That is sufficient for cues and
chapters inside one selected audio stream, but not for independently selectable
content that a demuxer or codec exposes behind an index.

Tracker modules are a concrete case. FFmpeg's `libopenmpt` demuxer accepts a
subsong index but does not publish the file's subsong count as chapters or
streams. Conversely, containers may hold several audio streams for languages,
commentary, or alternate mixes. Treating every audio stream as another song
would silently misrepresent those semantics.

## Decision

- A local logical source may carry an explicit decoder selection containing an
  optional container audio-stream index and an optional codec-native subsong
  index. It is typed playback input and persists separately from both the
  opaque logical identity and the optional sample range.
- Decoder selection is passed through Trackbench's row model, persistence,
  serialized player worker, gapless-next preparation, and FFmpeg decoder. The
  decoder rejects negative or unavailable indexes and rejects a subsong option
  that the selected demuxer did not consume.
- Alternate audio streams are never expanded implicitly. Ordinary files still
  use FFmpeg's best audio stream. A future explicit stream chooser may create a
  selected logical item, preserving the stream index in its identity and typed
  decoder selection.
- Tracker modules recognized by FFmpeg's `libopenmpt` demuxer use libopenmpt's
  stable C API to read the bounded subsong count, names, and durations once.
  At most 10,000 subsongs are admitted. MOD, XM, S3M, and IT files participate
  in ordinary folder intake; explicitly opened files retain the existing
  extension-independent probe path.
- libopenmpt is a required BSD-3-Clause dependency for tracker identity
  projection. It does not create a second playback engine: FFmpeg remains the
  decoder and receives the selected subsong and stream indexes.
- Each tracker subsong becomes one Trackbench row with `codec-subsong-v1`
  identity. A non-empty native subsong name is preferred; otherwise the title
  is `Subsong N`. The module title becomes the album fallback, and one-based
  subsong order becomes the track-number fallback.
- The libopenmpt musical duration is projected to `[0, duration)` in the
  selected subsong's decoded sample domain. This keeps list duration, seek
  limits, persistence, and playback aligned and excludes FFmpeg/libopenmpt's
  generated post-duration fade tail from the clean playback path.
- SQLite migration 5 adds nullable selected-stream and codec-subsong indexes.
  A selection is valid only for a local item, contains at least one index, and
  contains no negative index.

## Alternatives considered

### Expand every audio stream

Rejected. Stream multiplicity commonly represents languages, commentary, or
alternate encodes, not a sequential album. Automatic expansion would create a
plausible-looking but semantically false queue.

### Discover the count by opening indexes until FFmpeg rejects one

Rejected. It repeatedly parses the same file, emits an expected backend error
for every valid probe, and makes worst-case work proportional to repeated full
opens. libopenmpt exposes the count directly.

### Decode tracker modules directly with libopenmpt

Rejected. Trackbench owns one FFmpeg decode boundary for playback, scanning,
conversion, cancellation, and PCM behavior. The direct library adapter is
limited to identity metadata FFmpeg does not expose.

### Encode decoder selection into the opaque logical reference

Rejected. Playback would then need to parse persistence identity strings, and
identity-version changes could silently change decoder behavior.

## Consequences

- Cues, chapters, tracker subsongs, and future explicit alternate streams share
  one source-selection/range path rather than format-specific player branches.
- Tracker files with a single song remain one ordinary file row.
- Other codec-native subsong families need explicit bounded adapters and real
  fixtures before they are claimed.
- A rewritten container may reorder streams. M5 source revision checks and
  operation plans must revalidate selected indexes before committing changes.

## Validation

- A repository-owned, hand-authored ProTracker MOD fixture contains two hidden
  subsongs with different pattern notes. Probe coverage requires two selectors,
  two 600 ms / 28,800-sample ranges, and distinct identities.
- FFmpeg selected-range decode proves both subsongs are non-silent and distinct.
- The bounded playback core queues subsong 1 after subsong 0 through the same
  ring and consumes exactly 57,600 stereo frames with one crossing and no gap.
- Offscreen Trackbench coverage opens the containing folder, observes two rows,
  and restores both decoder selections, ranges, identities, metadata, and order
  from SQLite.

## Revisit when

- Trackbench adds an explicit alternate-stream chooser;
- another codec-native subsong family has a maintained identity API and a real
  multi-song fixture;
- metadata/file rewrites begin changing selected stream or subsong ordering.
