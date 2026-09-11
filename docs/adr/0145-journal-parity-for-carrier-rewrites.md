# ADR-0145: Journal parity for CUE and sidecar rewrites

Date: 2026-09-11

Status: accepted

## Context

ADR-0139, 0141, and 0143 introduced two mutation paths outside the
ADR-0059 operation journal: CUE-sheet ReplayGain rewrites and loudness-
sidecar merges. Both publish atomically (prepared temp + rename), so
they cannot tear a file — but they leave prepared-copy debris after a
crash, retain no byte pre-image, cannot be undone, and never surface in
startup recovery or reconciliation. All three ADRs tracked this as one
shared follow-up. The journal's persisted `content_kind` was also
capped at text/artwork by a SQLite CHECK.

Inspection showed the journal machinery is almost entirely
kind-agnostic: `publish_prepared_metadata_copy` (flock, hard-link
backup, rename, fsync discipline, state transitions, rollback, backup
retention), crash recovery's rollback/roll-forward classification, and
the RENAME_EXCHANGE undo need only per-kind *content verification* and
*evidence* to work for any single-file replace.

## Decision

### Scope: journaled replaces

Both carriers join the journal for the **replace** shape — the carrier
file exists before and after:

- CUE rewrites are always replaces (a sheet always exists).
- Sidecar merges over an existing sidecar are replaces. A merge that
  empties the sidecar now publishes an **empty-entries document**
  instead of unlinking, so even "removal" is a journaled, undoable
  replace; ADR-0141's delete-on-empty behavior is superseded.
- Sidecar **creation** (no prior file) stays a direct atomic publish:
  there is no pre-image to protect, no backup to retain, and no
  ambiguous crash state beyond an inert temp file. Recovery and undo
  do not apply to it by construction.

### Evidence and schema

Two new content kinds, `cue_replay_gain` and `loudness_sidecar`, share
the text kind's evidence shape: `operation_journal_changes` rows with
the carrier-internal identity encoded in `exact_native_name`
(`cue-album`, `cue-track:<file>:<track>`,
`entry:<stream>:<subsong>:<start>:<end>` with `-` for absent), the REM
name in `property_name`, canonical name, patch kind, planned values,
and originals captured from the parsed pre-image at commit time —
including stale sidecar entries, because undo restores the byte
pre-image and verification must match what that file actually says.
Schema version 29 rebuilds `operation_journal` to widen the
`content_kind` CHECK (rename → create → copy → drop on the
foreign-keys-off migration connection, index recreated).

### Lifecycle

The journaled commits follow `commit_flac_metadata_source` exactly:
process lock and flock on the carrier, single-link verification,
fresh parse with original-evidence capture, journal record, prepared
copy at the journal's sibling path, then the shared publish sequence.
Per-kind branches exist only in content verification: published
content re-parses the carrier and compares planned evidence; original
content (undo, recovery) compares original evidence. Both return an
empty `MetadataDocument` — `MetadataCommitResult` gains the record's
`content_kind` so dependent-state consumers dispatch: the library
refresh and list-model document replacement apply only to text
commits; carrier commits refresh rows through their existing
per-carrier paths.

`apply_metadata_write_plan` takes explicit committers for sheets and
sidecars, so the caller owns journal and dependent-state wiring for
every kind, and startup recovery picks the new kinds up with no
filtering — completing interrupted publications, rolling back
prepublication debris, and reporting ambiguity as reconciliation
exactly as for tags.

## Consequences

- Carrier rewrites now share the tag pipeline's guarantees: crash
  debris is swept, interrupted publications complete or roll back,
  byte pre-images are retained and undoable (RENAME_EXCHANGE
  filesystems), and ambiguity becomes visible reconciliation instead
  of silence.
- Emptied sidecars remain on disk as empty documents; cleaning them up
  is a maintenance concern, not a mutation, and may ride backup
  maintenance later.
- Sidecar creation remains outside the journal by explicit argument
  (nothing at risk), keeping the novel-machinery surface at zero.
