# Trackknife milestones

Milestones are capability gates, not calendar promises. The order reflects the
MPD-client-first direction accepted in ADR-0009 and the two-application split
accepted in ADR-0025: **Trackknife** stays a pure MPD/Melody client, and the
**Trackbench** local tool is a separate
foobar2000-inspired workstation for local files — playback, album grouping,
tagging, MusicBrainz, ReplayGain, conversion, and resampling — sharing this
repository, its internal libraries, and its visual language.

## Status

| Milestone | State | Outcome |
| --- | --- | --- |
| M0 | Complete | Toolchain, core boundaries, fixtures, and measured legacy UI spike |
| M1 | Complete | Shared `tkfmt-1` expression engine and sandbox |
| M2 | Complete | Reliable asynchronous MPD session and domain backbone |
| M3 | Complete | Polished Qt MPD workspace — the basic MPD client is finished |
| M4 | Active | Application split and Trackbench's playback workspace |
| M5 | Planned | Fast local tag workspace and safe file operations |
| M6 | Planned | MusicBrainz identification and metadata providers |
| M7 | Planned | Universal parallel ReplayGain workflow |
| M8 | Planned | Parallel converter, resampler, and organized output |
| M9 | Planned | Melody output endpoint for the MPD client |
| M10 | Planned | Hardening, packaging, and first public releases |

Only one milestone is normally active. Preparatory work is allowed when it
removes a real dependency, but moving on never disguises a failed gate.

## Rules applying to every milestone

- Keep the core independent of Qt UI types.
- Neither application depends on the other at runtime; shared behavior lives
  in shared internal libraries, not copies.
- Add tests with behavior; protocol behavior needs stock MPD and Melody
  provenance, and format claims need real files.
- Never block the UI thread on network, disk, decode, tags, SQL, or unbounded
  formatting.
- Use bounded workers, cancellation, progress, and structured errors.
- Use MPD song IDs for live-queue mutations whenever the protocol supports
  them; positions are display data and can race with other clients.
- Keep remote URI and local raw path distinct; Trackbench never implies
  MPD membership and the MPD client never implies local file access.
- Preserve the shipped default experience and keyboard/mouse parity in both
  applications.
- Every mutation uses plan, preview, revalidation, journal, verification, and a
  truthful recovery story.
- Update this file and `docs/feature-matrix.md` when a gate changes state.
- Do not stabilize a plugin ABI before built-in boundaries have real use.

## M0 — Decisions, skeleton, and test infrastructure

### Objective

Prove the selected C++23/Qt 6 stack, performance approach, tests, and hard core
boundaries.

### Completion evidence

- Accepted ADRs cover Qt, licensing, C++/Qt boundaries, platform baseline,
  media/storage/output backends, Unicode, and the workspace.
- CMake presets, warnings, formatting, static analysis, unit tests, sanitizers,
  fuzzing, and CI exist.
- Qt-free error/result, stable ID, cancellation, progress, and revision
  primitives have tests.
- Deterministic 10k/100k/1m fixtures and measured million-row Qt models satisfy
  the initial UI budgets.
- The skeleton contains no fake playback/tag features presented as complete.

## M1 — Trackknife formatting expressions

### Objective

Provide one deterministic expression engine for display, sorting, grouping,
queue/list views, transformations, and conversion/file naming.

### Completion evidence

- ADR-0008 and `docs/title-formatting.md` define the versioned `tkfmt-1`
  language without foobar2000 or Picard compatibility claims.
- Source-preserving parser, diagnostics, immutable programs, bounded scalar and
  multi-value evaluation, dependency extraction, caching identity, batch use,
  cancellation, and concurrency tests are complete.
- Queue, tree, and conversion relative-path fixtures share the compiled program
  type.
