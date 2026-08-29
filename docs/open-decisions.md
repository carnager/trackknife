# Open decisions

Accepted foundations are recorded in ADRs through 0025. The MPD-client-first
direction, separate live queue/working-list model, `libmpdclient` boundary,
and the two-application split (a pure MPD client plus the standalone Trackbench tool
sharing this repository's libraries) are no longer open.

## Resolved for M2–M3

1. Credentials remain session-only until a desktop secret-service adapter is
   selected; ordinary settings and SQLite never receive passwords.
2. Browse/search pages and artwork are bounded memory caches. Only explicit
   working-list snapshots survive restart, so Trackknife does not create a
   competing local library index.
3. Playback modes acknowledge optimistically and reconcile against the next
   authoritative snapshot. Stable-ID queue mutations remain pending until MPD
   confirms them; conflicts refresh instead of replaying the edit.
4. The typed MusicBrainz projection and tested deterministic fallback keys own
   release/artist/medium/track grouping. New aliases require corpus evidence.
5. The default visual language is desktop-native Qt Widgets with lightweight
   delegates for compact queue grouping and row states, as accepted by
   ADR-0012.

## Resolved local-engine foundations (pre-split M4 work)

ADR-0014 fixes raw-path preservation, deterministic recursive folder
discovery, duplicate handling, directory-symlink behavior, cancellation, and
recent-location semantics for ad-hoc local sources. ADR-0015 fixes the initial
streaming decode contract at source rate/layout and the decoder-core PCM WAV,
AAC/M4A, LAME MP3, Opus/Ogg, and Ogg Vorbis acceptance fixtures. ADR-0016 and
ADR-0018 fix the bounded playback core and the direct PipeWire adapter.
ADR-0021 fixes the serialized playback worker, ADR-0023 list progression, and
ADR-0024 volume/device selection (cubic percent onto PipeWire's stream mixer,
bounded sink enumeration, in-place retargeting preserving position and state).
These contracts move into Trackbench unchanged; the ADR-0022 domain chip
and bound dual-domain transport are superseded by ADR-0025.

## Needed during M4 (application split)

1. Shared-library layout and target boundaries for `core`, `titleformat`,
   `formats`, `audio`, the PipeWire adapter, persistence infrastructure, and
   reusable Widgets components.
2. The SQLite migration policy for persisted MPD-client lists that contain
   local raw-path rows (drop, export to Trackbench, or both).
3. Local-path containment implementation and behavior for symlinks/bind
   mounts.
4. Expansion from the proven decoder-core matrix to the shipped local
   playback format matrix (FLAC and WavPack are the first candidates needing
   real fixtures), plus PipeWire buffer presets.
5. PipeWire hotplug/default-change behavior.
6. Album-grouping fallback rules for local files lacking MusicBrainz tags,
   and cover-art source precedence (embedded art versus folder images).

## Needed before M5–M8

1. Sidecar location/format and precedence relative to embedded metadata.
2. Default filename sanitization and Unicode normalization policy.
3. ReplayGain true-peak and Opus output-gain/storage policy.
4. Initial exact read/write/preservation claims per local format.
5. Undo retention by time, operation count, and disk budget.
6. Converter's shipped codec/device presets, resampler quality settings, and
   source-root inference UX.
7. MusicBrainz web-service rate-limit/cache policy and whether AcoustID
   fingerprinting earns its dependency (M6).
8. Destination profiles for organized output, including MPD-music-root
   publication defaults.

## Deferred

- Whether a local-tool-owned library index is ever needed ("no database for
  now, maybe later").
- Cross-tool integration: opening a mapped server item in Trackbench and
  triggering an MPD database update after publication.
- Deeper query/autoplaylist language.
- Plugin ABI/distribution.
- A transactional Melody import/upload protocol and destination adapter; no
  such extension is currently claimed.
- CD ripping, internet radio, streaming-service integration, and non-Linux
  ports.
