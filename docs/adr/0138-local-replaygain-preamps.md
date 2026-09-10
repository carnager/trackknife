# ADR-0138: Local playback ReplayGain preamps

- Status: accepted
- Date: 2026-09-10
- Owners: Trackknife project
- Extends: ADR-0119 local playback modes and ReplayGain

## Context

Local ReplayGain playback applies the selected track/album gain with peak
limiting, but offers no level adjustment: normalized material often plays
quieter than the user wants, and tracks without loudness data jump in
level relative to normalized ones. The specification requires separate
preamps for tracks with and without loudness data (roadmap area 5;
ADR-0119 recorded preamps as future work).

## Decision

- `ReplayGainPreamps` carries two decibel values bounded to ±20 dB:
  `with_gain_db` applies on top of the selected gain before the existing
  peak limit (`min(gain, 1/peak)` still caps the result), and
  `without_gain_db` applies alone when ReplayGain is active but the
  playing source carries no usable gain value — previously such tracks
  played at unity. With ReplayGain off, neither preamp applies.
- `LocalPlayback`/`LocalAuditionService` gain `set_replay_gain_preamps`,
  following the ReplayGain-mode command path exactly: serialized worker
  command, stored in the session config so newly loaded sources inherit
  it, and surfaced in the snapshot.
- The local ReplayGain menu gains a "Preamp…" dialog with the two spin
  boxes (0.5 dB steps), persisted under `playback/rg-preamp-with` /
  `playback/rg-preamp-without` and applied with the other local playback
  modes. MPD playback is untouched — server gain stays server-owned.

## Consequences

- Normalized and unnormalized material can be leveled against each other
  without touching files, matching the established player convention.
- Tests pin the multiplier math (preamp on top of gain, the peak limit
  capping a boosted result, the without-data path, off staying unity)
  and the persisted settings round trip.
- The additional processing-mode policies from the specification (no
  processing / prevent clipping variants) and true-peak-based limiting
  remain follow-up work.
