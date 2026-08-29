# Metadata, tagging, artwork, and file operations

## Application scope

This document specifies Trackbench capabilities. Per ADR-0025, all tagging,
artwork, MusicBrainz, file rename/move/copy, ReplayGain, and conversion work
belongs to Trackbench, the standalone local-file workstation. The Trackknife
client only reads and displays metadata reported by the server and performs no
local file operations.

## Metadata is a typed, multi-source model

The core must not reduce tags to `Map<String, String>`. A useful logical model
is:

```text
MetadataDocument
  fields: ordered collection of Field
  pictures: ordered collection of Picture
  chapters/cue data: optional structured data
  loudness: optional LoudnessRecord
  unknown/native objects: retained by format adapter where possible

Field
  canonical lookup name
  original/native name or frame identity
  ordered values: [string]
  language/description qualifiers where the format supports them
  provenance: embedded | cue | sidecar | database | stream
```

Field lookup for title formatting is case-insensitive. Writes should preserve
unknown native data and the original representation when it is safe. The UI
may present conventional names (`Artist`, `Album Artist`) while permitting
arbitrary custom fields.

Technical properties—codec, duration, sample rate, channels, bit depth,
encoder, container, bitrate—are read-only decoder/container information, not
ordinary editable tags. Playback statistics and Trackbench-only annotations
also have distinct provenance.

## Format adapters

Each adapter publishes independent capabilities: probe, metadata read, metadata
write, artwork read/write, chapter/cue handling, loudness read/write, padding
optimization, and preservation guarantees. An adapter must fail explicitly when
a requested value cannot be represented.

Important tag families include:

- ID3v1/ID3v2 for MP3 and ID3 chunks in formats such as WAV/AIFF;
- Vorbis comments and FLAC metadata blocks;
- Opus comments plus the Opus output-gain field;
- APEv2 for APE/WavPack/Musepack and optional uses elsewhere;
- MP4 atoms/freeform `----` metadata;
- ASF/WMA attributes;
- Matroska tags, attachments, and chapters;
- RIFF/BWF/INFO and AIFF chunks;
- cue-sheet metadata;
- sidecar metadata where embedding is impossible or deliberately disabled.

Do not assume the same logical field maps identically everywhere. For example,
foobar2000 historically maps common ID3 fields to native frames (`TITLE` to
`TIT2`, `ARTIST` to `TPE1`, `ALBUM ARTIST` to `TPE2`, track/total track to a
combined `TRCK`, disc/total discs to `TPOS`) and uses user-defined frames for
many non-native names. Trackbench needs a documented mapping table per adapter
and interoperability fixtures with other taggers/players.

## Reading and merge precedence

A playable item may receive metadata from the file/container, an external cue,
a sidecar, Trackbench's persisted annotations, a stream, and a playlist
snapshot. Preserve provenance rather than flattening immediately.

Proposed effective-value precedence:

1. explicit user sidecar override;
2. metadata belonging to the logical segment/cue track;
3. current embedded/container metadata;
4. current Trackbench-only annotation/statistic for its namespace;
5. cached playlist snapshot when the source cannot be read.

ReplayGain has its own rules in `replaygain.md`. A user must be able to inspect
the source of an effective value and choose the write target.

## Properties/tag editor

Opening one or many locally resolved tracks produces a non-modal, job-backed
workspace optimized for keyboard and bulk data entry. It must not resemble a
form where every destination field is clicked individually or selected from an
endless fixed dropdown.

The primary surface is a virtualized track-by-field grid with:

- editable field columns/rows and separately visible read-only technical data;
- a state for common value, mixed values, missing everywhere, and partially
  present;
- individual per-track values in selection order;
- arbitrary new fields;
- type-to-add field creation with fuzzy completion over present, conventional,
  MusicBrainz, and recently used names;
- direct keyboard traversal, range selection, fill, clear, copy, and undo;
- rectangular clipboard paste with a complete alignment preview;
- saved field layouts and task presets instead of one global enormous field list;
- correct multi-value editing without semicolon-as-data ambiguity;
- copy/paste of values, complete fields, and an aligned track-by-field matrix;
- field removal and complete tag removal as distinct operations;
- artwork and ReplayGain sections;
- reload/discard and external-change conflict handling;
- a complete preview before writing.

