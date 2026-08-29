# ADR-0022: Bound single transport with explicit domain ownership

- Status: accepted
- Date: 2026-08-27
- Owners: Trackknife project

## Context

ADR-0020 separated MPD control from Local preparation and required "visibly
local controls" for audition. The first implementation satisfied that with a
second bottom toolbar carrying its own play/pause, stop, and seek. Hands-on use
showed the presentation is worse than the ambiguity it was meant to prevent:
two simultaneous sets of playback buttons make it unclear which pair the user
should reach for, duplicate the seek affordance, and consume vertical space for
a scoped tool. A single set of shared controls with implicit targeting would be
equally ambiguous in the other direction.

The prohibitions in ADR-0020 are semantic — no mixed queue, no combined
transport timeline, no implicit MPD mutation, no automatic backend handoff —
plus a legibility requirement that ownership stay obvious. None of them mandate
two physical transport widgets.

## Decision

- The top transport row is the only transport surface. At any moment it is
  **bound** to exactly one domain, MPD or Local audition, and every transport
  widget (previous/play-pause/stop/next, cover, title/detail, elapsed/seek/
  duration, volume) renders and controls only the bound domain.
- A **domain chip** (`transport-domain-chip`, backed by the palette-discoverable
  `action-transport-toggle-domain`) sits at the head of the transport row. It
  displays the bound domain (`MPD` / `LOCAL`), and clicking it switches the
  binding. The chip is the visible-ownership mechanism required by ADR-0020.
- Binding follows the last explicit play action. Starting a local audition
  binds Local. Explicit MPD playback-starting activations — playing a queue
  row, replace-and-play from search, library, or the server tree — bind MPD.
  Transport buttons act on the current binding and never rebind. Append,
  insert, browse, and playlist loading never rebind.
- Switching the binding is **bind-only**: it never issues play, pause, or stop
  to either domain. Starting local audition never mutates MPD transport — MPD
  is shared server state that other clients and listeners may depend on. When
  the unbound domain is playing, the chip appends a dot indicator and its
  tooltip explains it, so "switch, then pause" remains an explicit two-step
  user action.
- Under Local binding, previous/next are disabled (local audition has no track
  progression by design) and the volume slider is disabled until PipeWire
  device/volume selection exists. Elapsed/seek/duration map the sample-domain
  snapshot to milliseconds; the title shows the escaped source basename with
  the escaped full path and PipeWire/underrun diagnostics in tooltips.
- Switching to Local requires a loaded (non-empty) audition; switching to MPD
  is always allowed. The binding stays put through `ended` and `failed` — those
  are states the user asked to see — and falls back to MPD only when the local
  domain becomes empty after an explicit close (`action-local-audition-clear`).
- Status-bar playback modes, ReplayGain, and outputs remain MPD-only controls.
- The MPD and Local domains keep separate queues, timelines, and state; the
  binding is presentation-level routing inside the main window, not a
  backend-neutral player abstraction or progression coordinator.

## Alternatives considered

- **Keep two transport toolbars.** Rejected: duplicate play/seek affordances
  are the observed usability failure this ADR fixes.
- **Auto-pause the other domain on switch or audition start.** Rejected: it
  makes local audition mutate shared MPD transport as a side effect, which
  ADR-0020 forbids for good reason.
- **A backend-neutral player interface.** Rejected: ADR-0020 already rejected a
  progression coordinator; the binding needs only per-widget routing across a
  handful of call sites in the main window.

## Consequences

- ADR-0020's "visibly local controls" clause is amended: visibility is provided
  by the bound transport's domain chip and Local-specific rendering, not by a
  separate control surface. All ADR-0020 semantic prohibitions stay in force.
- The separate Local audition toolbar from ADR-0021's UI slice is removed. Its
  diagnostics (PipeWire node, position, underruns) move into transport tooltips
  while Local is bound.
- The four transport actions keep their object names, so persisted shortcut
  overrides now drive whichever domain is bound — the expected meaning of "one
  transport".
- Offscreen tests cover: audition start binds Local; the chip switches without
  issuing transport commands; transport buttons never rebind; failed (non-
  playing) audition shows no unbound-playing indicator; closing the audition
  falls back to MPD and disables the switch.

## Revisit when

- ~~Local-list audition progression is designed (previous/next under Local
  binding would gain meaning).~~ Resolved by ADR-0023: previous/next step
  through the originating working list's local rows.
- PipeWire device/volume selection lands (volume slider behavior under Local
  binding).
