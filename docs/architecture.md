# Architecture

## Status

Qt 6 Widgets, C++23 with a Qt-free core, the Linux/build baseline, focused
media/storage backends, utf8proc, `tkfmt-1`, the MPD-client-first direction,
the unified MPD/local Trackbench workspace (ADR-0058), Trackbench's versioned composed panel
layouts (ADR-0026), its semantic track-view layouts (ADR-0027), bounded
cue-sheet logical-track planning (ADR-0028), and persistent source-rate buffer
profiles (ADR-0029), and continuous PipeWire device monitoring with conservative
output recovery (ADR-0030), exhaustive container-chapter projection
(ADR-0031), explicit decoder selection plus tracker subsongs (ADR-0032), the
typed local metadata read model (ADR-0033), the staged metadata selection grid
(ADR-0034), bounded in-memory metadata drafts (ADR-0035), and the aggregate
metadata fields workspace (ADR-0036), plus exact ordered metadata value editing
(ADR-0037), the file-selection-driven Properties workspace (ADR-0038), its
dynamic metadata field vocabulary (ADR-0039), and ranked field-name completion
(ADR-0040), background complete Draft projection (ADR-0041), and revalidated
metadata write-plan/conflict preview (ADR-0042), plus the preservation-verified
prepared-copy FLAC text writer (ADR-0043), the journaled native-FLAC
commit/recovery executor (ADR-0044), and the provenance-aware dependent-state
cache transaction (ADR-0045), plus startup recovery presentation and bounded
retained-backup undo (ADR-0046), and cancellable bounded metadata Apply jobs
(ADR-0047), plus versioned previewed metadata transformation chains (ADR-0048),
persisted exact-value chains (ADR-0049), typed first-character metadata
capitalization (ADR-0050), and the persistent automatic tagging transformation
panel (ADR-0051), plus the tabbed tagging workspace and expandable change
preview (ADR-0052), and exact-value matching plus selection-order numbering
(ADR-0053), plus the unified preparation plan and reusable output profiles
(ADR-0054), and versioned output-profile persistence plus pure path planning
(ADR-0055), plus fresh filesystem preflight and a durable file-publication state
machine (ADR-0056), plus journaled same-filesystem file publication and recovery
(ADR-0057), revision-qualified all-occurrence relocation (ADR-0059), linked
same-filesystem undo (ADR-0060), and verified cross-filesystem publication
(ADR-0061), plus revision-qualified active-playback relocation (ADR-0062), are
joined by bounded file-publication Apply (ADR-0063) and typed keep-first
metadata transformation (ADR-0064), plus pasted rule-script translation and
conditional removal (ADR-0065), and explicit semantic/freeform metadata field
identity (ADR-0066); decisions are accepted through ADR-0066.
Backend selection is not a capability claim; protocol and file behavior still
require fixtures and measurements.

## System shape

One repository builds a unified Trackbench workspace over shared internal
libraries. The standalone Trackknife MPD shell remains during migration.

```text
Trackbench: MPD Queue context       Trackbench: Local Queue context
  | typed commands, immutable view models, event subscriptions
Application services
  | MPD session | server library      | local playback | preparation | jobs
Core domain (shared)
  | source refs | metadata/MusicBrainz | list docs | tkfmt | jobs | plans
Adapters (linked per application)
  | MPD | SQLite | Melody agent        | filesystem/tags | FFmpeg | loudness
  |                                    | PipeWire
External/platform
  | MPD or melodyd                     | local files | Linux desktop/audio
```

The UI never owns sockets, decoders, tags, SQL transactions, or file writes.
Adapters never reach into widgets. The core can later serve a CLI without a
second interpretation of domain behavior. Trackbench links both adapters but
routes them only through the active authority; local mutation services reject
MPD selections by construction.

## Primary data flow

```text
Trackbench MPD context:
MPD server --command/idle--> MpdSession --> immutable server snapshots
                                      \--> tab/list and view services --> UI

Trackbench local context:
Local source --> playback: FFmpeg --> gain/DSP --> PipeWire
             \-> tags / MusicBrainz / ReplayGain / organize / convert plans
```

MPD is the primary library and live-queue authority for its context. Trackbench
persists authority-qualified profiles, workspace/list state, presets, jobs, and
journals in SQLite. The compatibility shell retains its existing client state
during migration; neither store becomes a mandatory mirror of the MPD database
or a local library index.

## Suggested modules

### `model`