ReplayGain records appear in the same inspection workspace because users
reason about them as metadata. Analysis remains a separate PCM job with its own
algorithm/provenance and cannot be faked as an ordinary text-field edit.

Server items live in the Trackknife client, which displays their metadata but
offers no file-write actions. Trackbench edits only files it opened from disk;
opening a mapped server item in Trackbench is a deferred cross-tool
convenience (ADR-0025), not an eligibility rule inside the client.

Edits are staged in memory. A staged document must record the source revision
(file identity, size, mtime, and preferably tag hash). If a file changes before
commit, stop or request reconciliation rather than overwriting newer metadata.

### Metadata providers

A provider may inspect current metadata, path context, durations, fingerprints,
or other explicitly supplied evidence and return proposed fields, identifiers,
artwork references, provenance, and confidence. It cannot mutate a source. Its
proposal enters the ordinary properties/tag workspace, where alignment,
ambiguity, source revision, and every resulting value remain previewable before
commit.

An online MusicBrainz provider is the committed first implementation (M6) and
must be an explicit network action. Build and validate the Trackbench-owned
provider boundary with real metadata workflows before freezing a public plugin
ABI; other lookup providers should reuse the same proposal contract rather
than receiving direct tag-writer access.

### Auto track numbering

Assign track numbers by visible/selection order, with configurable grouping.
foobar2000 behavior groups different folders as separate albums and writes
`TRACKNUMBER` plus `TOTALTRACKS`. Trackbench should show the grouping and
result before commit and allow grouping by a title-format expression.

### Fill values from filenames or text

The inverse of formatting parses a source string into captures:

```text
source:  Artist/Album [2024]/03. Title.flac
pattern: %artist%/%album% [%date%]/%tracknumber%. %title%
```

foobar2000's “Automatically Fill Values” capture pattern is **not** title
formatting: functions and optional-section brackets do not apply. `%%` ignores
a captured portion, and filename mode can traverse parent directories when the
pattern includes separators.

Implement this as a distinct, versioned capture-pattern parser. Sources include:

- full path or filename;
- a title-format expression evaluated per item;
- existing field(s);
- aligned clipboard lines.

The preview must reveal ambiguous matches, unmatched input, repeated delimiters,
and every captured value. Never silently guess when several parses are possible.

## Saved bulk transformation chains

The foobar2000 Masstagger model—an ordered list where later actions see earlier
results—is worth preserving, with a modern preview and undo journal.

Required actions:

- set a field to a literal value;
- add a value without destroying existing values;
- remove a field;
- remove matching values;
- replace matching values;
- copy one field to another;
- format a field from a pure `tkfmt-1` expression;
- split a field by an exact separator;
- join/merge values explicitly;
- auto-number tracks;
- guess/capture values from a filename/path or other formatted source;
- remove every field except an allowlist;
- embed or remove a cue sheet where supported;
- manipulate artwork through explicit actions.

Chains are named, reorderable, importable/exportable, and versioned. Preview
shows the original and final document plus optionally each intermediate step.
Do not serialize executable host-language code; persist declarative action data
and formatting source.

## Artwork

Artwork is typed (`front`, `back`, `artist`, `disc`, `icon`, `other`) and carries
MIME type, description, dimensions, byte size, and provenance. Support:

- embedded pictures using the format's native representation;
- external search patterns relative to the track/album path;
- configurable source order and fallback names;
- view, add, replace, remove, export, and copy across selected tracks;
- deduplication by content hash;
- warnings for very large images or formats rejected by a container;
- preserving unrelated embedded images during tag writes.

For Vorbis-comment containers, `METADATA_BLOCK_PICTURE` is the preferred
interoperable embedded representation. FLAC uses native picture blocks; ID3 uses
APIC; MP4 uses `covr`. Exact mappings belong to adapter specs/tests.

## Filesystem rename, copy, and move

The operation accepts a selected track set, destination root, and `tkfmt-1`
relative-path pattern. Separators in the evaluated pattern request
subdirectories below the explicit root.

### Plan pipeline