- The native sandbox and CLI expose live evaluation and dependencies.
- Parser and evaluator each passed 100,000-run sanitized fuzz campaigns.
- The optimized scalar benchmark evaluates one million synthetic rows at 4.49
  million rows/second; evidence is under `benchmarks/results/`.

The sandbox remains available for later hands-on UX refinement, but no longer
blocks the MPD client backbone.

## M2 — MPD session and domain backbone

### Objective

Connect reliably to stock MPD and Melody without coupling protocol work to Qt
widgets, and expose typed immutable state for the future UI.

M2 closed complete; its detailed work list and exit criteria are condensed
into the completion evidence below.

### Completion evidence

- `libmpdclient >= 2.22` sits behind a Qt-free, move-only adapter with bounded
  connect/command timeouts and TCP keepalive.
- Ordered repeated/unknown tag projection, MusicBrainz identities beyond the
  baseline enum, queue occurrence IDs, stock outputs, and Melody output
  extensions have domain tests.
- Lexical local-root mapping rejects URLs, absolute paths, traversal, empty
  components, invalid protocol UTF-8, and percent-decoding guesses.
- A scriptable fake socket-level MPD server covers greeting/version,
  capabilities/tag types, current song, queue, outputs, ping, and idle events;
  slow/disconnect/ambiguous-mutation and concurrent-client fixtures cover the
  fault surface.
- The persistent session owns separate serialized command and cancellable
  `idle` connections, publishes generation-tagged snapshots, selectively
  refreshes queue/player/output state, and reconnects with bounded backoff.
  A dropped response from a non-idempotent queue add produces an error, is
  never replayed, and is followed by a generation-tagged reconnect.
- Live-queue mutation uses stable MPD song IDs exactly once: play, append,
  insert, delete, move, crop, clear, drag-reorder permutations, and advertised
  priority control, with `plchanges` reconstruction and full-snapshot
  fallback. Stale-ID rejections classify as queue conflicts, refresh state,
  and surface a non-modal toast.
- Typed transport, volume, playback modes, ReplayGain status/control,
  additive MPD outputs, and Melody exclusive-output switching cross the
  session and controller boundaries; a live Melody system verified them by
  hand alongside the headless `trackknife-mpd-probe`.
- Typed non-recursive browse, bounded/paged any-tag search with cancellation
  and stale-result protection, stored-playlist discovery, and the complete
  advertised stored-playlist lifecycle run on the session worker.

## M3 — Polished Qt MPD workspace and queue tabs

### Objective

Deliver a genuinely pleasant daily MPD client before expanding local tooling.

Implementation note: the M3 implementation and live acceptance pass are
complete against Melody protocol 0.23.5 and stock MPD 0.24.14.
Versioned SQLite state preserves multiple connection profiles, scratch/saved
identity, tab ordering, duplicates, remote profile identity, raw local path
bytes, repeated snapshot fields, dirty/pinned state, and per-list view presets.
The workspace supports named and scratch list item editing, pin/duplicate,
keyboard tab switching/reordering, command-driven and cross-tab drag copy/move,
server-side artist/album/genre/stored-playlist browsing, continued search pages,
bounded async artwork, command discovery, and persisted configurable shortcuts.
SQLite profile/list/view state is loaded and saved on one serialized worker;
rapid edits are debounced and shutdown performs a durable worker-thread flush.

### M3 completion status (2026-08-26)

Implementation, automated verification, and live compatibility validation are
finished.

Completed evidence:

- [x] All 19 tests pass in development, ASan/UBSan, and optimized release
  builds.
- [x] Clang static analysis, warnings-as-errors, formatting, and SPDX gates
  pass.
- [x] The one-million-row UI benchmark meets every provisional p95 budget; see
  `benchmarks/results/2026-08-26-m3-offscreen-smoke.md`.
- [x] A read-only live Melody probe against `gemenon:6600` projected protocol
  0.23.5, 92 commands, 7 tag types, 18 queue occurrences, 4 outputs, and the
  advertised exclusive-output extension.
