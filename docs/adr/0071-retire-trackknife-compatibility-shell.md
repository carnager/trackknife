# ADR-0071: Retire the Trackknife compatibility shell

- Status: accepted
- Date: 2026-08-30
- Owners: Trackknife project
- Extends: ADR-0058 unified Trackbench MPD/local workspace

## Context

ADR-0058 made Trackbench the primary workspace with authority-bound MPD Queue
and Local Queue tabs, and kept the standalone `trackknife` MPD executable as a
compatibility shell "during migration". Since then Trackbench's MPD authority
has carried the day-to-day client workload: live session, library navigation,
transient search, queue mutation, playback modes, outputs, and priority
control, all bound to the same typed session controller the shell used.

Maintaining the shell doubled the cost of every shared workspace change: a
4,000-plus-line parallel main window, its own tests and UI benchmark, and a
second packaging entry point, for a process the project no longer treats as
primary. The project owner has determined that Trackbench has reached feature
parity for the workflows that matter.

## Decision

- The `trackknife` executable, its `src/app` entry point, and the shell-only
  UI (`main_window`, `format_sandbox`, `library_tree_editor_dialog`) are
  removed. Trackbench is the only shipped workspace executable.
- `src/ui` remains as the shared server-side UI library (MPD connection
  dialog, server library tree model/view) consumed by Trackbench.
- The shell's main-window workspace test and UI interaction benchmark are
  removed with it. The `ui-smoke` gate now launches Trackbench through its
  `--screenshot` QA hook so an end-to-end startup smoke test survives.
- Committed `Shift+Enter` search-result tabs and stored-playlist tabs, which
  only the shell provided, are consciously dropped for now; reintroducing them
  inside Trackbench remains open work and is tracked in
  `docs/open-decisions.md`.
- Connection-profile ownership needs no migration: both processes already
  shared the SQLite profile store, which Trackbench continues to own.

## Consequences

- One workspace shell to maintain, test, benchmark, and package; M10's
  "compatibility-shell migration outcome" is resolved as removal.
- Documentation describing the standalone MPD client is historical; current
  behavior is specified against Trackbench's MPD authority.
- Any future need for a headless or minimal MPD surface starts from the typed
  session controller and shared libraries, not from resurrecting the shell.
