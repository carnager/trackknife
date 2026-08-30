# ADR-0043: Preservation-verified prepared-copy FLAC metadata writer

- Status: accepted
- Date: 2026-08-29
- Owners: Trackknife project
- Extends: ADR-0033 conservative metadata adapter capabilities and ADR-0042
  revalidated metadata write plans

## Context

The metadata write-plan preview can prove source identity, merge compatible
logical edits, and expose format capability blockers, but no real adapter is
yet allowed to write. Enabling TagLib's generic `save()` for every readable
format would not prove that artwork, unknown container structures, untouched
text fields, or audio essence survive.

The first writer boundary also must not prematurely define commit, recovery, or
replacement semantics. Those require a second plan revalidation, per-source
locking, a durable journal, atomic publication, reread verification, cache/list
updates, and reconciliation.

## Decision

- The first writable mapping is native FLAC (`fLaC`) Vorbis-comment text through
  a dedicated Qt-free adapter, identified as `taglib-flac-v1`. Ogg FLAC,
  ID3-prefixed FLAC, other TagLib formats, artwork mutation, and container block
  mutation remain unsupported.
- A real FLAC read advertises text-field writing and unknown-data preservation
  only when TagLib resolves it to its concrete FLAC implementation and the file
  begins with the native FLAC marker. Picture read/write capabilities remain
  independent and false in the metadata adapter.
- The writer consumes one ready, freshly observed `MetadataWritePlanSource` and
  writes only to a caller-supplied path that must not exist. It revalidates the
  plan's observed source revision immediately before copying and again after
  verification. The source is opened read-only and is never replaced or
  removed by this primitive.
- Existing fields retain the freshly exposed native key. Known newly added
  fields use the documented native-FLAC conventional mapping; arbitrary fields
  use their staged display spelling with ASCII letters uppercased. Keys must be
  valid Xiph-comment keys, must resolve to the planned canonical identity, and
  must not address artwork conventions reserved for the future typed artwork
  path. All native keys with the same Trackbench canonical identity are removed
  before the one exact planned value list is installed, preventing
  separator/case aliases from leaving a hidden duplicate.
- Ordered values are passed individually; no joined display string is parsed.
  Removal remains a distinct operation. An exact empty value is rejected for
  this adapter because TagLib's Xiph-comment mutation API treats it as field
  removal; invalid UTF-8 is likewise rejected because Vorbis comments are UTF-8
  text. The plan must not claim a representation it cannot reproduce.
- Before publishing a successful prepared result, the adapter rereads it and
  requires every intended field result plus every untouched effective text
  field to match. It parses both native FLAC layouts and byte-compares every
  non-Vorbis-comment/non-padding metadata block in order, including STREAMINFO,
  APPLICATION, SEEKTABLE, CUESHEET, PICTURE, and reserved block types. It also
  byte-compares the complete compressed-audio region. Comment size and padding
  changes are intentionally allowed.
- Cancellation is checked around bounded copy, backend save, reread, and
  chunked comparisons. Any failure unlinks only the output path created by this
  call. A successful result returns the prepared file's revision and reread
  metadata document.
- This adapter makes compatible FLAC previews ready, but Trackbench still has
  no Apply action. Prepared-copy creation is an internal mutation primitive,
  not authorization to publish it over the user's source.

## Alternatives considered

### Write the source in place and keep a backup

Rejected for this slice. Backup naming, durability, crash recovery, restoration,
and cache/list transactions belong to the journaled executor and must be
designed as one boundary.

### Trust TagLib's successful return value

Rejected. A successful save proves neither preservation nor the exact logical
result. The adapter verifies the rewritten container and its reread projection
itself.

### Compare only decoded PCM

Rejected. Equal decoded samples could still hide a changed FLAC bitstream,
STREAMINFO, seek table, or application data. The adapter compares the compressed
audio and preserved metadata blocks byte-for-byte; fixture tests additionally
decode both sides as an independent validity check.

### Treat empty strings as removal

Rejected. The draft model deliberately distinguishes an exact empty value from
field removal. Unsupported exact values must block instead of silently changing
meaning.

## Consequences

- Native FLAC text-only plans can become genuinely adapter-ready without
  exposing an unsafe commit path.
- Preservation is checked for every prepared output, not inferred forever from
  one backend/version fixture.
- The prepared copy may have different comment/padding layout and size while
  still preserving all user data outside the intended text edits.
- Later journaled execution can reuse this primitive, but must still handle
  locks, temporary-sibling placement, durability, atomic replacement, backups,
  rollback/reconciliation, and dependent Trackbench state.

## Validation

- Repository-owned FLAC fixtures cover ordered repeated arbitrary and
  MusicBrainz values, untouched ReplayGain text, embedded artwork, an unknown
  APPLICATION payload, and decoded PCM.
- Tests require intended replace/add/remove results, exact value order,
  untouched text equality, byte-identical preserved blocks and compressed
  audio, exact artwork bytes, equal complete decoded PCM, source immutability,
  source-revision conflicts, invalid mappings, cancellation, existing-output
  rejection, and failure cleanup.
- Reader and write-plan tests require only qualifying native FLAC sources to
  advertise `taglib-flac-v1` and become ready.

## Revisit when

- the journaled executor publishes a prepared sibling over the source;
- exact empty Vorbis-comment values gain a proven lower-level mapping;
- typed artwork or ReplayGain mutation is added;
- Ogg FLAC or ID3-prefixed FLAC gets its own preservation corpus;
- additional TagLib versions require separate adapter qualification.
