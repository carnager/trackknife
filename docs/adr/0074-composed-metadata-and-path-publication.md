# ADR-0074: Composed metadata and path publication

- Status: accepted
- Date: 2026-08-31
- Owners: Trackknife project
- Extends: ADR-0045 provenance-aware dependent metadata state, ADR-0059
  all-occurrence source relocation, ADR-0063 bounded file Apply, ADR-0067
  immutable preparation review, and ADR-0073 destination-artifact publication

## Context

ADR-0073 qualified one changed destination artifact through publication,
recovery, dependent-state commit, and exact source removal. Properties still
could not execute a ready plan containing both changed native-FLAC metadata and
Rename or Move. The existing dependent callbacks could either refresh metadata
at an unchanged path or relocate every occurrence while retaining its previous
metadata cache, but neither expressed the single logical change.

Running those callbacks sequentially would expose a durable intermediate state:
lists could point at the destination with stale metadata, or metadata could be
refreshed at the source before its path changed. A crash or an ordinary delayed
workspace save could preserve that split state. Startup recovery also needs the
same transaction after a changed artifact has already been published.

## Decision

- Reversible schema migration 22 adds `metadata_refreshed` to each
  `local_source_relocations` idempotency record. Existing path-only records
  default to false.
- The all-occurrence relocation request may carry the exact published
  `MetadataDocument`. Without it, the existing path-only transaction rekeys the
  source cache. With it, one SQLite transaction:

  1. revalidates the recorded source path and source revision;
  2. relocates every matching list occurrence to the recorded target revision;
  3. preserves logical annotation, external-CUE, chapter, subsong, and sidecar
     overlays;
  4. replaces embedded and stream-derived fields with the verified published
     document;
  5. removes the source cache, installs the target cache and revision evidence,
     and records `metadata_refreshed = 1`.

- Operation-id replay is idempotent only when source, target, revisions, and
  metadata-refresh intent agree. Reusing a path-only relocation id for a
  combined publication, or the reverse, fails closed.
- The immutable preparation assembler pairs metadata and path work only when
  their raw source path and captured source revision are identical. A mismatch
  blocks the complete plan. Changed tags plus Rename or Move is no longer a
  structural blocker once both reviewed subplans are ready.
- One Qt-free bounded 1–8-worker preparation Apply composes the existing
  executors per physical source:

  - metadata with no path change uses the journaled metadata executor;
  - a path change with unchanged metadata uses byte-preserving file
    publication;
  - changed metadata with a path change prepares the native-FLAC artifact
    directly in the destination directory and passes its verified document to
    the combined dependent transaction;
  - an unchanged source reports an ordered no-change result.

  Progress, cancellation, fresh per-source admission, shared-directory
  coordination, and ordered partial results retain ADR-0063 semantics.
- Trackbench persists the captured workspace before Apply. Its active-player
  relocation barrier runs before the durable combined repository transaction
  and is compensated if that transaction fails. Visible occurrences receive
  the committed target path and, for an artifact, the exact published metadata.
- The file-publication journal remains the sole recovery owner. It deliberately
  does not duplicate a serialized metadata document. During startup recovery,
  Trackbench rereads the published artifact off the UI thread, requires its
  exact recorded target revision, and supplies that document to the same
  combined dependent transaction before source removal can complete.
- Changed-artifact operations remain non-undoable. Their history explicitly
  distinguishes a changed metadata artifact from a byte-preserving path
  operation; it never offers the same-filesystem byte-exact path undo.

This qualifies combined native-FLAC text metadata plus Rename/Move in
Properties. It does not qualify artwork mutation, another metadata writer,
companion-file moves, cross-filesystem undo, or ReplayGain work.

## Alternatives considered

### Relocate first, then refresh metadata

Rejected. The intermediate target can retain stale cache data after a crash,
and a delayed workspace save can make that stale view durable.

### Refresh metadata at the source, then relocate

Rejected. The source would describe destination content that has not been
published, recreating the split operation rejected by ADR-0073.

### Store the complete metadata document in the file journal

Rejected. The published file and its exact recorded revision are the recovery
truth. Duplicating a rich document would add another evolving serialization
contract and could become stale relative to the verified artifact.

### Require every metadata source to change path

Rejected. One reviewed batch may legitimately contain sources whose naming
expression changes only some destinations. Unchanged paths still need the
existing journaled metadata commit under the same bounded Apply result model.

## Consequences

- Lists, caches, visible rows, and active local-player bindings observe a
  combined publication as one logical path-and-metadata update.
- The database transaction remains short: artifact writing and rereading occur
  before it on operation workers.
- Startup recovery can finish a published artifact without trusting stale UI
  state or inventing metadata from a journal payload.
- Partial batch failure is explicit. Successful sources stay committed; failed
  or cancelled sources require a fresh review before retry.
- The schema records whether idempotent relocation evidence includes metadata
  refresh, preventing an older path-only replay from silently satisfying a
  stronger combined request.

## Validation

- Repository tests prove atomic all-occurrence relocation plus metadata refresh,
  logical-overlay preservation, stale-snapshot convergence, replay, and
  path-only/combined operation-id mismatch rejection across schema 22.
- A real rich native-FLAC fixture applies a ready combined preparation plan
  through the bounded job, publishes directly at its reviewed destination, and
  proves the persisted path, revision, and reread metadata.
- Offscreen Trackbench tests prove that a combined review enables Apply and
  hands the immutable plan to the application service.
- Startup recovery tests begin at a durably published artifact, reread the real
  target, atomically reconcile path and metadata, remove the exact source, and
  expose complete non-undoable history.

## Revisit when

- artwork or ReplayGain joins the same artifact preparation pipeline;
- another exact format writer is qualified;
- changed-artifact undo receives a retained-original policy;
- artifact scheduling needs explicit disk-space admission or resource classes.
