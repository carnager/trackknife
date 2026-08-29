# Feature matrix

This matrix describes intended scope rather than claiming completion. M0
through M3 are complete; M4 — the ADR-0025 application split plus Trackbench's
playback workspace — is active. ADR-0009 records the MPD-client-first product
direction, ADR-0010 defines queue/list tabs, ADR-0020 rejects a mixed playback
queue, and ADR-0025 splits the product into the **Trackknife** MPD client and
the **Trackbench** local workstation over shared libraries. Milestone numbers
follow the restructured `MILESTONES.md` (tagging M5, MusicBrainz M6,
ReplayGain M7, converter M8, Melody endpoint M9, release M10).

## Foundations

| Capability | Status | Required behavior |
| --- | --- | --- |
| C++23/Qt 6 skeleton | Complete | Qt-free core, strict builds/tests, virtualized UI spike, measured budgets. |
| `tkfmt-1` | Complete | Versioned pure compiled expressions shared by views, sorts, groups, transforms, and paths. |
| Source/reference model | M2 (complete) | MPD URI, remote snapshot, optional mapped raw path, and local-only source stay distinct. |
| MPD session | M2 (complete) | Command/idle workers, typed snapshots and query payloads, selective refresh, bounded autonomous reconnect, ambiguous-mutation non-replay, transport/output commands, stable-ID queue primitives, versioned queue diffs, bounded search/browse reads, and exactly-once stored-playlist reads/mutations exist. |
| Working-list model | M3 (complete) | Explicit live queue, server playlist, scratch, and saved kinds exist. Lists may retain remote and local references as working memory, but no mixed playback-queue kind is planned. |
| Job system | M4–M7 | Bounded resource pools, progress, cancellation, retry, per-item errors. |
| Metadata abstraction | M2/M5 | Ordered arbitrary values, MusicBrainz identities/credits, art, loudness, provenance. |
| Safe mutation engine | M5 | Plan/preview/revalidate/journal/verify shared by tags, files, loudness, conversion. |

## Priority 1: MPD client (Trackknife) and local playback groundwork