- [x] The final audit moved SQLite profile/list/view work off the UI thread,
  added debounced/durable persistence, exposed the Playlists library category,
  fixed album/artist/genre indexes being replaced by the folder model, added
  direct Enter activation, and implemented bounded exponential reconnect
  backoff with jitter.
- [x] The hands-on Melody UI/mutation/concurrent-client pass succeeded against
  protocol 0.23.5, including exclusive output switching and exact restoration
  of the shared server's original state.
- [x] The same pass succeeded against stock MPD 0.24.14, including additive
  outputs and the complete advertised stored-playlist lifecycle.
- [x] A live daemon-restart defect found during the stock pass now has a fix
  and a socket-level autonomous-reconnect regression test. Details are in
  `docs/m3-validation.md`.

### Exit criteria (met)

- An untouched first run can connect and reach a complete listening workspace
  without constructing panels.
- Browse/search, transport, output switching, live queue editing, and stored
  playlists work on both stock MPD and Melody.
- Scratch tabs survive restart with order, duplicates, remote identity, and
  dirty state intact.
- A user can complete all common queue and tab workflows with only keyboard or
  only mouse.
- Another client can edit playback/queue/output state without stale UI or lost
  local tab data.
- Measured tab switching, queue acknowledgement, scrolling, search, and artwork
  behavior meet `docs/ui-workspace.md` budgets.

With ADR-0025, M3's outcome is the finished basic MPD client. Its remaining
roadmap items are context-menu parity (delivered in M4 groundwork below), the
Melody endpoint (M9), and hardening (M10).

## M4 — Application split and Trackbench's playback workspace

### Objective

Stand up Trackbench as its own application with the migrated, proven
FFmpeg/PipeWire engine; return the MPD client to a pure server client; and
deliver Trackbench's foobar2000-inspired tabbed playback workspace with
album grouping. Performance and user experience are the primary gates. No
local library database is created; the tool grows from direct filesystem
navigation (revisit an index later if real use demands it).

### Engine and UI groundwork completed before the split

This work was done under the pre-ADR-0025 single-application plan. The engine
contracts move into Trackbench unchanged; the in-client UI surfaces are
migration sources.

- The M3 context-menu carryover reuses the registered command actions for
  live-queue, working-list, stored-playlist, search, library, and tab
  surfaces. Right-click establishes the target row/tab while preserving an
  existing multi-selection; play/open/replace, add-next/append, remove/crop,
  queue priority, copy/move, rename/pin/duplicate/reorder/close, shortcuts,
  enabled state, capability gates, and command-palette discovery share one
  action path. (Stays in the MPD client; the pattern is reused locally.)
- ADR-0014 defines one raw-path-safe ingestion path for dialogs, drag/drop,
  recent locations, and command-line arguments without creating a local
  library index. Cancellable bounded discovery recursively expands folders in
  deterministic raw-byte order, preserves duplicate occurrences, reports
  per-path failures, and persists exact local source bytes, with invalid
  UTF-8 filename coverage.
- ADR-0015 and the Qt-free `formats` adapter establish cancellable FFmpeg
  probe and streaming decode at source rate/layout: sample-domain seeking
  with flush and precise discard, end-exclusive range trim without unbounded
  buffering, and exactly-once codec/container delay and padding handling.
  Repository-owned PCM WAV, AAC-in-M4A, LAME MP3, Opus-in-Ogg, and Ogg Vorbis
  fixtures prove complete-decode identity, non-silent edges, gapless adjacent
  ranges, and sought-slice byte identity, including positive and overlapping
  best-effort timestamps.
- ADR-0016 adds the bounded local playback core: a single non-real-time
  producer fills an explicitly sized SPSC PCM ring; the real-time render
  boundary only copies/zero-fills preallocated storage and updates atomics.
  Tests prove transport states, exact pause/resume/seek, starvation recovery
  and underrun accounting, segment drain, restart, cancellation, and
  invalid-command preservation against real PCM.
