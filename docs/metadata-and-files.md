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
  provenance: cached snapshot | annotation | embedded | stream | segment | sidecar
```

Field lookup for title formatting is case-insensitive. Writes should preserve
unknown native data and the original representation when it is safe. The UI
may present conventional names (`Artist`, `Album Artist`) while permitting
arbitrary custom fields.

Technical properties—codec, duration, sample rate, channels, bit depth,
encoder, container, bitrate—are read-only decoder/container information, not
ordinary editable tags. Playback statistics and Trackbench-only annotations
also have distinct provenance.

**Trackbench decision (ADRs 0033, 0043–0047, M5 baseline):** the Qt-free document
and conservative TagLib property reader are implemented. Canonical lookup lowercases
ASCII letters and ignores spaces, underscores, and hyphens while retaining the
adapter's native exposed key. Reads preserve ordered repeated values, project
the initial MusicBrainz identity set, inventory unsupported native objects, and
carry a raw-path filesystem revision. Native FLAC now has an independent
Vorbis-comment text writer that creates only a distinct prepared copy, rejects
unrepresentable mappings, rereads all text, and byte-verifies preserved FLAC
blocks and compressed audio on every operation. Its headless executor now adds
per-source serialization, locked revalidation, a durable journal, preserved
filesystem metadata, retained exact backup, atomic publication, reread,
dependent-state commit, rollback, and conservative startup recovery. Startup
now presents recovery and ambiguous evidence, while retained backups support
verified crash-recoverable undo, explicit release, and bounded maintenance.
ADR-0047 exposes that complete path through an explicit two-worker cancellable
Apply job with per-source results and fresh-preview retry. Other formats and
picture mutation imply no write capability.

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

### Native FLAC text mapping (`taglib-flac-v1`)

ADR-0043 qualifies only native FLAC Vorbis-comment text. Existing fields retain
TagLib's exposed uppercase key. The initial conventional mapping for newly
added fields is:

| Trackbench field | FLAC key |
| --- | --- |
| Title, Artist, Album Artist, Album | `TITLE`, `ARTIST`, `ALBUMARTIST`, `ALBUM` |
| Date, Original Date | `DATE`, `ORIGINALDATE` |
| Track Number, Total Tracks | `TRACKNUMBER`, `TOTALTRACKS` |
| Disc Number, Total Discs | `DISCNUMBER`, `TOTALDISCS` |
| Genre, Composer, Performer, Conductor, Lyricist | `GENRE`, `COMPOSER`, `PERFORMER`, `CONDUCTOR`, `LYRICIST` |
| Label, Catalog Number, Barcode, ISRC | `LABEL`, `CATALOGNUMBER`, `BARCODE`, `ISRC` |
| Comment, Grouping, Copyright, BPM, Compilation | `COMMENT`, `GROUPING`, `COPYRIGHT`, `BPM`, `COMPILATION` |
| Subtitle, Version, Language, Media, Encoder | `SUBTITLE`, `VERSION`, `LANGUAGE`, `MEDIA`, `ENCODER` |
| Artist Sort, Album Artist Sort | `ARTISTSORT`, `ALBUMARTISTSORT` |
| Artists, Album Artists | `ARTISTS`, `ALBUMARTISTS` |
| MusicBrainz Artist Id | `MUSICBRAINZ_ARTISTID` |
| MusicBrainz Album Artist Id | `MUSICBRAINZ_ALBUMARTISTID` |
| MusicBrainz Track Id | `MUSICBRAINZ_TRACKID` |
| MusicBrainz Release Track Id | `MUSICBRAINZ_RELEASETRACKID` |
| MusicBrainz Album Id | `MUSICBRAINZ_ALBUMID` |
| MusicBrainz Release Group Id | `MUSICBRAINZ_RELEASEGROUPID` |
| MusicBrainz Work Id | `MUSICBRAINZ_WORKID` |
| MusicBrainz Disc Id | `MUSICBRAINZ_DISCID` |

Arbitrary additions retain their entered separators and uppercase ASCII
letters. Invalid Xiph keys, invalid UTF-8, exact empty values, and artwork keys
are blocked rather than sanitized or reinterpreted. Replacing/removing one
logical field removes all separator/case aliases of that canonical identity
before installing the planned key and ordered values.

## Reading and merge precedence

A playable item may receive metadata from the file/container, an external cue,
a sidecar, Trackbench's persisted annotations, a stream, and a playlist
snapshot. Preserve provenance rather than flattening immediately.

**Trackbench decision (ADR-0033):** effective-value precedence is:

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

The primary surface is a vertical split with a virtualized file selector above
a field-oriented table with Field, Original, and Draft columns. Selecting one
file gives an individual view; selecting several files gives a bulk view. It
provides:

- editable field columns/rows and separately visible read-only technical data;
- a state for common value, mixed values, missing everywhere, and partially
  present;
- individual per-track values in selection order;
- arbitrary new fields;
- type-to-add field creation with fuzzy completion over present, conventional,
  MusicBrainz, and recently used names;
- direct keyboard traversal, range selection, fill, clear, copy, and undo;
- saved field layouts and task presets instead of one global enormous field list;
- correct multi-value editing without semicolon-as-data ambiguity;
- field removal and complete tag removal as distinct operations;
- artwork and ReplayGain sections;
- reload/discard and external-change conflict handling;
- a complete preview before writing.

**Trackbench decision (ADR-0034, M5 read-only grid baseline):** Properties is
now a non-modal workspace reachable from Edit, the track context menu, and
`Alt+Return`. It preserves every selected occurrence as a row and shows a
preferred-first sparse union of ordinary, arbitrary, and MusicBrainz fields as
columns. Headers report common, mixed, missing, or partial state from exact
ordered value vectors; selecting a cell reveals its unjoined values and winning
provenance. Equal values with different provenance remain common. Projection is
bounded, snapshot capture yields between short event-loop slices, aggregation
is dispatched after the dialog shell opens, and the Qt model creates no
persistent per-cell objects. That baseline was deliberately read-only and had
no edit trigger or Apply action.

**Trackbench decision (ADR-0035, M5 draft baseline):** Properties now layers a
bounded sparse in-memory draft over that immutable selection. Keyboard or
double-click editing stages one exact scalar value; repeated values are shown
with an explicit replacement prompt and are never parsed from joined display
text. Delete stages field removal, which remains distinct from replacement;
undo/redo, selected-cell revert, whole-draft discard, restrained draft styling,
and exact Original/Draft inspector rows expose every change. ADR-0041 now
projects complete Draft result states in the background. Closing a dirty
workspace requires discard.
There is still no Apply or file-write path.

**Trackbench decision (ADR-0036, M5 aggregate baseline):** Properties now opens
on the compact Fields/Original/Draft projection. Common Original values remain
exact, while mixed, partial, and missing states stay explicit. Editing Draft
stages one value across the complete selection; Delete removes the field and
`Ctrl+Backspace` reverts it. The Tracks page retains occurrence-aligned cells
and the exact values inspector. Both projections share the same bounded patch
set and undo history. Known uniform bulk intent is shown directly; ADR-0041
projects exact aggregate results after individual exceptions.

**Trackbench decision (ADR-0037, M5 ordered-value baseline):** `Edit values…`
and `Ctrl+Enter` open a non-blocking structured editor for the current bulk
field or individual track cell. One row is one exact value; add/remove/reorder
preserve order, duplicates, delimiter characters, and explicit empty strings.
Zero rows cannot be accepted because field removal remains the separate Delete
operation. Accepted vectors enter the same bounded patch and undo history as
scalar edits, with no file-write path.

**Trackbench decision (ADR-0038, M5 selection-driven Properties):** the separate
Fields and Tracks modes are superseded by one vertical split. A read-only,
multi-select file list sits above Field/Original/Draft, and its selected rows
are the only individual/bulk edit scope. One row exposes exact Original and
Draft values; multiple rows expose common, mixed, partial, or missing states.
Nontrivial subset summaries are debounced, generation-safe, and computed off
the UI thread. Scalar edits, exact ordered values, removal, revert, undo/redo,
and discard continue to share one bounded sparse draft; there is still no
Apply or write path.

**Trackbench decision (ADR-0039, M5 dynamic field vocabulary):** `Add field…`
and Insert accept an arbitrary field name, append a missing row to the current
session vocabulary, and focus its Draft cell. Canonical duplicates select the
existing row. The operation shares immutable per-occurrence baselines instead
of copying the complete selection, and it preserves the selected-file scope
while the backing model gains its hidden field column. `Remove field` and
Delete stage explicit removal across the selected files; for a newly added
field, removal cancels its staged additions. Removed rows remain visible for
partial scopes, revert, and undo.

**Trackbench decision (ADR-0040, M5 field-name completion):** `Add field…`
offers at most 12 deterministically ranked names from fields present in the
selection, workspace-recent additions, a conventional catalog, and the initial
MusicBrainz identity/sort-name set. Matching ignores ordinary field separators
and ranks exact, prefix, substring, then ordered-subsequence matches. Canonical
duplicates prefer the spelling already visible in the selection. The popup is
only a shortcut: arbitrary names remain valid, and catalog membership implies
no write capability.

**Trackbench decision (ADR-0041, M5 complete Draft projection):** the Draft
column is a complete projection of the current selected files plus their sparse
patches. A 40 ms debounce dispatches one cancellable worker over an immutable
copy-on-write snapshot; generations reject obsolete scope/edit results. Sparse
traversal yields exact common values and common/mixed/partial/missing states
without materializing the item-by-field matrix. Pending non-uniform rows remain
labelled as preparing. This is still an in-memory result preview, not a
revalidated file-write plan or Apply capability.

**Trackbench decision (ADR-0042, M5 revalidated write-plan preview):**
`Preview write plan…` snapshots the complete draft and rereads every distinct
staged raw source once on a cancellable worker. The Qt-free plan compares
captured and observed revisions, retains every exact logical occurrence intent,
merges compatible fields for shared paths, and blocks missing/changed sources,
incompatible CUE/duplicate results, non-embedded targets, distinct paths that
alias one inode, unavailable writers, and unproven unknown-data preservation.
Its virtualized table shows Fresh original beside Planned result for every
intent. ADR-0047 now adds Apply only when the complete immutable plan is ready;
blocked previews remain inspection-only.

**Trackbench decision (ADRs 0043–0047, M5 native-FLAC transaction baseline):**
one compatible physical source can be materialized as an exclusive
preservation-verified sibling and committed by the Qt-free operations layer.
The executor locks and revalidates the direct single-link file, journals the
complete immutable plan and raw sibling paths through reversible SQLite
migration 6, preserves owner/mode/bounded Linux extended attributes, creates an
exact hard-link backup, atomically renames the prepared file over the source,
and rereads its revision, document, and every planned field. Completion also
requires an idempotent all-or-nothing callback to refresh every affected
logical occurrence. Failure restores the recorded original identity; startup
recovery completes only unambiguous publication and otherwise preserves
evidence as `needs_reconciliation`. Migration 7 implements that callback as a
provenance-aware transaction over every raw-path occurrence plus a durable
source cache: embedded/stream layers advance while annotations and logical
CUE/chapter/subsong/sidecar overlays remain intact, and recovery replay is
idempotent. Legacy flattened logical snapshots require a fresh probe instead of
guessing provenance. Migration 8 keeps the backup lifecycle separate from
publication state. Trackbench runs recovery and bounded maintenance off the UI
thread after list/cache initialization, opens Metadata operations automatically
for ambiguous evidence, and offers verified single-step undo or explicit backup
release for retained records. Undo atomically exchanges the recorded old and
published identities, rereads and verifies the original fields, and refreshes
all occurrences under a fresh idempotency identity before deleting the replaced
publication. Interrupted undo is recoverable. The fixed initial retention policy
runs at startup and keeps backups for up to seven days, only the newest per exact
raw source, and at most 256 entries/10 GiB globally; ambiguous evidence is never
deleted for a budget. Apply captures and persists the current workspace before
admitting file mutations, then feeds only a wholly ready plan through two
bounded workers. Cancellation stops new admission while in-flight sources reach
a safe boundary. The result keeps plan order and reports committed, failed, and
cancelled sources independently; committed sources refresh every durable and
visible occurrence plus operation history. Because a batch is not a fictional
cross-filesystem transaction, successful sources remain committed after an
unrelated failure, and every retry requires a fresh complete preview.

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
- existing field(s).

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

**Trackbench decision (ADRs 0048–0053, M5 transformation slices):** schema 1
chains are Qt-free ordered declarative data evaluated against the selected
items' current staged draft. The first actions set exact values, remove a
field, trim/lowercase/uppercase each existing value without flattening the
list, capitalize only the first Unicode character of each existing value while
leaving its remainder unchanged, or set one scalar from the typed metadata-
transformation `tkfmt-1` host.
Exact add appends values in order without deduplication; copy mirrors the full
source field state and removes the target when the source is missing; split
uses one non-empty exact separator while retaining leading, trailing, and
adjacent empty components; join writes one exact value and permits an empty
separator.
Later actions observe all earlier results. An empty format result is one
explicit empty value, not removal. A cancellable bounded planner emits only
changed final cells with exact missing/value states and the last writer step.
Properties requires that immutable preview and stages it only if every before
value still matches the live draft; all cells then enter the existing sparse
patch model as one undoable transaction. The normal fresh physical write plan,
format capability checks, and Apply remain mandatory. Reversible SQLite
migrations 9–11 store at most 256 non-empty, exact-name chains in normalized
ordered action/value rows with stable schema codes. Properties loads, saves,
updates, saves-as-new, and deletes them through the serialized persistence
worker. Its **Tagging scripts** side panel lists every definition with a
persistent checkbox and opens the selected row in the editor. Checked chains
run in displayed name/stable-ID order after manual draft edits and before fresh
write-plan revalidation. Their results are applied to a temporary draft copy,
so the final immutable plan contains the complete tagging operation while the
visible draft and undo history remain manual and repeated previews cannot
compound additive actions. Explicit one-off transformation preview/staging and
the final explicit Apply boundary remain available.
Per ADR-0052, Properties is a temporary protected `Tags · N tracks` workspace
tab rather than a separately sized top-level window. Its transformation preview
shows Field/Old/New at the top level and retains affected-file plus producing-
step diagnostics as an expandable child beneath each exact changed cell.
Transformation target fields reuse the ranked canonical completion behavior
over field names actually present in the selected tags; completion is guidance
only, and arbitrary new target names remain valid.
Per ADR-0053, remove-matching and replace-matching compare complete values by
case-sensitive valid-UTF-8 byte equality, with no normalization, substring,
glob, regular-expression, or locale behavior. Remove deletes every match and
makes the field missing when none remain; replace expands every match to a
bounded non-empty ordered replacement sequence, where an empty replacement
string remains a present value. Selection-order numbering replaces the target
with one consecutive decimal value in captured file order using a bounded
start and optional minimum-width zero padding. It neither infers groups nor
writes `TOTALTRACKS`; grouped/title-format numbering must later preview its
boundaries and totals explicitly. Reversible migration 12 stores these three
typed actions under stable codes without reinterpreting older definitions.
Import/export, grouped numbering, richer match dialects, and the separately
versioned capture-pattern parser remain future slices.

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

**Trackbench decision (ADRs 0054–0055):** rename/move belongs to the same typed
preparation plan as optional tag persistence and qualified ReplayGain storage.
The tagging workspace will expose independent Save tags, Rename files, Move
files, and ReplayGain choices only as each operation gains a real immutable
preview and journaled Apply path. Checked scripts and manual edits produce the
final in-memory metadata context before path evaluation; if Save tags is off,
the preview must distinguish values used for naming from values actually
written.

A versioned output-layout profile stores separate relative-directory and
basename `tkfmt-1` expressions plus its sanitization-policy version. An absolute
raw-path destination root is a separate named profile. Rename without Move
changes only the basename in the current directory; Move without Rename applies
the directory layout below the selected root while preserving the original
basename; enabling both evaluates both components. Existing-file operations
preserve the source extension. The converter may reuse both profile types while
owning its output extension and permitting explicit per-job overrides.

Combined content/path operations prepare and verify the final file directly at
the destination rather than rewriting the source and then moving it. Same-
filesystem path-only changes may use atomic rename; cross-filesystem moves must
copy, verify, durably publish, and only then delete the source. The recoverable
journal and dependent-state transaction cover raw source/target paths,
sidecars, list occurrences, playback state, metadata cache, and statistics.

The operation accepts a selected track set, destination root, and `tkfmt-1`
relative-path pattern. Separators in the evaluated pattern request
subdirectories below the explicit root.

The first implemented `linux-v1` policy treats those directory separators as
structure, replaces `/` in a generated basename and all generated NUL bytes
with `_`, maps empty and `.` components to `_` and `..` to `__`, and performs
no Unicode normalization, transliteration, whitespace trimming, or portable-
platform reserved-name rewriting. Raw and sanitized results remain side by
side in the immutable plan. The pure planner consumes captured source
revisions plus an explicit existing-path snapshot, detects device/inode aliases
and lexical conflicts, and performs no I/O. Symlink/mount resolution,
permissions, exact filesystem limits, and publication capability require a
fresh later preflight and are not implied by a ready pure plan.

**Trackbench decision (ADR-0056):** the fresh preflight now walks every existing
path component from `/` without following symlinks, requires an unchanged
single-link regular source, rejects newly occupied targets and unusable
parents, tightens component/path limits from the target filesystem, and lists
missing target directories without creating them. Source/target device
identity explicitly classifies atomic rename versus cross-filesystem copy.
Migration 14 persists fixed prepared-target, published-target, dependent-state,
source-removed, complete, rollback, and reconciliation boundaries. This is
recovery structure rather than mutation authority.

**Trackbench decision (ADR-0057):** the first executor slice now qualifies
same-filesystem path-only publication. It repeats descriptor-relative source,
target, device, access, and missing-directory checks; journals before `mkdirat`;
publishes only with `renameat2(RENAME_NOREPLACE)`; syncs both directory entries;
and verifies the original inode at the target. An idempotent all-or-nothing
dependent-state callback defines the rollback boundary: failure before callback
success restores the exact source path, while a journal failure afterwards
retains the target and replays the callback during startup recovery. Ambiguous
topology is never deleted. Case-only aliases on case-folding filesystems remain
unsupported because ordinary rename cannot retain the no-replace guarantee.
The concrete list/cache/playback relocation transaction, undo, cross-filesystem
copy, bounded batch execution, and UI choices remain unavailable.

### Plan pipeline

```text
selected TrackRefs
  -> evaluate format expression
  -> validate/sanitize each path component
  -> resolve normalized absolute targets
  -> detect conflicts and relationships
  -> show immutable lexical plan
  -> fresh non-mutating filesystem preflight
  -> show publication kind and live blockers
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
