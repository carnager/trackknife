# ADR-0027: Versioned track-view layouts

- Status: accepted
- Date: 2026-08-29
- Owners: Trackknife project
- Complements: ADR-0026 panel composition

## Context

Panel composition alone does not capture the workstation flexibility valued in
foobar2000 and Columns UI. The queue/list surface is itself a composed visual:
album groups can own a header and artwork region while each occurrence remains
in independently arranged metadata columns. Trackbench already had album
headers and small covers, but its physical column contract was fixed and its
layout was not persisted.

This is an independently specified Trackknife facility. It is not compatible
with Columns UI configuration files, title-format dialects, components, or
plugin APIs.

## Decision

- A track-view layout is versioned declarative data bound to one queue/list.
  Version 1 stores the presentation preset plus the complete visual order,
  visibility, and width of stable semantic column IDs.
- Trackbench ships four recoverable presentations: albums with side artwork,
  albums with header artwork, plain columns, and a compact queue. The default
  is albums with side artwork.
- The default grouped geometry uses a full-width album label and duration
  summary. A dedicated artwork column paints one cover through the visible
  member-row region while Artist, track number, Title, Album, Date, and Length
  remain independently movable, resizable, and hideable.
- Visible columns fill the viewport. Artwork, number, date, and duration retain
  compact preferred widths; Artist, Title, and Album divide the remaining
  space proportionally and reflow when the window or panel changes size. A
  genuinely narrow viewport respects per-column minimums and may scroll.
- Track cells and group labels are always single-line. Long values elide at the
  right and never increase row or group height.
- Side artwork is a bounded visible-region overlay, not a `QTableView` row
  span. Underlying occurrence rows keep normal selection, activation,
  multi-selection drag/drop, and insertion geometry.
- Header context actions and the Workspace menu select a shipped presentation,
  toggle columns, reset the current view, or copy its layout to all open lists.
  Direct header moves and resizes are part of the same persisted definition.
- Every registered column appears exactly once and at least one is visible.
  Persisted definitions are bounded and strictly validate schema,
  presentation, IDs, widths, uniqueness, and completeness before application.
- Invalid or newer definitions visibly fall back to the shipped default without
  overwriting the original bytes. An explicit user edit or reset adopts the
  current schema.
- Version 1 deliberately does not pretend to implement the complete
  `TrackViewDefinition` from `docs/ui-workspace.md`. User-defined display,
  independent sort, group, label, and summary expressions will extend this
  model using versioned `tkfmt-1` source and dialect metadata.

## Alternatives considered

### Persist only `QHeaderView::saveState()`

Rejected for Trackbench. The binary state has no semantic presentation mode or
stable column identifiers and cannot safely represent later formatting and
group definitions. The existing SQLite preset payload remains an opaque
transport slot, but Trackbench stores validated versioned JSON inside it.

### Use row spans for album artwork

Rejected. Spans make a large artwork cell replace ordinary row hit targets and
complicate precise drops, scrolling, and selection. Painting only the visible
group region preserves the model's one-row-per-occurrence behavior.

### Implement arbitrary formatting expressions immediately

Rejected for this slice. The persisted layout and source-independent renderer
boundary must be proven before exposing an editor whose definitions become
long-lived user data. Hard-coded pseudo-expressions would also violate the
project-owned `tkfmt-1` requirement.

## Consequences

- Trackbench's local model exposes artwork/status separately from Artist and
  track number so the grouped layout does not sacrifice useful columns.
- Presentation changes are view state, not list-content mutations; they do not
  mark a saved list dirty.
- The shared grouped delegate accepts explicit semantic column mappings while
  preserving the MPD client's existing six-column projection.
- A later shared view-definition service can replace the built-in column set
  without changing queue/list occurrence semantics or the outer panel tree.

## Validation

- Unit coverage round-trips all v1 fields and rejects malformed, newer,
  incomplete, duplicate, unknown, fractional/out-of-range, and all-hidden
  definitions.
- An offscreen Trackbench regression switches presentation, verifies the side
  artwork column and independent metadata columns, changes order/width/
  visibility, verifies viewport fill at multiple window sizes plus single-line
  elision, restores state across restart, and proves future bytes survive a
  fallback shutdown.
- Offscreen visual inspection uses multi-track album fixtures with embedded
  covers to verify the full-width header and side-art geometry.

## Revisit when

- the first `tkfmt-1` column/group editor is ready;
- one reusable named layout should bind several lists by reference rather than
  copying its current definition;
- collapsing, nested disc groups, or artwork-size policies join the model.