```text
selected TrackRefs
  -> evaluate format expression
  -> validate/sanitize each path component
  -> resolve normalized absolute targets
  -> detect conflicts and relationships
  -> show immutable plan
  -> journal
  -> execute
  -> update local sources/Trackbench lists/statistics
  -> report/retry/undo
```

The preview must detect:

- multiple sources mapping to one target;
- collisions with existing files/directories;
- source equals target and case-only rename cases;
- targets escaping the chosen root (`..`, absolute outputs, symlinks);
- invalid/NUL/separator characters and reserved names for the chosen filesystem;
- maximum component/path lengths;
- read-only sources or unwritable targets;
- cross-filesystem moves (copy + verify + delete rather than atomic rename);
- multiple logical cue tracks referring to the same physical source;
- companion files that would be copied twice;
- Trackbench-list and statistics consequences.

Sanitization is a named policy, not hidden string replacement. Offer Linux,
portable, and custom profiles and show raw versus sanitized output. `$ascii()`
remains a language function but is not a substitute for path safety.

### Execution safety

- Prefer atomic rename when source and destination share a filesystem.
- For cross-filesystem moves, copy to a temporary sibling, flush as appropriate,
  verify size/content policy, atomically publish, then remove the source.
- Never delete a source after a failed or unverified copy.
- Use explicit overwrite/skip/rename/error conflict policy; default to error.
- Journal completed steps durably enough to resume or reverse after a crash.
- Update persisted paths, Trackbench list references, preparation selections,
  statistics, sidecars, and active playback references as one logical
  transaction. Trackbench speaks no MPD protocol; if a destination happens to
  be an MPD music root, requesting a server update afterwards is a job for any
  MPD client, including Trackknife (ADR-0025).
- If the persistence update fails after filesystem success, keep a
  reconciliation record; do not pretend the operation rolled back when it did
  not.

### Companion files and empty folders

Optional “copy/move source folder content” supports covers, booklets, logs, and
cues. It requires a previewed include/exclude policy. Removing empty source
folders occurs only after successful file operations and must never remove a
watched root or a directory containing excluded/hidden user data.

### Cue-aware behavior

Several selected logical tracks may share one audio file. A physical rename or
move runs once per source, and all logical references follow it. External cue
paths must be rewritten safely when chosen. Renaming individual cue tracks into
separate files is a conversion/split operation, not a filesystem rename.

## Metadata write transaction

For each physical source:

1. Acquire a per-source mutation lock.
2. Revalidate source identity/revision.
3. Read a complete native metadata/container representation.
4. Apply the staged logical patch through the format adapter.
5. Write via the safest supported strategy—temporary sibling and atomic replace
   where possible, or the format library's proven in-place update.
6. Preserve permissions and requested timestamps; fsync policy is configurable.
7. Re-read and validate intended fields plus preservation invariants.
8. Update caches and dependent views.
9. Retain journal/backup information according to undo policy.

Batch concurrency must serialize operations that touch the same physical source
even if they originated from different logical tracks.

## Sidecars

Sidecars solve otherwise unwritable formats and user preference not to modify
masters. They must be portable and explicit, not an opaque database accident.

Proposed sidecar requirements:

- one versioned file per physical source or a clearly defined directory index;
- source URI plus robust identity/fingerprint;
- logical subsong identifiers;
- typed multi-value metadata, artwork references, and loudness provenance;
- atomic writes and human-inspectable serialization;
- automatic following during Trackbench file operations;
- conflict behavior when source and sidecar are changed independently;
- opt-in export/import and cleanup.

Trackbench maintains no library database for now (ADR-0025); a future local
index may cache the same data for speed but is not the only copy when the user
expects portable persistence.

## Tests

Every writable format needs real fixture round trips that assert:

- intended fields changed and multi-values survived;
- arbitrary/unknown frames or atoms survived;
- audio essence is unchanged for metadata-only operations;
- artwork and ReplayGain not targeted by the operation survived;
- padding/size changes are acceptable;
- invalid encodings and malformed-but-readable files fail safely;
- cancellation and injected write failures leave a valid original or recoverable
  journal;
- external changes are detected;
- cue/shared-source operations occur once.