| Area | Capability | Milestone | Required behavior |
| --- | --- | --- | --- |
| Connection | Profiles and reconnect | M2–M3 | TCP/Unix socket, session-only password, visible reconnect state, bounded backoff, generation-safe results, explicit first-run setup, multiple saved profiles, one-click switching, and selected-profile startup auto-connect exist. Secret-service-backed authenticated auto-connect remains deferred. |
| Protocol | Capabilities and idle | M2 | Discover commands/tagtypes; refresh affected subsystems; preserve unknown fields. |
| Library | Browse and search | M2–M4 | The Server tab is a real expandable library tree backed by a versioned editor for explicit `tkfmt-1` grouping, label, and sort expressions. Its default is album artist → chronological album (undated last) → disc only for multi-disc releases → tracks. Root tags load first; expanding one artist performs one bounded exact MPD query and refuses silent truncation above 10,000 tracks. Tree selection never replaces the center workspace: the current or hovered node reveals append/insert/replace controls, and a matching context menu includes working-list destinations. A permanent recursive filter strictly shrinks the tree to matching rows (labels for branches, labels plus descriptive tags for tracks; matched rows never drag their subtree in), auto-expands the results, and a debounced, bounded server-side search additionally reveals unloaded artist roots whose descendants match, auto-fetching them only while few roots remain visible. Two-line level-specific rows show artist placeholders and album counts, dated album covers with track count/duration, and numbered tracks with duration. Lazy roots retain arrows through filtering and defer first expansion until children arrive so it animates. Covers load serially without request bursts or UI-thread decode. Non-recursive folders and stored playlists remain adjacent actions. Live search uses an anchored non-modal panel that preserves the active tab; album results load at most 32 covers serially, while committed searches remain independent tabs. Richer query syntax remains deferred. |
| Metadata | MusicBrainz organization | M2–M3 | Preserve IDs/sort fields; release-aware grouping with deterministic fallback. |
| Playback | Remote transport | M2–M3 | Play/pause/stop/seek/prev/next, volume, repeat/random/single/consume, and advertised ReplayGain status/control cross the worker/controller boundary with immediate UI state and passed live compatibility validation. |
| Outputs | MPD outputs | M2–M3 | Additive enable/disable/toggle and output attributes. |
| Outputs | Melody extensions | M2–M3 | Online/primary/format/bitrate and advertised exclusive switch without stock-MPD regression. |
| Queue | Live MPD queue | M2–M4 | Stable song-ID play/add/delete/move/priority, ordered multi-select append/Add next command lists, duplicate occurrences, album-header group selection with bounded serial cover loading, bounded multi-select deletion/crop/priority, native clear, multi-selection drag reorder as one stable-ID command list, versioned reconciliation, and selection-preserving row changes exist. Priority is capability-gated and preserved in typed snapshots. M4 follow-up must render nonzero priority in the queue and revalidate the native multi-selection drag/drop gesture, drop affordance, and confirmed reorder end-to-end. Recoverable stale-ID rejections trigger authoritative queue/player reconciliation and an explicit non-modal conflict notice; richer concurrent-edit explanations remain. |
| Lists | Queue/list tabs | M3 | Live queue, stored playlists, search results, persistent scratch tabs, and named working lists have distinct semantics. Create/rename/pin/duplicate/reorder/close, dirty protection, keyboard switching, exact item edits, copy/move menus, cross-tab drag, crop, sort, reverse, randomize, and deduplicate are wired with mouse/keyboard actions. |
| Persistence | List documents | M3 | Three reversible SQLite migrations transactionally preserve scratch/saved kind, tab order, duplicate MPD/local references, raw path bytes, repeated snapshot fields, dirty/pinned state, remote profile identity, multiple connection profiles, and binary Qt track-view presets. A serialized worker owns SQLite; startup loads and debounced saves do no SQL work on the UI thread, while shutdown performs a durable worker-thread flush. |
| Playlists | MPD stored playlists | M2–M3 | Typed discovery and metadata-rich loading open reusable, closable server-playlist tabs. Exactly-once create/save, queue-load, append/insert, multi-entry remove, reorder, clear, rename, and delete operations cross the adapter/session boundary. Advertised capabilities gate their tab/menu actions and accepted edits reload authoritative contents; richer dirty/conflict/recovery presentation remains. |
| UX | Complete default workspace | M3–M4 | Pure Qt Widgets shell has the tabbed Library pane left. Per ADR-0025 the client's Library pane is MPD-only (server browsing/playlists); the Local Folders surface and filesystem navigation moved to Trackbench in the M4 split. Live/scratch/named/stored/search document tabs, transient live search, compact Cantata-informed player, grouped queue rows, context totals/actions, non-blocking toasts, and native docks ship. Capability-aware row and tab context menus reuse registered actions and gates. |
| UX | Command palette/shortcuts | M3 | Major registered actions are searchable, runnable, conflict-checked, and assigned persisted shortcuts; tab/search/dialog focus rules and accessible names have regression coverage. |
| Performance | Fluid large/remote UI | M3 | Virtualized bounded models, cancellable network requests, server-windowed continuation, worker-thread artwork decode, bounded art cache, and p50/p95 interaction measurements are implemented. Live network latency remains server-dependent. |
| Local sources | Preparation intake | M4 (partial) | File/folder/drop/recent/CLI entries use one cancellable raw-path discovery path and enter persistent working lists without becoming MPD members. The **Local Folders** tab persists chosen raw-byte roots, lazily enumerates direct children off the UI thread, losslessly presents invalid UTF-8, and omits directory symlinks. Per ADR-0025 this entire surface now lives in Trackbench. Tree-fetch cancellation, root management, FFmpeg probing, readiness/source badges, and local actions remain. |
| Local audition | FFmpeg + PipeWire | M4 (partial) | Qt-free FFmpeg probe and cancellable bounded source-rate float decode pass real PCM WAV fixtures. Sample-accurate seek/ranges and repository-owned AAC/M4A, LAME MP3, Opus/Ogg, and Ogg Vorbis timestamp fixtures prove exact manual gapless trim and a contiguous logical PCM timeline even when container frame timestamps overlap or leave gaps. The bounded playback core proves transport, SPSC render, zero-fill underrun recovery, and exact consumed-sample position. The direct PipeWire adapter proves exact-rate mono/stereo float negotiation and an RT-safe render boundary. ADR-0021 now serializes source replacement, scheduled decoder production, transport, seek, drain, quiescence, and cancellation on one dedicated worker; duration-based buffers scale at the actual source rate. ADR-0023 adds list progression (previous/next and end-of-track auto-advance through the originating working list's local rows), and ADR-0024 adds perceptual stream volume plus bounded sink enumeration with in-place device retargeting. The superseded ADR-0022 domain-chip transport binding is removed during the split; this whole engine becomes Trackbench's first-class player. Broader format coverage, device-clock projection, and hotplug remain. |
| Playback ownership | Separate applications | M4 | MPD remains shared server state controlled by multiple clients and advertised outputs/agents; the Trackknife client is purely its client. Local playback lives in Trackbench, a separate process with its own transport; there is no mixed queue, no backend handoff, and no cross-process side effects (ADR-0025). |

## Priority 2: Trackbench metadata, MusicBrainz, and ReplayGain

| Area | Capability | Milestone | Required behavior |
| --- | --- | --- | --- |
| Tagging | Fast field grid | M5 | Type-to-add/fuzzy fields, direct keyboard movement, rectangular paste, saved layouts. |
| Tagging | Multi-file states | M5 | Common/mixed/missing/partial values and per-item drill-down. |
| Tagging | Arbitrary/multi-value data | M5 | Preserve ordering, spelling, MusicBrainz data, qualifiers, and unknown native structures. |
| Tagging | Derived values/chains | M5 | Previewed declarative chains, `tkfmt-1`, numbering, and capture patterns. |
| Metadata | Provider proposals | M5 | Internal provider boundary returns proposed values, MusicBrainz identities, artwork references, provenance, and confidence into the staged preview. A public plugin ABI remains deferred. |
| Metadata | MusicBrainz identification | M6 | Online release/recording matching, identifier and artwork proposals with provenance/confidence, honest rate limiting/caching, explicit cancellable network operations, optional AcoustID evaluation. |
| Artwork | Management | M5 | Typed embedded/external art, replace/remove/export, preservation tests. |
| Files | Rename/move/copy | M5 | Relative expressions, collision preview, safe roots, journal, list-reference updates. |
| Loudness | Universal parallel scan | M7 | Every decodable source; bounded FFmpeg/libebur128 jobs; track/album and peaks. |
| Loudness | Metadata integration | M7 | Review beside tags; embed safely or use explicit sidecar/database fallback. |
| Loudness | Playback application | M7 | Track/album modes, preamps, clipping prevention, source-aware behavior. |

## Priority 3: converter and import

| Capability | Milestone | Required behavior |
| --- | --- | --- |
| Codec presets | M8 | Versioned FFmpeg capabilities, useful defaults, missing encoder diagnostics. |
| Destination mirror | M8 | Preserve source structure relative to an explicit root. |
| Destination expressions | M8 | Presets or `tkfmt-1` relative paths with complete sanitization/conflict preview. |
| Signal pipeline | M8 | Resample/channel/bit depth/dither, optional DSP/gain, bounded parallelism. |
| Metadata/art transfer | M8 | Capability-aware mappings and warnings; no stale loudness after signal change. |
| Publication/verification | M8 | Temp sibling, atomic publish, cancellation cleanup, optional output scan/check. |
| Organized publication | M8 | Previewed copy/move/converted publication below a configured accessible root, containment revalidation, verification, and atomic commit. A destination may be an MPD music root reached by plain filesystem access; Trackbench speaks no MPD protocol, and requesting a database update afterwards is a job for any MPD client (ADR-0025). |

## Priority 4 and later

| Capability | Status | Notes |
| --- | --- | --- |
| Melody playback endpoint | M9 | Register, stream/direct source, queue sync, preload, report clock/state. |
| MPRIS/media keys/notifications | M9 | Linux desktop integration after playback ownership is proven. |
| DSP graph | M9/later | Versioned presets; bypass must not alter samples. |
| Trackbench local index | Deferred | Add only if direct filesystem navigation plus working lists stop scaling. |
| Autoplaylists/deep query | Deferred | Server search and `tkfmt-1` sorting first. |
| Online MusicBrainz provider | M6 | First metadata-provider implementation; explicit network capability whose proposals still pass through normal preview/write safety. |
| Melody remote import | Proposal | No protocol extension exists today. If Melody later advertises transactional upload/import, implement it as an optional destination for the existing import plan. |
| Plugin API | Deferred | Derive from proven built-ins rather than freezing an early ABI. |
| CD ripping/internet radio | Deferred | Separate product and correctness designs. |

## Format-support dimensions

Never publish one undifferentiated “supported formats” list. Track local formats
independently:

| Dimension | Meaning |
| --- | --- |
| Probe/decode | Produce technical data/PCM for local playback, scanning, conversion, verification. |
| Seek/gapless | Accurate duration, sample seeking, delay/padding/segment behavior. |
| Read metadata | Text, arbitrary values, MusicBrainz, binary/art, chapters/cues, loudness. |
| Write metadata | Safely update intended data while retaining unknown structures and audio essence. |
| ReplayGain embed | Interoperable embedded mapping exists and is tested. |
| Sidecar loudness | Explicit fallback when embedding is unsafe/unavailable. |
| Convert output | Encoder/muxer exists with versioned options. |
| Verify | Decode/container/checksum behavior is specified. |

The initial local matrix should cover MP3, FLAC, Ogg Vorbis, Opus, MP4/M4A,
WavPack, WAV/RF64, AIFF, and common cue/subsong cases before expanding claims.
FFmpeg decoding alone never implies safe tag writing or exact gaplessness.
