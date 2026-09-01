# ADR-0084: Silent recovery and draft color semantics

- Status: accepted
- Date: 2026-09-01
- Owners: Trackknife project
- Extends: ADR-0083 direct apply
- Amends: ADR-0046 recovery/undo presentation and ADR-0067 unified operation
  history (their journals, recovery state machines, and retention machinery
  are unchanged; their user-facing surfaces are removed)

## Context

The `Preparation operations` window exposed the metadata undo-backup journal
and file-publication history directly: raw journal UUIDs, byte counts, and
states like "Backup released" / "Needs attention", with Undo/Delete buttons.
In the real tag-then-rename workflow every backup was invalidated moments
after it was created (any later write or relocation fails the strict
revision check), so the buttons were permanently disabled and the window —
which also opened itself at startup — presented an incomprehensible pile of
dead records. No comparable tagger has such a surface. Separately, with the
ADR-0083 review dialog gone, the tag grid itself needed to carry the "what
will change" signal.

## Decision

### Recovery is silent; only unresolvable incidents surface, once

- The history window, its menu action, the undo/release/file-undo job kinds,
  and the after-apply snapshot reloads are removed. Journals, automatic
  startup recovery for interrupted tag writes and file publications, and the
  reconciliation-count window properties remain.
- Records recovery can neither finish nor safely roll back (metadata journal
  records and file publications in `needs_reconciliation`) are shown in the
  compact ADR-0083 feedback window — file plus one plain-language reason —
  exactly once: presented journal ids are remembered in settings (pruned to
  ids still present) so known incidents never reopen the window.
- Successful recovery reports only a brief status-bar message.

### Undo backups are no longer retained across restarts

- The commit protocol still creates its verified backup as part of atomic
  publication, but the startup retention policy now keeps zero entries, so
  full-file undo copies (up to 2 GiB) stop accumulating for an undo feature
  that no longer exists. Same-filesystem move undo loses its only entry
  point and is intentionally gone with it.

### The grid shows Picard-style draft semantics

- Staged state is colored by meaning in both the per-file grid and the
  Field/Original/Draft table: green text = field added, orange text =
  values changed, red struck-through text = field removed. Only the changed
  content is colored — no background tint, no row-wide painting — with
  medium-strength hues that read on light and dark palettes.

### Fixed alongside: artwork apply progress race

- `apply_artwork_write_plan` incremented its completed count outside the
  progress mutex, so concurrent workers could deliver counts out of order
  and the final update could carry a stale total. Count increment and
  delivery are now one serialized step, matching the ADR-0063 executor's
  contract.

## Alternatives considered

### Keep the history window but only show actionable rows

Rejected. In the dominant workflow no row is ever actionable, so the window
would be permanently empty while still costing the journal-vocabulary UI.

### Make undo survive renames and re-saves

Rejected for now. Correct undo across relocations and successive writes
means chaining backup identity through the file-publication journal; the
demand signal is zero while drafts, direct apply, and journaled crash
safety already cover the real risks.

## Consequences

- No user-facing undo of committed tag writes or file moves. The journals
  retain the evidence, so a future undo surface can be rebuilt if wanted.
- Stale `needs_reconciliation` records linger invisibly in the journal;
  they are reported once and then only occupy database rows.
- The grid is now the single trust surface for tag changes, matching the
  naming-layout preview for paths (ADR-0083).

## Validation

- Offscreen tests prove: an unresolvable interrupted tag write and an
  unresolvable interrupted rename each surface once in the compact feedback
  window with their reason and are not re-presented by a second window; a
  fully recovered combined publication updates the list and shows no window
  at all.
- The artwork progress race no longer reproduces under 16 concurrent runs
  of the metadata-commit suite.

## Revisit when

- users ask for undo of committed operations (rebuild on the retained
  journal evidence, with relocation-aware backup identity);
- provider-driven edits (M6) raise the stakes of a wrong batch write.
