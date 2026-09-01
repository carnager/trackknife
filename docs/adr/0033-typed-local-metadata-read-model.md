# ADR-0033: Typed local metadata read model and conservative TagLib boundary

- Status: accepted
- Date: 2026-08-29
- Owners: Trackknife project
- Complements: ADR-0006 focused media/storage backends and ADR-0025 the
  standalone Trackbench application
- Amended by: ADR-0076 bounded typed artwork inventory, ADR-0078 immutable
  native-FLAC artwork write plans, and ADR-0079 journaled artwork publication

## Context

M4 projected a few FFmpeg tags directly into Trackbench list columns. That was
enough to label playable rows, but it discarded repeated values, native field
identity, MusicBrainz relationships, qualifiers, provenance, and the source
revision needed by a safe editor. It also encouraged separate metadata paths
for ordinary files, external cues, chapters, and codec-native subsongs.

M5 needs a read foundation before it can offer a field grid or any mutation.
Selecting TagLib as a backend does not itself define Trackbench's domain model
or prove that a format can be rewritten without losing native data.

## Decision

- `Trackknife::Metadata` is a Qt-free library. Its document is an ordered
  collection of fields. Each field retains a Trackbench-owned canonical lookup
  name, the adapter's native name, ordered string values, optional
  language/description qualifiers, and provenance. Native objects that the
  generic text projection cannot represent are inventoried by opaque identity.
- Canonical lookup performs only deterministic ASCII folding: `A`–`Z` become
  lowercase and spaces, underscores, and hyphens are ignored. Other bytes and
  punctuation remain exact. Native spelling is never rewritten by lookup.
- Effective values use explicit precedence: sidecar, logical segment,
  embedded/stream, annotation, then cached snapshot. Fields and values at the
  winning level retain document order and adapter-exposed value order. External
  CUE projections overlay the physical file document; container chapters and
  tracker subsongs use the same document and display projection with segment
  provenance.
- MusicBrainz recording, release-track, release, release-group, artist,
  album-artist, work, and disc identities have a typed projection. Credited
  artist values remain distinct from sort names and identifiers.
- TagLib 2.0 or newer is the first read adapter. It reads the generic
  `PropertyMap`, retains repeated values and native exposed keys, and records
  identities from `unsupportedData()`. FFmpeg may supply otherwise missing
  container/stream names for list display, but does not replace the primary
  property representation.
- Every read is bracketed by raw-path `stat` observations containing device,
  inode, size, and nanosecond mtime. A change during the read is a typed
  conflict. Raw Linux path bytes pass to the adapter without UTF-8 conversion.
- Reads are bounded to 4,096 fields, 16,384 values, and 4 MiB of projected text.
  TagLib's synchronous call runs only inside Trackbench's existing bounded
  background probe work; cancellation is checked before and after that backend
  boundary.
- Trackbench list snapshots cache only effective canonical fields and ordered
  values. They are sufficient for fast restart display, but omit source
  revisions and are never authority for a later tag commit. A mutation must
  freshly read and revalidate the physical source.
- The initial adapter advertised field-read capability only. ADR-0043 later
  qualified native-FLAC text writes and preservation, while ADR-0076 qualifies
  native-FLAC picture reads through a separate bounded inventory. Picture
  writes and other embedded mappings remain false until each format passes
  repository-owned native round-trip and preservation tests.

## Alternatives considered

### Continue using FFmpeg's flattened probe tags

Rejected. The probe projection cannot provide the native metadata structure or
write-preservation evidence M5 needs, and display-first-value helpers discard
ordered repeated data.

### Expose TagLib types throughout Trackbench

Rejected. Backend types would leak into UI and operation planning, make cached
or sidecar metadata awkward, and turn an implementation library's behavior into
the persisted product contract.

### Enable TagLib writes with the first reader

Rejected. Successful parsing does not prove preservation of artwork, unknown
frames/atoms/chunks, audio essence, padding, or malformed-but-readable data.
Write claims remain format-specific.

### Persist the complete native read document in list SQLite

Rejected for this slice. Lists are presentation/workspace snapshots, not a
local metadata database. Persisting effective values avoids stale native state
being mistaken for commit authority; staged editor documents will own their own
revision-aware lifecycle.

## Consequences

- Ordinary files and logical rows share one metadata/display path while
  retaining their distinct playback identity and segment semantics.
- Arbitrary and MusicBrainz multi-values survive list restart, but unsupported
  native-object inventories are intentionally refreshed from the file.
- Exact format read coverage may exceed the initial real fixtures, but no
  format is advertised writable from this generic adapter.
- The field grid, staged patch model, artwork readers, and format-specific write
  adapters can extend this document without depending on Qt or FFmpeg types.

## Validation

- A real FLAC fixture carries repeated `ARTIST`, `ARTISTS`,
  `MUSICBRAINZ_ARTISTID`, and custom-field values plus the initial MusicBrainz
  identity set. Reader tests require exact value order, native exposed keys,
  typed identity projection, stable source revision, and read-only capability
  flags.
- A real WavPack fixture and a copy addressed through an invalid-UTF-8 Linux
  filename exercise another tag family and raw path bytes.
- Cancellation, missing paths, and non-regular sources return typed errors.
- An offscreen Trackbench regression reads the rich FLAC through the background
  probe, verifies its display projection and full values, restarts from SQLite,
  and verifies repeated custom and MusicBrainz values remain ordered while the
  stale source revision is absent.

## Revisit when

- the staged editor presents ADR-0076 picture inventories;
- the first per-format writer and preservation corpus are ready;
- sidecar format and annotation namespaces are accepted;
- source revisions gain an optional metadata-region hash.
