# ADR-0025: Split local file work into a standalone application

- Status: accepted
- Date: 2026-08-29
- Owners: Trackknife project
- Supersedes: the single-application scope of ADR-0009; the in-client Local
  domain of ADR-0017 and ADR-0020; the domain chip and bound dual-domain
  transport of ADR-0022; the "scoped auditioning" framing of ADR-0021,
  ADR-0023, and ADR-0024 (their engines and contracts carry over intact)

## Context

ADR-0020 already separated MPD control from local preparation inside one
executable: two Library domains, one bound transport with a domain chip, and
local playback deliberately limited to "auditioning" so it could not be
mistaken for a second queue authority next to a shared MPD server.

Living with that model through M4 showed the boundary wants to be a process
boundary, not a pane boundary. An MPD client and a local file workstation have
different primary objects (server queue occurrences versus files on disk),
different failure domains (network/protocol versus filesystem/codec), and
different feature growth (server compatibility versus tagging, MusicBrainz,
ReplayGain, conversion, resampling). Keeping both in one window forces
permanent explanatory UI — the domain chip, capability gates on every row,
"which transport am I holding" rules — that exists only to manage the
combination, not to serve either job.

The basic MPD client is finished: M0–M3 closed with live validation against
stock MPD 0.24.14 and Melody 0.23.5. The local engine work completed during M4
(FFmpeg probe/decode with proven gapless trim, the bounded SPSC playback core,
the direct PipeWire adapter, raw-path ingestion, audition service, volume and
device selection) is Qt-free and MPD-free by construction, so it moves without
rework.

## Decision

- The repository builds **two applications** that share internal libraries:
  - **Trackknife**, the MPD/Melody client. Its scope freezes to server work:
    connection, browse/search, live queue, stored playlists, transport,
    outputs, and later the Melody output endpoint.
  - **Trackbench** (binary `trackbench`; named 2026-08-29), a
    foobar2000-inspired tabbed workstation focused entirely on local files:
    playback, album grouping, tagging, MusicBrainz support, ReplayGain,
    conversion, and resampling. It maintains no library database for now; a
    local index may be reconsidered later.
- The Local domain is **removed from the MPD client**: the Local Folders
  library tab, local ingestion into its working lists, the audition service
  wiring, the domain chip, the bound dual-domain transport, and the local
  volume/device controls. The MPD client's transport returns to controlling
  MPD alone. Persisted working lists drop or migrate local-only rows.
- In Trackbench, local playback is **first-class, not auditioning**. With
  no shared server queue in the same process, the ADR-0020 restriction is
  enforced by the process boundary instead of UI rules: Trackbench may own
  real playback progression, per-tab queues, gapless transitions, and
  album-grouped views without any risk of implying MPD ownership.
- Shared code lives in internal static libraries with no public ABI: `core`
  primitives, the `tkfmt-1` engine, `formats` (FFmpeg), `audio` (playback
  core), the PipeWire adapter, persistence infrastructure, and reusable
  Widgets components (tabbed track workspace, transport row, command palette,
  toasts, shortcut/config machinery). The two applications share one visual
  and interaction language.
- Neither application depends on the other at runtime. Trackbench speaks
  no MPD protocol. Publishing prepared files into a directory that happens to
  be an MPD music root is plain filesystem output; requesting an MPD database
  update afterwards is a job for any MPD client, including Trackknife.
  Cross-tool conveniences (open a mapped server item in Trackbench,
  trigger an update after import) are deferred until both tools stand alone.
- The preparation-safety contract is unchanged and now lives in Trackbench:
  every mutation uses plan, preview, revalidation, journal,
  verification, and a truthful recovery story. The metadata-provider boundary
  (MusicBrainz first) proposes values with provenance and confidence into the
  staged preview and never writes files directly.

## Alternatives considered

### Keep one application with two domains (status quo, ADR-0020)

Workable — M4 proved the engines — but every future local feature (tag grid,
ReplayGain queue, converter jobs) would keep paying the combination tax:
capability-gating against MPD state it does not use, sharing a transport it
must not touch, and growing a second product inside a client whose basic job
is already done.

### Separate repositories

Cleanest conceptual split, but the shared Qt-free modules are exactly the
high-value proven code (`tkfmt-1`, decode, playback core, PipeWire). Vendoring
or submoduling them across repos adds release friction with no user benefit.
One repository, two executables, shared internal libraries keeps one CI, one
test suite, and coordinated quality gates.

### Extract the shared core into a third library repository first

Most flexible long-term, most up-front work now, and it would stabilize
library boundaries before a second consumer exists to validate them. Can still
happen later if a real external consumer appears.

## Consequences

- The MPD client gets simpler: no domain chip, no dual-binding transport
  rules, no local capability gating. Its remaining roadmap is Melody endpoint
  work and hardening.
- The milestone plan is restructured: M4 becomes the application split plus
  Trackbench's playback workspace; tagging, MusicBrainz, ReplayGain, and
  conversion milestones move into Trackbench; hardening/release covers
  both applications.
- ADR-0022/0023/0024 UI decisions are superseded, but their tested engine
  contracts (serialized audition worker, list progression semantics, cubic
  PipeWire volume, in-place device retargeting) transfer directly into
  Trackbench's player.
- SQLite persistence needs a versioned migration for MPD-client lists that
  contain local raw-path rows.
- Two applications must be packaged, documented, and released; the shared
  visual language keeps that cost mostly mechanical.

## Validation

- Both executables build, run, and pass their test suites with no runtime
  dependency on each other; the MPD client links no FFmpeg/PipeWire playback
  path once the split completes.
- Existing M3 gates (million-row budgets, live-server passes, reconnect
  regression tests) stay green in the pure MPD client after the removal.
- Trackbench plays the proven codec/container fixtures gapless through
  the migrated engine with tabbed lists and album grouping.
- A persisted MPD-client workspace containing local rows migrates without
  losing server-backed list content.

## Revisit when

- shared library boundaries prove painful enough that a third library
  repository (or a merge back) is cheaper;
- users demonstrate a real need for controlling MPD and local playback in one
  window again;
- a local library index becomes necessary for Trackbench's performance
  goals.
