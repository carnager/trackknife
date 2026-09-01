# ADR-0083: Direct apply with trusted live preview

- Status: accepted
- Date: 2026-09-01
- Owners: Trackknife project
- Amends: ADR-0042 revalidated write-plan preview, ADR-0047 explicit
  metadata Apply, ADR-0063 bounded file-publication Apply, and ADR-0067
  immutable preparation review presentation

## Context

Every Properties mutation ran through a routine modal review dialog
(`Review changes`) followed by a second modal progress/result dialog, stacked
over the tab-hosted Properties workspace and the non-modal naming-layout and
destination managers. Live use showed the chain works against the operator
instead of for them: the review restates edits the user just made by hand,
the windows cover each other, the fixed-size tables need constant resizing to
find the one important cell, and the prose leans on internal planning
vocabulary ("physical sources", "preparation publication").

The safety chain underneath — fresh revalidation, pure path planning, live
preflight, journaled bounded Apply, recovery — never depended on the dialog.
The dialog only displayed an already-computed immutable plan and offered the
Apply button. The user's trust question is not "which tags will change?"
(they typed them) but "will the naming expression produce the paths I
expect?", which a one-line example answered poorly.

## Decision

### Ready plans apply immediately

- The Properties footer button becomes **Apply**. It still snapshots the
  draft, runs checked automatic chains, revalidates every source, plans pure
  output paths, and preflights the filesystem — unchanged, off the UI thread.
- A plan in which every enabled effect is ready proceeds straight into the
  existing bounded Apply job. No review dialog is shown. Every attempt still
  consumes its plan; retry replans freshly.
- A blocked plan changes nothing and opens one compact **Apply blocked**
  window listing only the offending files with one short problem sentence
  each (kind-qualified, full text in tooltips). There is no routine dialog on
  the success path.

### Progress lives in the workspace footer

- During Apply the footer shows a progress bar (`n of N`), a short status
  ("Saving tags · 2 of 12", "Updating files · 1 of 3"), and a **Stop** button
  bound to the existing cooperative cancellation source. The modal
  metadata-apply and file-publication-apply progress dialogs are removed.
- A fully committed (or committed-plus-unchanged) run closes the Properties
  tab as before, with no result dialog. Failures and stops open the same
  compact feedback window listing only the files that were not committed;
  committed work is reported in one summary line.
- Closing Properties during a run still requests cancellation first; the
  in-flight-file safety contract is unchanged.

### The naming layout editor is the trust surface

- The naming-layout manager becomes a resizable workspace (fixed-font
  expression fields, form-grown inputs) whose lower half is a live preview
  table: current file name → resulting relative path for the selected tracks
  (all tracks when none are selected, bounded at 100 with an explicit
  "first N of M" status).
- The preview reuses the debounced bounded planner from ADR-0055 across the
  whole item set, honors the Save tags toggle for draft visibility, reports
  expression errors inline, and refreshes on expression edits, selection
  changes, and manager reopen.

## Alternatives considered

### Keep the review dialog behind an option

Rejected for now. The review restated user-authored edits; the blocked and
failed cases — the only ones where review content changed decisions — are
still fully surfaced. An opt-in inspection surface can return later without
changing the apply contract.

### Morph the review dialog into the progress view in place

Rejected. It removes the stacking but keeps a routine modal window whose
content the user has already seen in the grid and the naming preview.

## Consequences

- One click applies; dialogs appear only when something needs attention.
- The plan/preflight/journal/recovery chain, observers, history, and
  reconciliation surfaces are untouched; this is presentation-boundary only.
- `preparation_plan_dialog`, `metadata_apply_dialog`, and
  `file_publication_apply_dialog` are deleted; one `preparation_feedback`
  window serves blocked plans, stops, and failures. The apply progress state
  structs move to the Properties dialog boundary.
- Artwork keeps its ADR-0080 review/apply dialogs; aligning it with this
  contract is follow-up work.
- The exact per-field plan table (old/new values per intent) is no longer
  visible anywhere; if provider-driven edits (M6) reintroduce values the user
  did not type, an inspection surface must return with them.

## Validation

- Offscreen Trackbench tests prove: a ready tag plan applies directly and
  closes Properties with no feedback window; Stop during a three-source run
  surfaces a compact `Save stopped` list of untouched files and preserves the
  draft for fresh replanning; a blocked FLAC mapping opens `Apply blocked`
  with the kind-qualified reason and applies nothing; path-only and combined
  tag-and-rename plans reach the applier with the same reviewed targets as
  before, without any review dialog.
- The naming-layout preview test proves the live table lists each selected
  track's resulting path, reports expression errors inline, and recovers.

## Revisit when

- provider proposals (M6) stage values the operator did not author;
- artwork review/apply adopts the same direct contract;
- batch scale makes a pre-apply inspection surface worth reintroducing.
