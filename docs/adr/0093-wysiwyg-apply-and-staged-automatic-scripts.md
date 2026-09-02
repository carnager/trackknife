# ADR-0093: WYSIWYG apply and staged automatic scripts

- Status: accepted
- Date: 2026-09-02
- Owners: Trackknife project
- Extends: ADR-0083 direct apply with trusted live preview; informed by
  Picard's script-execution model

## Context

Automatic transformation chains ran invisibly inside the write-plan
worker at Apply time: the grid showed one thing and the file received
another. That hid the ADR-0087-addendum failure from view (the user's
chain stripped totals nobody could see being stripped) and made Suggest
fight the user's own chains (it proposed the very fields a chain
deletes). Picard's model, verified from its source, is the opposite:
tagging scripts run once when new metadata arrives
(`Album._finalize_loading_album`), the UI shows the post-script result,
and save writes exactly what is displayed — but Picard only ever runs
scripts on MusicBrainz results. The user wants the no-surprises half
without that limitation.

## Decision

### Apply writes exactly what the grid shows

- The write plan is built from the staged draft alone. The apply-time
  combined-chain composition is gone; the Apply button enables on staged
  drafts (or path operations), never on the mere existence of automatic
  chains.

### Automatic scripts stage visibly, and not only for MusicBrainz

- Checked ("automatic") scripts stage their edits into the grid as one
  ordinary colored, undoable draft transaction at three moments: when
  the Properties grid finishes loading over the local baseline — the
  Trackbench extension beyond Picard, so plain local files get the same
  treatment as provider results; after any provider staging (Suggest and
  the ADR-0090 Identify flow both land in the same completion); and
  immediately when a script's automatic checkbox is turned on.
- Staging is computed by the same `plan_metadata_transformation`
  preview the script editor uses, off the UI thread, and re-running is
  idempotent — a chain whose effect is already staged produces an empty
  preview and stages nothing.
- Turning a script's checkbox off un-stages nothing by itself; the
  staged transaction remains user-owned and undoable like any edit.

## Consequences

- The grid is the write: every chain effect is reviewable as colors
  before Apply, and rejecting one is a single undo.
- Suggest and automatic chains stop fighting: the chain's removals are
  part of the draft Suggest inspects, so it no longer refills fields the
  user's scripts delete.
- Scripts-panel copy now describes staging ("stage their edits as
  colored drafts when files load and when suggestions arrive") instead
  of apply-time execution.
