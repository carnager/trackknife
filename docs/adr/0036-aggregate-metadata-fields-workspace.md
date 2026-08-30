# ADR-0036: Aggregate metadata fields as the primary bulk workspace

- Status: accepted
- Date: 2026-08-29
- Owners: Trackknife project
- Revises: ADR-0034 primary-surface choice
- Complements: ADR-0035 bounded metadata drafts
- Superseded in layout by: ADR-0038 file-selection-driven metadata Properties
- Draft fallback superseded by: ADR-0041 background metadata Draft projection

## Context

ADR-0034 chose a track-by-field matrix as the primary Properties surface
because it keeps occurrence alignment visible. Hands-on use showed that this
orientation makes the common operation—reviewing and changing one tag across
the complete selection—needlessly wide. Picard's compact field-oriented
Original/Draft presentation is a better default for bulk tagging, while the
matrix remains valuable for resolving individual differences.

The two presentations must not own separate edits. A bulk field edit and a
per-track cell edit are different projections of one staged selection and one
undo history.

## Decision

- Trackbench Properties opens on a virtualized `Fields` table with one row per
  union field and three columns: Field, Original, and Draft.
- Original displays an exact value only for a common field. Mixed, partial,
  and missing states remain explicit; they are never collapsed to an arbitrary
  representative value.
- Editing Draft stages one scalar replacement for every selected occurrence.
  Delete stages explicit removal across the selected field rows, and
  `Ctrl+Backspace` reverts those rows. All actions use ADR-0035's existing
  sparse patch set and bounded undo/redo history.
- The existing track-by-field matrix moves to the adjacent `Tracks` drill-down.
  It retains exact occurrence alignment, individual cell editing, and the
  Original/Draft value inspector. Both views update immediately from the same
  draft model.
- A field summary retains the first present occurrence index so a common
  Original value can be displayed in constant time without copying it into
  every aggregate row or scanning the selection on the UI thread.
- A uniform bulk edit may show its exact Draft value immediately. ADR-0041
  projects later per-track exceptions in the background, after which the row
  reports the exact resulting common, mixed, missing, or partial state.
- A single bulk transaction addresses at most 100,000 cells, matching the
  draft-cell bound. The aggregate model remains a `QAbstractTableModel` with
  no persistent per-cell widgets or content-scanning geometry.
- This remains an in-memory draft surface. It adds no Apply action, metadata
  writer, MusicBrainz network action, or mutation-script compatibility claim.

## Alternatives considered

### Keep the track matrix primary

Rejected after hands-on feedback. It is excellent evidence and alignment, but
poorly shaped for reviewing conventional fields and applying one value to a
large selection.

### Replace the matrix with only the aggregate table

Rejected. Mixed and partial summaries cannot show which occurrence owns each
value.

### Recompute every aggregate Draft value synchronously

Rejected. A field can span 100,000 selected occurrences. ADR-0041 uses a
bounded, cancellable background projection and rejects stale generations.

## Consequences

- Conventional bulk edits are compact and require no horizontal traversal.
- Users can move to Tracks only when they need per-occurrence evidence or
  exceptions.
- Undo, discard, limits, and future preview/write planning remain singular;
  there is no synchronization protocol between independent drafts.
- The exact ordered multi-value editor can attach to Draft in Fields and to an
  individual cell in Tracks without changing the domain patch type.

## Validation

- The real-FLAC offscreen Properties regression verifies that Fields is the
  default page, exposes Field/Original/Draft columns, distinguishes mixed,
  partial, and missing Original values, stages a bulk Draft value into both
  track cells, and undoes it through the shared history.
- Qt-free selection coverage verifies constant-time representative occurrence
  identity for present fields and no representative for missing fields.

## Revisit when

- a complete revalidated write plan needs additional aggregate evidence.