- ADR-0018 adds the direct PipeWire adapter: exact source-rate interleaved
  float mono/stereo, inactive connect, mapped buffers, the requested quantum
  in an RT-safe process callback, target-object/exclusive configuration, and
  synchronous activate/drain/quiesce, with a silent live-server test.
- ADR-0021 serializes source opening, production, activation, drain, seek,
  replacement, and cancellation on one dedicated audition worker with bounded
  commands and non-blocking status publication.
- ADR-0023's list-progression semantics (previous/next through the
  originating list's local rows, single end-of-track auto-advance, visible
  failure instead of skipping) and ADR-0024's volume/device contract (cubic
  percent onto PipeWire's stream mixer, bounded sink enumeration, in-place
  retargeting preserving position/state, sticky failure visibility) are
  tested and carry into the local player. The domain chip and bound
  dual-domain transport they fed (ADR-0022) are superseded by the split.
- ADR-0017's Local Folders library tab (persisted raw-byte roots, lazy
  off-thread enumeration, symlink policy, lossless display escaping) and the
  ADR-0019 configurable lazy MPD library tree with its strict filter are
  done; the former migrates to Trackbench, the latter stays in the MPD
  client.
- Live-queue priority badges, the drag-reorder drop-indicator fix with a
  live-confirmed stable-ID permutation, and the anchored non-modal search
  panel (ADR-0013) are finished MPD-client polish.

### Work

1. Restructure the build into shared internal libraries (`core`, `titleformat`,
   `formats`, `audio`, PipeWire adapter, persistence infrastructure, reusable
   Widgets components) plus two application targets, with tests and quality
   gates covering both.
2. Create the `trackbench` executable: main
   window, tabbed track workspace, transport row, command palette, configurable
   shortcuts, toasts, and transactional persistence, reusing the shared
   components so both applications keep one visual and interaction language.
3. Remove the Local domain from the MPD client: the Local Folders tab, local
   ingestion, the audition service wiring, the domain chip, the bound
   dual-domain transport, and local volume/device controls. Provide a
   versioned SQLite migration for persisted lists containing local rows. The
   MPD client's transport controls MPD alone again.
4. Migrate ingestion (ADR-0014), folder navigation (ADR-0017), the audition
   worker (ADR-0021), progression (ADR-0023), and volume/device selection
   (ADR-0024) into Trackbench, promoting audition to first-class playback:
   play/pause/seek/previous/next, per-list progression, and end-of-track
   advance owned entirely by Trackbench.
5. Album-grouped presentation for local lists: deterministic MusicBrainz-aware
   grouping with tag fallbacks, dense grouped rows, and bounded serial cover
   loading from embedded art and folder images.
6. Local-path containment and symlink/bind-mount revalidation policy for the
   sources file operations will later accept.
7. Expand from the proven decoder-core matrix to the shipped local playback
   format matrix (at least FLAC and WavPack candidates need real fixtures),
   plus PipeWire buffer presets and hotplug/default-change reaction.
8. Gapless track transitions in the local player across the proven fixture
   matrix.

### Current progress (2026-08-29)

- Done (work items 1–4, first slice): the build now produces two executables
  over shared libraries. `src/uicommon` holds the MPD-free shared Widgets
  pieces (command palette, list persistence service, local folder tree
  model); `src/bench` builds the `trackbench` executable. Trackbench v1 has
  tabbed persistent local lists in its own SQLite database, a folder library
  dock with persisted raw-byte roots, file/folder/drag-drop/CLI ingestion
  through the ADR-0014 discovery path, and a single local transport over the
  ADR-0021 worker with ADR-0023 end-of-track auto-advance and prev/next plus
  ADR-0024 volume and PipeWire device selection.
