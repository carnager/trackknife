# ADR-0023: Local-list audition progression

- Status: accepted
- Date: 2026-08-27
- Owners: Trackknife project

## Context

ADR-0022 bound the single transport to either MPD or Local audition and left
previous/next disabled under Local binding because the audition core plays one
source with no progression concept (ADR-0016). Auditioning a preparation list
one manual activation at a time is tedious: finished tracks stop dead and
skipping requires reselecting rows. ADR-0020 still applies — local playback
must never form a mixed queue with MPD, hand progression to the server, or
mutate MPD transport as a side effect.

## Decision

- An audition remembers its origin: the working-list document and row it was
  started from. Progression is scoped to that one list and visits only its
  local rows; MPD references in mixed lists are skipped, other tabs and the
  MPD queue are never involved.
- Previous/next under Local binding step to the adjacent local row of the
  originating list and are enabled exactly when such a row exists. The
  remembered row is a hint: if edits moved the list, the playing source is
  re-anchored by its exact raw path before stepping.
- A track that plays to its end auto-advances once to the next local row. The
  end of the list stays in the `ended` state — there is no wrap-around and no
  handoff anywhere. A failed load stops progression with the ordinary error
  toast; it is never skipped silently.
- The progression state clears when the audition is closed or the service
  returns to `empty`. Closing the originating tab or removing the playing row
  simply ends progression at the current track.
- The engine keeps its single-source contract; progression lives entirely in
  the workspace shell as ordinary `load_and_play` calls on the serialized
  audition worker (ADR-0021).

## Alternatives considered

- **A local playback queue.** Rejected: ADR-0020 deliberately avoids a second
  persistent queue; the working list itself is the ordered material, and
  duplicating it into a queue re-creates the coordinator ADR-0020 removed.
- **Wrap-around at the list end.** Rejected: auditioning is directed listening
  through preparation material, not loop playback; an explicit restart is one
  click on the first row.
- **Skipping failed sources during auto-advance.** Rejected for now: silently
  skipping hides broken files from the person whose job is to fix them.

## Validation

- Offscreen workspace tests cover: next/previous enablement at list edges,
  stepping forward and back across the list's local rows with the transport
  staying Local-bound, and context reset when the audition closes.
- Auto-advance at natural track end requires real decode and PipeWire output
  and is validated hands-on.

## Revisit when

- Users need audition order decoupled from list order (e.g. shuffle within a
  preparation list).
- The preparation-list surface (M4) introduces explicit readiness states that
  progression should respect.
