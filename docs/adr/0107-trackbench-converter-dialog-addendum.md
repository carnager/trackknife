# ADR-0107 addendum: converter reachability and saved profiles

- Status: accepted
- Date: 2026-09-03
- Extends: ADR-0107 converter dialog, ADR-0101 workspace layout

## Decisions

- "Convert files…" joins the local track context menu beside Properties,
  sharing the same selection-gated action, so conversion starts straight
  from any queue or list row.
- The hamburger application-menu button is removed; the regular menu bar
  is visible again. The transport header keeps only playback controls.
- The converter offers the app's saved naming layouts and destination
  roots (the ADR-0069-family profiles the Properties dialog manages) as
  one-click choices above the expression fields. Picking one fills the
  editable fields; the first hand-edited keystroke drifts the choice
  back to Custom, so the fields remain the single source of truth for
  the plan.