- Done: the Local domain is removed from the MPD client — audition service
  and timer, domain chip and bound dual-domain transport, Local Folders tab,
  local ingestion/recent locations, local URL drops, and the local device
  chooser are gone; the transport controls MPD alone. Persisted local rows
  remain visible inert working memory (removable/transferable, not
  playable). The superseded chip/audition tests were deleted; all 25 tests,
  warnings-as-errors, format, and SPDX gates pass, and `ldd` shows the
  client links no PipeWire/FFmpeg while `trackbench` does.
- Done: the `formats` probe projects ordered container and best-stream tags
  (`MediaProbe::tags`, demuxer order, repeated names preserved), and a
  repository-owned tagged FLAC fixture with recorded provenance proves tag
  survival plus a 4,410-frame contiguous non-silent complete decode.
  Trackbench lists now show Title/Artist/Album/Length columns: new rows are
  enriched by a bounded cancellable background probe batch off the UI
  thread, results persist into the list document's typed fields and
  duration, and restored lists render without reprobing. CLI/startup opens
  queue until the asynchronous list restore finishes instead of being
  dropped. An offscreen end-to-end run verified the FLAC's tags and
  duration surviving into the SQLite document.
- Done: `core::revalidate_contained_source` performs the filesystem
  complement to the lexical mapping guards: it resolves the configured root
  and the referenced path to their final targets and accepts only a regular
  file strictly inside the resolved root. Tests cover in-root symlinks,
  escaping symlinks, dot-dot traversal, byte-prefix sibling directories,
  missing paths, directories, and empty inputs. File-operation call sites
  arrive with the M5 planner, which revalidates immediately before offering
  or executing every operation.
- Done: a tagged WavPack fixture with recorded provenance joins the matrix,
  proving APEv2 tag projection and a 4,410-frame contiguous non-silent
  complete decode alongside the FLAC entry.
- Done: ASan stress-running the session suite exposed two real M2-era
  concurrency bugs. First, the authoritative reconnect snapshot was
  published to observers before command acceptance was re-enabled, so an
  observer reacting to the snapshot could have a command rejected on a
  healthy connection; acceptance now precedes publication. Second, the
  command worker and the idle worker's reconnect backoff share one
  condition variable, and single-waiter notification could be consumed by
  the backoff waiter, leaving an accepted user command queued but
  unexecuted until an unrelated wakeup; enqueue/request now notify every
  waiter. The session test went from roughly one failure in twenty ASan
  runs to 0 in 150, and it now prints its wait-state flags on timeout.
- Remaining: Trackbench album-grouped presentation with covers, gapless
  verification in the shipped player, per-tab view polish (drag reorder,
  pin/duplicate, dirty indicators at parity with the client), the rest of
  the shipped format matrix (AIFF, WAV/RF64 edge cases, cue/subsong),
  PipeWire buffer presets and hotplug, and the shared UI budget
  measurements on large folders.

### Exit criteria

- Both applications build, run, and pass all tests independently; the MPD
  client links no local playback path and keeps every M3 gate green.
- A first run of Trackbench can open a folder, see album-grouped tracks,
  and play them with only keyboard or only mouse.
- Common local formats play, seek, pause, and transition gapless for proven
  codec/container fixtures, and playback cannot be starved by UI or
  background work under defined load.
- Persisted MPD-client workspaces containing local rows migrate without
  losing server-backed list content.
- Local sources are contained and revalidated before any file operation is
  offered.
- No unintended sample processing occurs with gain/DSP/conversion disabled.
- Measured list scrolling, tab switching, and grouping meet the shared UI
  budgets on large folders.

## M5 — Fast tag workspace and safe file operations

### Objective

Turn Trackbench into a serious preparation workspace where multi-file
editing feels like a modern data tool rather than a stack of per-field dialogs.

### Work

1. Local metadata/source model and read adapters for the initial real-file
   matrix, including arbitrary ordered values and MusicBrainz fields.