Qt-free domain types: stable IDs, server/profile identity, MPD URI, raw local
path, remote/local source capability, logical segment, arbitrary ordered
metadata, MusicBrainz identities/credits, technical info, artwork, loudness,
queue occurrence, list item, revisions, and errors.

### `mpd`

`libmpdclient` adapter, connection/authentication, capability discovery,
command serialization, idle monitoring, reconnect generations, protocol-to-
domain projection, queue diffing, browse/search, output extensions, and a fake
server test harness. It contains no widgets and exposes no libmpdclient handles.

### `workspace`

Shared track/list document model. In the client it hosts the live MPD queue,
stored playlists, scratch lists, and saved lists over remote references; in
Trackbench it hosts local working lists over raw paths. Owns dirty state,
persistence, cross-tab operations, duplicate occurrences, snapshot
provenance, and supported action capabilities. The model is distinct from Qt
tab widgets.

### `titleformat`

The immutable versioned `tkfmt-1` parser/compiler/evaluator, dependency
extraction, diagnostics, and corpus. It is pure and is shared by views, sorts,
groups, transformations, and relative destination generation.

### `formats`

FFmpeg probe/decode/encode adapters. A narrow libopenmpt identity adapter
enumerates tracker subsongs that FFmpeg can select but does not enumerate;
FFmpeg remains the PCM decode boundary.

### `audio`

Local playback state machine, source resolver, FFmpeg decoder lifecycle,
gapless scheduling, PCM negotiation, ReplayGain/DSP stages, PipeWire buffer and
device output, and underrun diagnostics. A dedicated Qt-free playback worker
serializes source replacement, bounded decoder production, transport, seek,
PipeWire transitions, drain, and cancellation while publishing immutable
snapshots. The real-time callback performs no blocking I/O, allocation, SQL, or
UI work. Named and exact duration profiles configure future ring allocations
without resizing live RT state. A persistent registry/default-metadata monitor
publishes device generations to the same worker; explicit-target loss pauses
without fallback and reconnects in place when the target returns. Trackbench's
player owns it today; the client reuses it later only as the M9 Melody
endpoint's server-controlled output.

### `preparation`

Trackbench's local intake and capability resolution across metadata, artwork,
loudness, organization, conversion, playback, and publication. It turns a
local selection into typed operation inputs and readiness state without
becoming a canonical local library.

### `metadata`

Rich multi-source documents, MusicBrainz projections, TagLib/format-specific
adapters, the sparse staged-selection projection, field vocabulary and ranked
search, the bounded copy-on-write patch set and worker result projection, and
the physical-source-aware revalidated write-plan preview, plus the native-FLAC
preservation-verified prepared-copy writer. Versioned ordered transformation
chains, their exact add/copy/split/join and first-character capitalization
semantics, pure final-cell preview, and future capture patterns remain in this
module. Saved definitions cross the
`persistence` boundary only as validated declarative data. Each adapter
publishes independent
read/write/artwork/preservation capabilities;
decode or read support never implies write safety. Loudness is displayed beside
tags but remains a typed record with algorithm/provenance.

### `loudness`

libebur128 accumulators, album reducers, sample/true peak, grouping, scan plans,
and typed results. It consumes FFmpeg-decoded PCM independently of tag
writability.

### `operations`

Shared plan/preview/commit/journal framework for tags, artwork, files,
conversion publication, verification, and loudness writes. Mutations acquire
per-source locks and revalidate revisions. The first implementation serializes
native-FLAC sources, journals exact raw paths and recovery identities through a
persistence interface, atomically publishes a verified sibling, retains the old
inode as a backup, requires an idempotent dependent-state commit, and rolls back
or conservatively escalates interrupted states without depending on Qt. A
separate backup lifecycle supports verified atomic-exchange undo, explicit
release, startup maintenance, and restart recovery without conflating backup
retention with publication state. A Qt-free two-worker Apply orchestrator now
consumes only wholly ready plans, serializes progress delivery, stops new
admission on cancellation, and preserves ordered per-source partial results.

### `jobs`

Bounded resource-aware scheduler, dependencies, cancellation, progress,
structured per-item results, retry, history, and UI events. Network sessions,
audio realtime, CPU decode/analysis, encode, local I/O, and mutation have
separate limits rather than one undifferentiated pool.

### `persistence`

