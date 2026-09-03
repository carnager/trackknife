# ADR-0104: Grouped numbering transformation

- Status: accepted
- Date: 2026-09-03
- Owners: Trackknife project
- Extends: ADR-0049–0053 transformation chains, ADR-0064 numbering,
  ADR-0072 native interchange

## Context

Numbering by selected-file order runs one counter across the whole
selection; renumbering a multi-album or multi-disc selection required
manual per-group selections. "Grouped numbering" was the last named
open M5 decision.

## Decision

- `MetadataNumberGroupedItemsAction` numbers selected items in selection
  order with an independent counter per evaluated `tkfmt-1` group value:
  `%album%` numbers every album 1..N even when the albums interleave in
  the selection, `%album%|%discnumber%` restarts per disc. An empty
  evaluation shares one counter; start/padding behave exactly like
  plain numbering. The group expression compiles at prepare time and a
  broken expression fails typed before any item is touched.
- Persistence stores it as action kind 17 (expression + dialect +
  start + padding); migration 26 rebuilds the transformation-action
  tables to widen the kind constraint, with the down migration removing
  kind-17 rows before narrowing. The native JSON interchange gains the
  `number_grouped_items` action with exact-key validation.
- The script editor offers "Number within each group" beside the plain
  numbering kind, reusing the start/padding spin boxes with a Group by
  expression input.

## Consequences

- One script step renumbers an entire mixed selection per album or per
  disc — composable with capture, formatting, and the automatic-staging
  contract like every other action.
- Tests pin interleaved-group semantics (independent counters, the
  empty group sharing one), typed failure for broken expressions,
  persistence round-trip through restart at schema 26, and exact JSON
  interchange round-trip.
