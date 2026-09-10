# ADR-0135: MPRIS desktop integration bound to the active authority

- Status: accepted
- Date: 2026-09-10
- Owners: Trackknife project
- Extends: ADR-0058 authority-bound workspace, ADR-0119 local playback
  modes

## Context

Trackbench had no desktop-session presence: media keys, GNOME/KDE player
widgets, and lock-screen controls could not see or steer playback while
the window was in the background (roadmap area 6). On Linux those
surfaces speak MPRIS (`org.mpris.MediaPlayer2` on the session D-Bus), so
one MPRIS player also delivers the media-key requirement.

## Decision

- A new `MprisService` (Qt D-Bus) exports
  `org.mpris.MediaPlayer2.trackknife` — falling back to
  `…trackknife.instance<pid>` when the plain name is taken — with the
  root and Player interfaces: Identity "Trackbench", Raise activates the
  window, Quit is not offered, and the Player surface carries
  PlaybackStatus, Metadata (`mpris:trackid`, `mpris:length`,
  `xesam:title/artist/album`), Position, Volume, and the Can* guards.
- **Authority binding follows the transport contract:** MPRIS mirrors and
  steers exactly what the in-app transport row is bound to — the active
  primary tab's authority. Play/Pause/Stop/Next/Previous trigger the same
  transport actions, Seek/SetPosition route through the shared seek path,
  and Volume writes the shared volume slider, so a desktop control can
  never do something the visible transport could not.
- The 30 Hz transport refresh publishes a compact state snapshot; the
  service diffs it and emits `PropertiesChanged` only for real changes.
  Position is served on demand without change signals, and a
  discontinuous jump on an unchanged track emits `Seeked`, per
  specification. The MPD controller newly exposes the current song's
  artist and album so the metadata map is typed rather than parsed from
  display strings.
- A missing session bus (or failed registration) leaves the service
  inert: publish becomes a no-op and the application behaves as before.

## Consequences

- Media keys and desktop player widgets control whichever authority the
  workspace transport is bound to, satisfying area 6's MPRIS and
  media-key items; optional desktop notifications remain open.
- Tests register a private-instance service, read properties over the
  bus, and drive the methods into signal spies; without a session bus
  they still pin the inert-service and state-diff behavior.
- Artwork (`mpris:artUrl`), LoopStatus/Shuffle mapping onto the
  authority modes, and notifications remain follow-up work.