SQLite migrations/repositories for profiles, workspace, list documents and
snapshots, presets, jobs, statistics, caches, and operation journals. Writes are
short and transactional; sockets, decode, and filesystem work never occur
inside a database transaction. Verified metadata publication refreshes every
matching local occurrence and a raw-path source cache in one idempotent
transaction; provenance keeps logical-track overlays separate, and the source
cache prevents a delayed ordinary workspace save from restoring stale fields.
The operation journal also guards retained-backup state and unique reverse
operation identities for crash-recoverable undo. Apply first persists a
UI-captured workspace snapshot on this worker so a newly opened occurrence
cannot race the ordinary save debounce.

### `ui`

Qt Widgets presentation split into a shared component library (virtualized
track views, tab workspace, transport row, command palette, toasts, settings,
shortcuts, and the versioned panel- and track-view-layout models/validators)
plus one primary Trackbench shell: authority-bound MPD connection,
server-browser/search, queue tabs, and outputs alongside the composed-panel
renderer, folder navigation, local list tabs, tag grid, previews,
metadata-operation history and reconciliation evidence, and job center. The
compatibility Trackknife shell reuses the MPD components during migration.
Panel layout
owns only placement and sizing of registered instances, never application
state. ADR-0052 hosts each captured metadata Properties task as a temporary
protected tab in the Track Lists surface while excluding it from durable list
documents. Track-view presentation separately owns semantic column placement and
group geometry, never queue/list occurrence state. UI consumes C++
`QAbstractItemModel` and controller objects; no
`QQuickWidget` hybrid.

## Identity and source records

```text
ProfileId       stable MPD connection identity
RemoteTrackKey  ProfileId + exact MPD URI + optional segment identity
QueueItemKey    ProfileId + MPD song ID + server queue generation
LocalSourceId   stable internal UUID + observed filesystem identity
ListId          stable Trackknife tab/list identity
JobId           stable job identity
Revision        server/list/source revision appropriate to the authority
```

An MPD URI and local raw path are different representations even when a profile
root maps between them. A remote snapshot may outlive connectivity. A local
revision uses filesystem identity/size/mtime plus hashes where needed for safe
mutation.

## Conceptual persistence

```text
mpd_profiles(id, name, endpoint, secret_ref, local_root, reconnect_policy)
workspace_panels(id, type, config, schema_version)
lists(id, kind, name, profile_id?, dirty, revision, config)
list_items(list_id, ordinal, source_kind, source_ref, logical_ref?, segment?, snapshot)
view_presets(id, name, version, definition)
local_sources(id, raw_path, fs_identity, size, mtime, revision, availability)
local_metadata(source_id, field, ordinal, value, provenance, revision)
loudness(source_id/segment, grouping, gains/peaks, algorithm, revision)
jobs(id, kind, state, summary, resumable_payload)
operation_journal(id, plan, steps, state, recovery_payload)
metadata_operation_backups(operation_id, state, undo_id?, timestamps, failure?)
```

The live MPD queue is not persisted as a second authoritative list. Its latest
snapshot may be cached for fast/offline display, clearly marked stale. Scratch
and named Trackknife lists are authoritative application data.

## Events and consistency

Typed events carry IDs/revisions or small immutable snapshots:

```text
MpdConnectionStateChanged / MpdCapabilitiesChanged
MpdStatusChanged / MpdQueueChanged / MpdDatabaseChanged
MpdStoredPlaylistsChanged / MpdOutputsChanged
TrackListChanged / ActiveListChanged
LocalSourceChanged / MetadataChanged / LoudnessChanged
MpdPlaybackStateChanged / LocalAuditionStateChanged
JobProgress / JobFinished / OperationNeedsReconciliation
```

Session generations reject stale network results. Coalesce server and job
bursts so changes do not create unbounded UI redraws. Live-queue conflicts
refresh server truth; local list edits remain transactional and undoable.

## Operation lifecycle

```text
Draft -> Resolve inputs -> Plan -> Previewed -> Revalidate -> Journal
      -> Execute -> Verify -> Commit state -> Complete
                                \-> Partial / Needs reconciliation
```

Plans are immutable after preview. Capability resolution reports remote-only,
local, mapped-local, writable, and decodable sources before offering actions.

## Concurrency rules

- The UI loop receives only small immutable messages.
- Each active MPD session has bounded serialized command and idle workers.
- Non-idempotent protocol mutations are not automatically replayed after an
  ambiguous disconnect.
- PipeWire real-time work has the highest isolation and shares no blocking lock
  with UI, network, decoder, or operation workers.
- CPU-heavy scans/conversions use bounded pools and account for FFmpeg's own
  codec threads.
- Only one mutation holds a physical source lock; pending writers do not starve.
- Cancellation crosses every backend boundary and updates visible state quickly.

