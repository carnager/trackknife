# ADR-0144: Optional desktop notifications

Date: 2026-09-11

Status: accepted

## Context

Roadmap Area 6 wants playback to stay legible while the window is in
the background. MPRIS (ADR-0135) already covers control and applet
display; what remains is a transient "now playing" toast on track
changes. The roadmap's constraints: optional, and quiet by default.

ADR-0135 also built the right seam. `publishMprisState()` computes an
authority-aware now-playing snapshot (`MprisPlaybackState` with a
stable per-track key) on every transport refresh, for exactly the
authority the in-app transport is bound to.

## Decision

A `DesktopNotifier` consumes the same `MprisPlaybackState` stream and
posts a notification through `org.freedesktop.Notifications` when the
playing track identity changes. Its rules keep it quiet:

- **Off by default.** A checkable "Desktop notifications" entry in the
  Playback menu enables it, persisted in settings.
- **Background only.** Nothing is posted while the Trackknife window
  is active; the user is already looking at the transport. Track keys
  are still tracked while active or disabled, so enabling or
  unfocusing never retro-notifies an old change.
- **Track transitions only, while Playing.** Pause/stop/seek/volume
  never notify. The notification body is "Artist — Album" under the
  title, markup-escaped.
- **One replaced bubble.** The server-returned notification id is
  reused (`replaces_id`), and the `transient` hint plus low urgency
  keep it out of notification history and do-not-disturb escalation.
- **Degrade to nothing.** No session bus, no notification daemon, or a
  failed call are silently ignored — playback behavior never depends
  on the desktop.

The trigger logic is deterministic and separated from D-Bus delivery
(an injectable send seam), so tests assert the decision stream without
posting desktop notifications.

## Consequences

- Background listening gets a quiet per-track toast in both
  authorities, consistent with what MPRIS applets show, with no new
  state model — the MPRIS snapshot remains the single now-playing
  truth.
- Artwork in the notification (image hint) is a follow-up; it needs a
  file-path or pixel handoff that the MPRIS state deliberately does
  not carry today.
