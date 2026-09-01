# ADR-0078: Immutable native-FLAC artwork write plan

- Status: accepted
- Date: 2026-09-01
- Owners: Trackknife project
- Extends: ADR-0043 preservation-verified native-FLAC prepared copies,
  ADR-0044 journaled metadata publication, and ADR-0076 bounded artwork
  inventory
- Extended by: ADR-0079 journaled native-FLAC artwork publication

## Context

Trackbench can inventory every native FLAC picture and present that evidence in
Properties, but it cannot yet describe an artwork change without passing
mutable backend objects or encoded image bytes through the UI. Enabling a
TagLib picture call directly from Properties would give preview and execution
different inputs and would not prove that the selected picture, replacement
file, unrelated pictures, text, unknown blocks, and audio still match the
reviewed operation.

Artwork changes also need a deliberate recovery contract. The current metadata
journal serializes text-field changes; pretending an artwork change is a text
change would make restart verification unsound. Conversely, storing image
bytes or a session inventory in SQLite would duplicate the user's files and
make stale database state an artwork authority.

## Decision

- The first artwork mutation domain is an immutable Qt-free batch plan for
  native FLAC embedded pictures. Each logical intent carries an occurrence
  index, exact raw media path, captured media revision, target native ordinal
  and SHA-256 fingerprint, and either `replace` or `remove`.
- Repeated logical occurrences of one raw media path collapse into one physical
  source when their intents agree. Conflicting intents, inconsistent captured
  revisions, and distinct paths resolving to one device/inode are visible
  blockers. Planning is bounded, cancellable, and re-inventories each source;
  stale revisions, missing or changed target pictures, and unavailable adapters
  never produce a ready source.
- Replacement accepts one exact PNG or JPEG file. Planning reads it under a
  revision bracket and captures its raw path, revision, MIME type, dimensions,
  byte size, and SHA-256 fingerprint. Encoded bytes are transient and are not
  returned in the plan or persisted. A replacement identical to the target is
  a blocked no-op.
- Replace changes only the target picture's encoded image, MIME type, and
  dimensions. It retains that picture's exact native FLAC type and description;
  color depth and indexed-color count become unknown (`0`) because the initial
  signature inspector does not claim those mappings. Remove deletes only the
  exact captured ordinal. Add, role/type editing, description editing, external
  artwork mutation, and non-FLAC mappings remain unsupported.
- A prepared-copy adapter consumes one ready source plan. It revalidates the
  media and replacement revisions and fingerprints, writes only to an exclusive
  caller-supplied path, and leaves the source read-only. The prepared result is
  reread and must match the complete planned embedded inventory.
- Every unrelated picture retains its exact serialized FLAC picture payload and
  order. Vorbis comments and every non-picture/non-padding metadata block are
  byte-compared in order, compressed audio is byte-identical, and the reread
  text document is unchanged. The source revision must still match after all
  verification. Failure or cancellation removes only the prepared path created
  by the adapter.
- A real native-FLAC read may advertise picture-writing adapter capability once
  this prepared-copy proof passes. That capability does not expose an action:
  Properties continues to report artwork changes unavailable until publication
  and recovery are implemented.
- Artwork publication must reuse the existing locked, journal-before-mutation,
  prepared-sibling, atomic-replacement, dependent-state, rollback, and retained-
  backup lifecycle. Before it is reachable, the durable journal must explicitly
  record an artwork operation kind, target ordinal and original fingerprint,
  replacement fingerprint when present, and occurrence indexes. Recovery must
  reread the exact recorded source/prepared revision and verify the planned
  final inventory before completing dependent state. No encoded image or
  inventory row belongs in SQLite.

This slice implements the immutable plan and verified prepared-copy adapter. It
does not add a migration, executor, button, Apply path, backup, or durable image
cache.

## Alternatives considered

### Put replacement image bytes in the plan

Rejected. It would make large plans own another copy of every image and invite
accidental persistence. Revision plus content identity lets execution reread
the user-selected file and fail closed when it changes.

### Address a picture only by role

Rejected. A FLAC may contain several front covers or native types that collapse
to one Trackbench role. Ordinal plus fingerprint identifies exactly what was
reviewed without silently choosing one.

### Replace every picture of the selected role

Rejected. That is a separate bulk transformation and would destroy distinctions
the typed inventory deliberately preserves.

### Reuse the current text-change journal without a schema change

Rejected. Text field rows cannot express picture identity or prove the final
inventory during restart recovery. The future journal extension must remain
small evidence, not an image store.

### Publish immediately through the metadata executor

Rejected. Its recovery verifier currently understands text results only. A
prepared copy proves the format mapping but does not authorize an incompletely
journaled destructive action.

## Consequences

- Preview and later execution share exact source, target, and replacement
  identities without retaining encoded bytes.
- Replacing one image can deliberately reset unproven color-depth hints while
  preserving its semantic native type and description.
- The adapter can later plug into unchanged-path metadata publication or the
  destination-artifact pipeline after artwork journal evidence and dependent
  refresh are qualified.
- Users still cannot mutate artwork from Properties in this slice.

## Validation

- Real FLAC tests build ready replace and remove plans from the repository's
  embedded-PNG fixture and a real JPEG replacement, including duplicate logical
  occurrence collapse and complete replacement evidence.
- Planner tests cover stale media, changed target identity, conflicting logical
  intents, physical aliases, missing/invalid replacement input, no-op
  replacement, limits, and cancellation.
- Prepared-copy tests construct a multi-picture FLAC, replace and remove the
  selected ordinal, and prove exact unrelated-picture serialization, unchanged
  text and unknown blocks, byte-identical compressed audio, equal decoded PCM,
  source immutability, exclusive output, stale replacement rejection,
  cancellation, and failure cleanup.

## Revisit when

- Resolved by ADR-0079: migration 23 and complete-inventory recovery evidence
  qualify unchanged-path publication, restart recovery, and undo;
- Properties receives replace/remove review and bounded Apply controls;
- add, export, copy, role/type, or description operations are specified;
- safe color-depth inspection is qualified for supported replacement formats;
- another container receives a fixture-backed artwork writer.