2. Virtualized track-by-field grid with common/mixed/missing states, direct
   keyboard navigation, type-to-add/fuzzy field commands, rectangular paste,
   saved field layouts, and an individual-values inspector.
3. Staged documents, source-revision conflicts, per-format preservation tests,
   and full write previews.
4. Named transformation chains, `tkfmt-1`-derived values, numbering, and the
   separate capture-pattern language.
5. Artwork operations and clear provenance/capability display.
6. Shared rename/move/copy planner with destination preview, sanitization,
   collision checks, journaling, verification, and reconciliation.
7. Metadata-provider boundary for proposed values, identifiers, artwork
   references, provenance, and confidence, proven by internal use before any
   online provider (M6) or public plugin ABI.

### Exit criteria

- Common bulk edits need no repetitive per-target clicking or fixed giant
  dropdown navigation.
- Keyboard-only and paste-heavy editing is fast on large selections.
- Each advertised writable format passes real round-trip, unknown-data,
  artwork, MusicBrainz, and audio-essence preservation tests.
- Plans stop on source revision changes and injected failures leave a valid
  original or documented recovery journal.
- Local references in every affected tab follow successful file moves as one
  logical update.

## M6 — MusicBrainz identification and metadata providers

### Objective

Make MusicBrainz support real: identify local files and propose complete,
provenance-carrying metadata through the M5 provider boundary, as explicit
network operations that never bypass staged preview/write safety.

### Work

1. MusicBrainz web-service client with honest rate limiting, caching, offline
   behavior, and clear failure states.
2. Release/recording matching for selections and albums: search, disc-shaped
   candidate ranking, track alignment, and a side-by-side proposal review in
   the staged metadata preview.
3. Complete identifier coverage: recording, track, release, release-group,
   artist, work, and disc IDs; credited names kept separate from sort names.
4. Cover Art Archive artwork proposals through the existing artwork
   operations.
5. Optional acoustic fingerprinting (AcoustID) evaluated behind the same
   provider boundary; it ships only if match quality justifies the dependency.
6. Provider provenance and confidence surfaced in the review UI; accepted
   proposals become ordinary staged edits.

### Exit criteria

- A ripped or downloaded album with poor tags can be identified, reviewed
  field-by-field, and committed without hand-typing standard metadata.
- Every network lookup is explicit, cancellable, cached, and fails visibly
  without corrupting staged work.
- Accepted proposals round-trip through the same preservation tests as manual
  edits; rejected proposals leave no trace in files.
- MusicBrainz-aware grouping and sorting improve measurably on the fixture
  corpus without breaking deterministic fallbacks for untagged files.

## M7 — Universal parallel ReplayGain

### Objective

Treat loudness as first-class metadata while analyzing every decodable source
quickly and correctly.

### Work

1. Validated libebur128 streaming analysis at the documented ReplayGain target.
2. Track/sample peak, optional true peak, correct album programme reducers,
   and explicit high-rate policy.
3. Track, selection-as-album, release-aware, and `tkfmt-1` grouping modes.
4. Resource-aware parallel FFmpeg decode graph with bounded codec threads.
5. Result grid integrated with metadata workflows, incomplete-album warnings,
   retry, preview, and quiet presets.
6. Safe per-format embedded mappings plus sidecar fallback.
7. Local playback gain modes, preamps, and peak-based clipping prevention.

### Exit criteria

- Every decodable fixture can be scanned regardless of tag writability.
- Album loudness is measured as one programme, never averaged from track gain.
- Results cannot be applied to a changed source revision.
- Embedded and sidecar results round-trip with provenance and interoperate with
  independent players where a standard mapping exists.
- Multicore scanning scales without starving playback, interaction, or storage.

## M8 — Parallel converter, resampler, and organized output

### Objective

Convert whole lists or selections quickly into predictable destinations with
no partial output. The destination is plain filesystem output — it may be an
MPD music root, but Trackbench speaks no MPD protocol; requesting a
database update afterwards is a job for any MPD client, including Trackknife.

