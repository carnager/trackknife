# ADR-0081: Native-FLAC artwork Add, Export, and Copy

- Status: accepted
- Date: 2026-09-01
- Owners: Trackknife project
- Extends: ADR-0076 bounded artwork inventory, ADR-0078 immutable artwork
  planning, ADR-0079 journaled publication, and ADR-0080 Properties Apply

## Context

ADR-0080 exposes only Replace and Remove. The M5 artwork contract also calls
for adding an image, exporting encoded artwork, and copying one inventoried
image across the selected tracks. These actions have different authority:
Add and Copy mutate native-FLAC destinations and therefore need fresh review,
journaling, recovery, and Undo; Export never mutates a media source and must
not create another retained-backup lifecycle.

Copy also must not achieve convenience by writing an embedded donor to an
untracked temporary image file. The inventory already has sufficient compact
identity—raw source, revision, ordinal, MIME, dimensions, size, and SHA-256—to
reread its bytes safely when the prepared destination is built.

## Decision

- `ArtworkWritePlanIntentKind` fixes its persisted numeric values explicitly:
  Replace is 0, Remove is 1, and Add is 2. Add has no original target. Fresh
  plan validation derives its resulting ordinal from the exact embedded-picture
  count and rejects an image whose encoded SHA-256 already exists in the
  destination.
- Add appends one semantic picture to the end of the native-FLAC picture order.
  Trackbench asks for one PNG/JPEG and one role (`front`, `back`, `artist`,
  `disc`, `icon`, or `other`) and maps that role to a canonical native FLAC
  picture type. The initial Add UI uses an empty description. The direct FLAC
  rewrite inserts the serialized block immediately after the last existing
  picture block, or before the first padding block when there is no picture.
  Every pre-existing picture block, unrelated metadata/padding block, and
  compressed-audio byte remains serialized unchanged.
- Reversible schema 24 extends compact artwork journal evidence for Add. Its
  original-target SHA-256 is null, its target ordinal equals the original item
  count, it carries a replacement SHA-256, and its planned count is original
  count plus one. Complete original/planned inventory digests retain the same
  publication, restart-recovery, and exact old-inode Undo proof. SQLite still
  stores no image bytes, donor path, or inventory snapshot.
- **Copy to Selection** treats exactly one inventoried row as the donor and Add
  as the destination operation. An embedded donor is excluded from its own
  media source; an external sibling may be copied into its associated source.
  Copy retains the donor's Trackbench role and description. Every destination
  must be a revision-matching native FLAC source. Duplicate bytes or stale
  donor/destination evidence block the fresh review.
- An embedded Copy plan retains only the donor's inventory evidence. Prepared
  writing rereads the exact ordinal from the exact donor revision, verifies
  MIME, dimensions, size, and SHA-256, and holds encoded bytes only for that
  bounded call. No temporary donor image is created. External donors use the
  same standalone-file revision bracket.
- **Export…** accepts selected embedded or external rows, asks for one output
  directory, and derives deterministic ASCII names in selection order:
  `artwork-N-role` plus a MIME-derived suffix (or `.bin` for an unknown MIME).
  A two-worker Qt-free job rereads every image under its inventory evidence,
  reports ordered partial results and progress,
  supports cancellation, and creates each output with `O_EXCL`. It durably
  closes the file and parent directory; it removes only a partial path created
  by that request. Existing files are never overwritten.
- Properties exposes Add whenever the entire selected-file scope is writable
  native FLAC, Copy when one supported donor has at least one eligible
  destination, and Export for any selected readable row. Replace/Remove remain
  limited to embedded native-FLAC rows. External artwork and non-FLAC media are
  therefore donors/export sources only, never mutation targets through their
  own adapter.

## Alternatives considered

### Put Copy on the desktop clipboard

Rejected. The product contract says copy across selected tracks. Clipboard
copy neither satisfies that workflow nor receives the mutation safety contract.

### Export an embedded donor to a temporary file before Copy

Rejected. It creates image-bearing filesystem state outside the reviewed
operation lifecycle and weakens donor identity. Revision/ordinal/hash evidence
is enough to reread the image transiently.

### Overwrite existing Export paths after confirmation

Rejected. A modal confirmation does not make a bulk partial result easier to
reason about. Exclusive creation makes every output outcome deterministic and
retryable with a different directory or after explicit user cleanup.

### Reuse padding bytes instead of inserting a picture block

Rejected for this slice. Repacking padding complicates the preservation proof.
Adding one explicit block while streaming all existing blocks unchanged is
simple and independently verifiable.

## Consequences

- Native-FLAC artwork now supports view, Add, Replace, Remove, Export, and Copy
  across the Properties selection with one coherent review/Apply model.
- Copy does not write temp images, and Export does not create SQLite records or
  retained old-inode backups.
- Add/Copy remain all-ready batch mutations; one duplicate or stale source
  blocks Apply until a new review has a coherent scope.
- Other container writers remain read-only. External artwork files remain
  unmodified even when used as Copy donors.

## Validation

- Qt-free plan tests cover Add ordinal derivation, role/description evidence,
  duplicate rejection, embedded-donor revalidation, and transient exact-byte
  reread.
- Real FLAC prepared-copy tests cover zero/multi-picture Add and embedded Copy,
  canonical type/description, exact pre-existing picture identity, untouched
  text and unknown blocks, compressed-audio identity, and source immutability.
- Journaled real-file tests cover schema-24 Add evidence, publication, reread,
  retained exact Undo, and the existing restart-recovery verifier.
- Export tests cover exact embedded bytes, two-worker ordered results,
  no-overwrite partial failure, cancellation without debris, and stale-donor
  rejection.
- Offscreen Properties tests cover capability labels and Add/Copy/Export
  gating alongside the existing real Remove Apply path.

## Revisit when

- a non-FLAC container receives a fixture-backed artwork writer;
- Add needs arbitrary exact native FLAC picture types or editable description;
- the export naming template becomes user-configurable;
- replacement from one embedded donor across a selected batch is requested in
  addition to Copy-as-Add.
