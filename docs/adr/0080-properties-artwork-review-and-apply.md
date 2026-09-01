# ADR-0080: Properties artwork review and Apply

- Status: accepted
- Date: 2026-09-01
- Owners: Trackknife project
- Extends: ADR-0077 Properties artwork presentation, ADR-0078 immutable
  native-FLAC artwork plans, and ADR-0079 journaled artwork publication

## Context

ADR-0079 qualifies crash-safe publication but intentionally exposes no mutating
control. Properties still reports native-FLAC artwork changes as unavailable,
so users cannot reach the proven Replace/Remove path. A usable surface must keep
the shared file selector authoritative, revalidate files and replacement input
away from the UI thread, make the exact batch immutable before Apply, and avoid
turning a partial result into an unsafe retry.

The prepared-copy writer also exposed a narrower preservation problem while
qualifying the UI with an untouched single-picture fixture. Asking TagLib to
remove that picture could normalize an unrelated native FLAC metadata block.
The existing byte-preservation verifier rejected the result correctly, but a
control advertised as writable must work without collateral normalization.

## Decision

- The Artwork inventory uses extended row selection. Replace and Remove are
  enabled only when at least one selected row is an embedded picture from a
  revision-matching `taglib-flac-picture-v1` source and the durable mutation
  service is available. External sibling images stay visible and read-only.
- Every selected embedded row expands through the shared Properties file scope
  to its exact logical occurrence indexes. The intent retains captured media
  revision, embedded ordinal, and target SHA-256. Replace chooses one raw PNG or
  JPEG path for the selected batch; its bytes are read only during fresh plan
  validation and prepared-copy execution.
- A cancellable worker builds a new immutable artwork plan for every review.
  The window-modal review shows per-file readiness, operation, role, ordinal,
  replacement path, affected occurrence count, and blocker details. Apply is
  present only when every physical source is ready. Selecting incompatible
  picture rows for one physical source therefore produces the existing
  explicit conflicting-intent blocker instead of an ambiguous write.
- Artwork Apply uses a Qt-free bounded batch executor with one to eight workers;
  Trackbench selects two. Progress callbacks are serialized and results remain
  ordered by reviewed source. Cancellation stops new admission while in-flight
  journaled publications reach a coherent boundary. Runtime failures are
  per-source, successful sources remain committed, and every retry requires a
  new review of fresh files.
- Before mutation, Trackbench persists its captured workspace and opens the
  existing schema-23 metadata-operation journal. Each source uses ADR-0079's
  executor and ADR-0045's all-occurrence refresh. Successful results update
  visible rows and operation history, advance the Artwork section's captured
  revision, and trigger a fresh file-backed inventory without closing
  Properties.
- Properties retains mutation-service callbacks even when they arrive before
  its asynchronous grid and Artwork page exist. Review and Apply make the
  temporary workspace close-protected; closing requests cancellation and waits
  for the safe boundary.
- The native-FLAC artwork prepared copy rewrites the FLAC metadata block stream
  directly. It replaces or omits only the reviewed picture block, adjusts the
  final-block marker, and streams every other metadata block, padding block,
  and compressed-audio byte unchanged. TagLib remains the bounded parser and
  serializer for the reviewed picture payload, but it no longer saves the
  whole artwork file and therefore cannot normalize unrelated blocks.
- The UI and batch layer retain no encoded image bytes. SQLite continues to
  store only ADR-0079's compact crash-recovery evidence. The existing bounded
  filesystem old-inode retention remains the byte-exact Undo mechanism.

## Alternatives considered

### Put artwork changes into the tag draft and its Apply button

Rejected. Artwork targets have ordinal/hash identity and replacement-file
evidence distinct from field/value patches. Combining the draft models would
make either scope or conflict reporting less precise.

### Apply selected rows immediately

Rejected. A selection is stale presentation state, not a mutation plan. The
fresh immutable review is the conflict and capability boundary.

### Retry only failed members of the old plan

Rejected. Successful publications change revisions and a failed source or
replacement may also have changed. Retry must rebuild evidence.

### Allow TagLib to normalize unrelated native blocks

Rejected. Unknown and unrelated metadata preservation is an acceptance
criterion, and the verifier must not be weakened to make a UI path appear to
work.

## Consequences

- Native-FLAC embedded pictures now advertise and expose Replace/Remove when
  their exact source revision matches Properties.
- Multi-file artwork changes have bounded progress, cancellation, partial
  results, crash recovery, all-occurrence refresh, and fresh-review retry.
- External artwork, other containers, Add, Export, and Copy remain visibly
  unavailable until separately qualified.
- Direct block rewriting removes collateral native-FLAC metadata normalization
  and keeps the existing byte-preservation checks meaningful.

## Validation

- Qt-free tests cover ordered partial success, serialized progress, bounded
  admission, and cancellation of pending artwork sources.
- Existing real multi-picture writer/commit tests still prove replacement,
  removal, unrelated-picture identity, text/unknown-block preservation,
  compressed audio identity, journaled publication, recovery, and exact Undo.
- A real untouched single-picture FLAC is removed through the offscreen
  Properties selection, immutable review, explicit Apply, and refreshed
  inventory. The result carries the selected occurrence index and publishes
  without changing preserved blocks.
- Offscreen tests keep unavailable services and external rows read-only and
  preserve asynchronous Properties geometry restoration.

## Revisit when

- native-FLAC Add, Export, or Copy is specified;
- another container receives a fixture-backed artwork writer;
- heterogeneous per-row replacement inputs are needed;
- the fixed retained-backup policy becomes user-configurable.
