# ADR-0052: Tabbed tagging workspace and expandable change preview

- Status: accepted
- Date: 2026-08-30
- Owners: Trackknife project
- Extends: ADR-0038 file-selection-driven metadata Properties and ADR-0048
  versioned previewed metadata transformation chains

## Context

Metadata Properties grew from a small inspector into Trackbench's primary tag
workspace: file scoping, exact values, arbitrary fields, saved transformations,
complete write plans, and Apply all live there. Keeping that surface in a
separately sized non-modal window makes users repeatedly arrange it beside the
main application and gives a long-lived workflow the visual weight of a
temporary dialog.

The transformation editor's exact preview also devoted five columns to File,
Step, Field, Before, and Final. File and producing-step information is valuable
for diagnosis, but showing it on every row makes the ordinary old-to-new review
harder to scan.

## Decision

- Invoking **Properties** creates a temporary special tab in Trackbench's
  existing Track Lists tab surface. Its title is `Tags · N tracks`; it is not a
  persisted list document and is not restored as workspace content.
- The tab hosts the existing Properties implementation and all of its bounded
  models, background jobs, and exact draft state. The move changes presentation
  and lifetime ownership, not the metadata operation contracts.
- A tagging tab is closable from its tab button, the Workspace close command,
  or its existing Close button. Every path invokes the same Properties close
  boundary: an uncommitted draft requires discard confirmation, a running Apply
  requests safe cancellation, and a committed result may close directly.
- Main-window shutdown first asks every open tagging tab to close. Cancelling a
  draft-discard prompt or waiting for an in-flight Apply cancels shutdown rather
  than silently destroying staged work.
- Tagging tabs may coexist with list tabs and are skipped by list persistence,
  list-document ordering, list actions, and track-view layout capture. Closing
  the final real list still creates the normal Scratch list even if a tagging
  tab was open.
- The transformation preview becomes a three-column expandable tree:
  **Field**, **Old**, and **New**. Each exact changed cell is one top-level row.
  Expanding it reveals the affected file label and the last chain step that
  produced the final value.
- Exact missing, empty, multi-value, and removed states retain their existing
  lossless display. Expansion changes information hierarchy only; it does not
  aggregate cells or change staging semantics.
- Child editors, the immutable physical write-plan review, and Apply progress
  remain window-modal dialogs for now because each represents a focused
  confirmation or structured subtask rather than the primary workspace.

## Alternatives considered

### Persist the separate Properties window geometry

Rejected as the primary answer. Geometry persistence reduces repeated resizing
but still treats the main preparation workflow as a detached utility window.

### Replace the source list tab in place

Rejected. A distinct temporary tab makes the captured selection and unsaved
draft visible, lets users return to the source list, and gives close protection
an explicit surface.

### Remove file and step data from transformation previews

Rejected. Those details explain duplicate-looking changes and chain ordering;
they belong behind expansion rather than disappearing.

### Show every detail as a permanent column

Rejected. That is the previous layout and optimizes for diagnosis at the cost
of the common old-to-new scan.

## Consequences

- Tagging uses the already-sized main workspace and no longer needs recurring
  top-level Properties window arrangement.
- Temporary tagging work is visible in the same tab vocabulary as other
  long-lived Trackbench tasks without becoming a saved list.
- Users can review compact transformations quickly and expand only ambiguous
  lines.
- Multiple tagging tabs can exist for independent captured selections. Their
  drafts remain isolated and are intentionally not restored after restart.

## Validation

- Offscreen main-window coverage verifies Properties is the current second tab,
  is not a top-level window, carries an informative temporary title, and
  disappears through the tab-close signal.
- The real-FLAC Apply test verifies successful Apply closes the tagging tab and
  returns to the source list.
- Transformation UI coverage verifies the three compact headers, old/new
  values, expandable detail child, affected-file label, and producing step.

## Revisit when

- focused write-plan or Apply review is redesigned as another task tab;
- tagging-tab recovery across process restart has a journal-backed draft model;
- user testing shows that multiple simultaneous tagging tabs should collapse to
  one replaceable workspace.
