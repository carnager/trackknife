# ADR-0073: Journaled destination-artifact publication

- Status: accepted
- Date: 2026-08-31
- Owners: Trackknife project
- Extends: ADR-0054 unified preparation plans, ADR-0056 durable file
  publication, ADR-0061 verified prepared-copy moves, and ADR-0067 immutable
  preparation review

## Context

Trackbench can either publish a byte-preserving path change or replace native
FLAC metadata at the existing source path. It cannot yet execute a reviewed tag
change together with Rename or Move. Writing the source and subsequently
moving it would expose two independently recoverable mutations and violate
ADR-0054's requirement to prepare one final verified file directly at the
destination.

The file-publication state machine already has the necessary durable physical
boundaries—planned, target prepared, target published, dependent state
committed, source removed, and complete—but its prepared lifecycle assumes an
exact byte copy. A changed metadata artifact must deliberately differ from its
source and therefore cannot use byte equality as recovery evidence.

## Decision

- File-publication evidence separates path topology from content intent:
  `publication_kind` remains same-filesystem rename or cross-filesystem copy,
  while `content_kind` is either `preserve_source_bytes` or
  `prepared_destination_artifact`.
- Reversible schema migration 21 adds `content_kind` with a fail-closed numeric
  check and defaults existing records to byte preservation. A changed artifact
  uses the prepared-target/source-removal lifecycle on both filesystem
  topologies; a same-filesystem artifact does not take the direct atomic-rename
  shortcut.
- One generic Qt-free executor owns the complete changed-artifact publication:

  1. revalidate and lock the exact single-link source;
  2. durably create the journal before directories or files;
  3. ask a format-specific callback to create and fully verify the exclusive
     executor-named destination sibling;
  4. lock the artifact, preserve ownership, mode, and bounded extended
     attributes, sync it and its parent, and revalidate both identities;
  5. record `target_prepared`, publish without replacement, and record
     `target_published`;
  6. invoke the idempotent all-or-nothing dependent-state callback while both
     original and destination exist;
  7. record dependent state, remove only the still-locked exact original, and
     complete through `source_removed`.

- The preparer must leave no path on failure and return the artifact's exact
  final revision on success. Unexpected debris remains reconciliation
  evidence. Publication never opens the source for writing.
- Recovery adopts an unrecorded prepared sibling only for byte-preserving copy
  records, where exact source equality proves intent. An unrecorded changed
  artifact at `planned` is ambiguous and is retained for reconciliation.
  Once `target_prepared` is durable, recovery uses the recorded artifact
  revision and topology rather than impossible source-byte equality, replays
  dependent state when required, and removes only the recorded source.
- `FilePublicationCommitResult` carries the content kind so recovery and future
  dependent-state composition can distinguish a simple relocation from a
  metadata-changing relocation.
- The preservation-proven `taglib-flac-v1` prepared-copy writer is exercised as
  the first real format preparer. It produces its verified native FLAC directly
  at the executor-owned destination sibling; the source is not rewritten.

This decision qualifies the journal and single-source executor boundary. It
does not yet enable changed tags plus Rename/Move in Properties. That requires
the bounded Apply job to pair each final metadata source plan with its path
source and a single durable dependent transaction that both relocates every
occurrence and refreshes the published metadata document. Startup recovery
must use that same composed callback before the UI blocker can be removed.

## Alternatives considered

### Commit metadata at the source and run a normal move

Rejected. Two journals could disagree after a crash, and the source path would
briefly contain content that the reviewed destination operation had not yet
published.

### Treat changed content as an ordinary cross-filesystem copy

Rejected. Exact-copy recovery would correctly reject the intended metadata
change, while weakening byte verification for every existing copy would erase
valuable evidence.

### Trust any executor-named file found after a crash

Rejected. Before the prepared revision is durable, Trackbench cannot prove
that a changed file is the successfully verified output of the requested
format writer. Ambiguous evidence is safer than inferred deletion or
publication.

### Add a separate metadata-at-destination journal

Rejected. The file-publication journal already owns target publication,
dependent state, and source removal. A second owner would recreate the split
transaction this decision is intended to avoid.

## Consequences

- Changed native FLAC content can now be safely prepared and published at a
  different destination under one recovery owner.
- Same-filesystem changed content performs extra I/O because the original must
  coexist until dependent state commits; this is required for rollback and
  crash recovery.
- Changed-artifact publication has no current undo surface. The exact original
  is removed only after dependent state, but no retained backup is created.
- The executor is format-agnostic, while each advertised writer remains
  responsible for semantic reread and unknown-data/audio preservation proof.
- The existing path-only UI remains honest until composed batch and persistence
  integration is complete.

## Validation

- Schema tests persist and traverse every prepared-artifact state on a
  same-filesystem topology and prove content intent survives restart.
- Executor tests inject a failure after dependent state, then recover the
  changed target without source-byte comparison and remove the original only
  after callback replay.
- Recovery retains an unrecorded changed prepared sibling for manual
  reconciliation rather than adopting or deleting it.
- A repository-owned rich native-FLAC fixture changes `TITLE` through the
  preservation-verified writer directly at a changed destination, rereads the
  exact prepared document after publication, and completes with no source or
  prepared sibling left behind.

## Revisit when

- the composed metadata-and-relocation dependent transaction is implemented;
- bounded multi-source preparation needs artifact-specific scheduling or disk
  space admission;
- changed-content undo receives a retained-original policy;
- another format-specific writer qualifies this executor.