## Backend decisions

- **MPD:** dynamically linked `libmpdclient >= 2.22`, generic pair parsing for
  unknown and advertised extension fields, no handles outside the adapter.
- **Decode/encode:** FFmpeg libraries, never an `ffmpeg` subprocess for playback.
- **Metadata:** TagLib plus format-specific adapters. The generic property
  reader is active; native-FLAC text has preservation-proven prepared-copy and
  headless journaled commit plus bounded Apply support (ADRs 0043–0047).
  ADR-0048's versioned transformation chains stage through the same draft and
  write-plan boundary; ADR-0049 persists validated definitions and adds exact
  add/copy/split/join actions, while ADR-0050 adds typed first-character
  capitalization. ADR-0051 composes checked saved definitions into a temporary
  draft immediately before the same immutable write-plan boundary. ADR-0053
  adds complete-value exact remove/replace and captured-file-order numbering
  without folding grouping or capture grammar into schema 1. ADR-0064 adds a
  bounded Unicode-scalar keep-first action without assigning it date parsing
  semantics. ADR-0065 translates a bounded Picard-style paste subset into
  ordinary typed actions and adds versioned conditional field removal without
  adding mutation to `tkfmt-1`. ADR-0066 permits semantic aliasing only through
  explicit adapter tables, exposes other properties as independently mutable
  freeform native fields, and carries exact-native imported deletion through
  preview, write, journal, and recovery. Other format and artwork writes remain
  disabled until their real preservation tests pass.
- **Loudness:** libebur128 behind Trackknife-owned ReplayGain policy.
- **Preparation:** ADR-0054 composes tag persistence, qualified ReplayGain
  storage, and filesystem rename/move into one immutable per-source review and
  recoverable Apply boundary. Versioned relative output layouts remain separate
  from explicit raw-path destination roots and are reusable by conversion.
  ADR-0055 persists those contracts in migration 13 and adds a bounded Qt-free
  path planner over final metadata, captured source revisions, and an explicit
  filesystem snapshot. It owns lexical mapping and collision diagnostics only;
  ADR-0056 adds a no-symlink live preflight and classifies same-filesystem rename
  versus cross-filesystem copy without mutation. Migration 14 durably separates
  target preparation/publication, dependent-state commit, and source removal.
  ADR-0057 qualifies locked descriptor-relative no-replace rename, exact
  rollback, and idempotent startup recovery; ADR-0059 supplies concrete guarded
  dependent path state, ADR-0060 adds linked same-filesystem undo, and ADR-0061
  adds bounded metadata-preserving verified cross-filesystem copy, no-replace
  publication, source removal, and recovery. ADR-0062 adds a serialized exact-
  revision audio-binding barrier and compensates it if the durable list/cache
  transaction fails. ADR-0063 adds ordered bounded file batches, fresh per-
  source admission, monotonic progress, partial results, cancellation, and
  shared-directory topology coordination. Cross-filesystem undo and UI
  exposure remain to qualify.
- **State:** SQLite with explicit reversible development migrations, including
  normalized ordered schema-1 metadata transformation definitions owned by the
  serialized persistence worker, including their automatic tagging policy
  flags, exact matching payloads, numeric arguments, and dialect-qualified
  conditions and exact-native removal identity (ADRs 0049–0053 and 0064–0066),
  plus separately versioned output-layout and raw destination profiles
  (ADR-0055) and file-publication recovery evidence (ADR-0056).
- **Local output:** direct PipeWire with a narrow fallback-capable interface.

## Implementation sequence

Steps 1–5 below were completed while establishing the separate authority
backbones. ADR-0058 now places them in one primary Trackbench workspace without
combining their queues or controllers:

1. ~~MPD domain snapshots, session, browse/search, and reconnect tests.~~
2. ~~Persistent queue/list tabs and the polished client workspace.~~
3. ~~FFmpeg decode, bounded playback core, PipeWire output, local ingestion.~~
4. ~~Shared-library build restructure and the `trackbench` executable.~~
5. ~~Migrate local playback/ingestion into Trackbench and retain separate MPD
   and local controllers.~~
6. Finish the Trackbench tag grid and mutation framework, including the
   combined metadata/path review and Rename/Move exposure; then add
   MusicBrainz providers.
7. Parallel ReplayGain, then converter/resampler and organized output.
8. Melody endpoint in Trackbench's MPD authority; hardening, compatibility-shell
   migration, and packaging.

Do not build a local library index, plugin SDK, or elaborate theme system
before both authority experiences are excellent.