### Work

1. Versioned FFmpeg encoder/container presets and capability probing.
2. Destination modes: mirror source-root structure, choose a useful preset, or
   evaluate a `tkfmt-1` relative path below an explicit root.
3. High-quality resampling, channel/bit-depth policy, dither, optional DSP and
   ReplayGain application, metadata/artwork mapping, and output post-scan.
4. One-output-per-track, grouped/cue-aware, and merge modes.
5. Bounded decode/encode scheduling, progress, cancellation, retry, atomic
   publication, verification, and conflict preview.
6. Copy/move/converted publication below a configured root with explicit
   destination preview, staged verification, and atomic commit. Move deletes
   originals only after verified publication.

### Exit criteria

- A preset fully reproduces codec, processing, naming, mapping, and concurrency
  choices with backend versions recorded.
- Mirror mode preserves the source hierarchy relative to a declared source
  root and rejects items outside it visibly.
- Cancellation/failure never damages existing destinations or publishes a
  partial file.
- Signal-changing operations remove or rescan stale ReplayGain metadata.
- Parallel work improves throughput without harming playback or UI budgets.
- Publishing into an MPD music root works with plain filesystem access and no
  protocol dependency.

## M9 — Melody endpoint and advanced listening (MPD client)

### Objective

Turn the shared local playback engine into a first-class Melody streaming
output for the MPD client and add deeper listening features without
destabilizing it. This milestone reintroduces the shared `audio`/PipeWire
libraries into the MPD client as a server-controlled output, not as local file
playback.

### Work

1. Implement the current Melody agent protocol behind a versioned adapter,
   including registration, queue sync, play/preload, seek, volume, ReplayGain,
   keepalive, reconnect, and playback-clock reporting.
2. Stream original or negotiated/transcoded server media when a shared local
   root is unavailable; prefer safe direct access when configured.
3. Gapless preload and reconnect/resume behavior compatible with Melody's
   primary/multi-output rules.
4. Add advanced playback orders, optional DSP presets, MPRIS, media keys,
   notifications, and persisted statistics as separately gated capabilities.

### Exit criteria

- Trackknife appears as an online/offline Melody output and survives daemon,
  network, and application reconnect scenarios without double advancement.
- Queue/version synchronization and primary-output clock rules pass fixtures
  derived from `../melody`.
- Direct-file and streamed playback select the correct source and produce the
  same visible track identity.
- Endpoint work does not regress stock MPD client behavior.

## M10 — Hardening, packaging, and releases

### Objective

Ship two maintainable Linux applications trusted for listening and collection
work.

### Work and exit criteria

- Transactional settings/database/tab/layout migrations and backup/restore for
  both applications.
- Malformed-server/media fuzzing, sanitizers, fault injection, reconnect and
  long-running stress campaigns.
- Accessibility and keyboard/mouse workflow audit of both applications.
- Complete dependency/license inventory and Qt LGPLv3 compliance.
- Native packaging and/or Flatpak with PipeWire, filesystem, and network access
  tested honestly, per application.
- User manuals, protocol capability diagnostics, preset export, and release
  checklists.
- The packaged MPD client completes connect/browse/search/queue/list workflows
  and the packaged Trackbench completes play/tag/identify/ReplayGain/convert
  workflows without hidden setup or UI stalls.

## Beyond the first releases

Possible later work includes a local-tool-owned library index if direct
filesystem navigation stops scaling, cross-tool integration (opening a mapped
server item in Trackbench, triggering an MPD database update after
publication), autoplaylists and deeper queries, secure CD ripping, internet
radio, plugins, additional DSP, more format adapters, and a
capability-advertised transactional Melody import/upload destination if such a
protocol extension is actually implemented. None should bypass the shared
source, list, job, operation, expression, and performance contracts.
