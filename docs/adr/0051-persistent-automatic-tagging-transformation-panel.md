# ADR-0051: Persistent automatic tagging transformation panel

- Status: accepted
- Date: 2026-08-30
- Owners: Trackknife project
- Extends: ADR-0048 versioned previewed metadata transformation chains and
  ADR-0049 persisted exact-value metadata transformation chains

## Context

Saved transformation chains were reusable only from inside the transformation
editor: open the editor, choose a definition, preview it, and stage its result.
That is appropriate for occasional work but makes routine cleanup chains too
hidden and repetitive. Presenting one saved-chain selector plus a separate
automatic-chain menu also duplicates the same catalog and obscures which state
will affect the next tag write.

The useful part of MusicBrainz Picard's scripting workflow is that enabled
cleanup definitions participate consistently in tag saving. Trackbench must
offer that immediacy without persisting or executing host-language scripts and
without bypassing its complete preview and explicit Apply boundary.

## Decision

- Properties presents one **Tagging scripts** side panel containing every saved
  declarative transformation chain. “Script” is presentation language only;
  persisted data remains the versioned typed schema from ADRs 0048–0050.
- Every row has one checkbox. Its state is stored with the saved chain and is
  shared by later Properties sessions. Reversible SQLite migration 11 adds the
  constrained `automatic` flag, defaulting existing definitions to disabled.
- One panel button opens the transformation editor. If a row is selected, that
  saved definition is selected in the editor; with an empty catalog the same
  button opens the editor for creation. Double-clicking a row is equivalent.
- Checked definitions are evaluated in the exact order displayed by the panel:
  exact chain name followed by stable ID as a deterministic tie-breaker. Their
  actions form one bounded chain and later checked definitions see the results
  of earlier ones. The existing 256-action chain limit applies to the combined
  automatic pass.
- The automatic pass targets the logical items already participating in the
  staged tag draft. It evaluates after manual draft edits and immediately
  before fresh physical-source revalidation.
- Automatic results are applied only to a temporary copy of the draft and its
  session field vocabulary. They appear in the final immutable write-plan
  dialog but do not silently enter the visible draft or its undo history.
- Closing and rebuilding the write preview repeats the operation from the same
  manual draft. Non-idempotent actions such as exact append therefore cannot
  compound merely because Preview was opened more than once.
- Checked transformations never write on their own. All exact manual and
  automatic results still require a wholly ready write plan and an explicit
  Apply through ADR-0047's journaled operation path.

## Alternatives considered

### A saved-chain combo plus a separate automatic menu

Rejected. The same catalog appears twice with different selection semantics,
and enabled state is hidden until another menu is opened.

### Stage automatic results into the visible draft

Rejected. It makes automatic policy look like a manual edit, pollutes undo
history, and lets append/split/self-referential formatting compound after
repeated write previews.

### Execute checked definitions after confirmation but outside the write plan

Rejected. The user would confirm a plan different from the metadata actually
written, violating the complete-preview requirement.

### Store executable Picard-compatible scripts

Rejected. Trackbench owns a typed, deterministic declarative transformation
schema and `tkfmt-1`; Picard scripting is not a compatibility target.

## Consequences

- Routine cleanup policy is visible directly beside the tagging workspace and
  requires no editor round trip for each operation.
- Checkbox changes are persistent product state rather than a property of one
  open dialog.
- Automatic changes remain exact, previewable, cancellable, and subject to the
  same conflict/capability checks as manual edits.
- Cross-chain order is deterministic but not yet user-reorderable. A later UI
  may persist explicit order without changing existing chain semantics.
- Definitions checked for automatic use still remain editable and usable for
  an explicit one-off transformation preview.

## Validation

- Repository coverage migrates to schema 11 and round-trips the automatic flag
  with the complete saved-chain payload.
- Offscreen Properties coverage creates a saved chain, observes it in the side
  panel, persists its checkbox, opens the selected chain in the editor, and
  verifies its exact results in the final write plan.
- The same UI coverage opens the write plan twice and verifies that automatic
  results never enter or multiply the visible draft.

## Revisit when

- users need drag-reordering or independently named automatic policy sets;
- a script-only operation with no manual tag draft is specified;
- automatic scope needs to include items outside the staged tag operation.
