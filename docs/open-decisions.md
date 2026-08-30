# Open decisions

Accepted foundations are recorded in ADRs through 0067. ADR-0058 supersedes
ADR-0025's permanent process split: Trackbench is the primary workspace and
hosts authority-bound MPD Queue and Local Queue tabs. The active primary tab
switches the server-library/local-folders sidebar, MPD/PipeWire output selector,
transport controller, row type, and available commands. The queues never mix,
and local operations remain unavailable to MPD rows. The standalone Trackknife
executable is a compatibility shell while committed search/stored-playlist tabs,
profile ownership, and packaging entry points finish migrating.

## Resolved for M2–M3

1. Credentials remain session-only until a desktop secret-service adapter is
   selected; ordinary settings and SQLite never receive passwords.
2. Browse/search pages and artwork are bounded memory caches. Only explicit
   working-list snapshots survive restart, so the MPD authority does not create
   a competing local library index.
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
ADR-0021 fixes the serialized playback worker, ADR-0023 list progression,
ADR-0024 volume/device selection, ADR-0029 named and exact buffer policy, and
ADR-0030 persistent sink/default monitoring with strict explicit-target
pause/recovery, and ADR-0031 exhaustive container-chapter projection through
the shared logical segment model. ADR-0032 adds typed decoder selection,
bounded tracker-subsong enumeration, and an explicit opt-in policy for
alternate container streams. These contracts moved into Trackbench unchanged;
ADR-0058 reuses the distinct MPD and local controllers behind the active
primary tab rather than restoring the ADR-0022 domain chip or a mixed
transport.

## Resolved M5 read, inspection, and draft foundation

ADR-0033 fixes the Qt-free ordered metadata document, deterministic canonical
lookup, provenance precedence, initial MusicBrainz projection, raw-path source
revision, and conservative read-only TagLib property boundary. Trackbench list
snapshots cache effective ordered values but are not mutation authority. Exact
format write mappings, artwork/native-object preservation, sidecars, and
journaling remain open M5 decisions and capability work. ADR-0034
adds the bounded sparse selection union, exact definitions for common, mixed,
missing, and partial fields, and the non-modal Properties workspace with
per-item exact-value/provenance inspection. ADR-0035 adds bounded sparse
replacement/removal patches, deterministic result-state projection, direct
scalar grid editing, bounded undo/redo, and explicit draft discard. These
drafts intentionally resolve no write claim. ADR-0036 makes the compact
Fields/Original/Draft projection primary, and ADR-0037 adds structured exact
ordered value editing without delimiter parsing. ADR-0038 supersedes the
separate Tracks drill-down with a file list above that table: file selection is
now the single source of individual versus bulk edit scope, with arbitrary
subset summaries projected outside the UI thread. ADR-0039 adds a dynamic,
copy-on-write session field vocabulary plus explicit Add field and Remove field
commands without changing that scope. ADR-0040 adds bounded deterministic field
completion while keeping arbitrary names open, ADR-0041 adds the complete
cancellable in-memory Draft projection, and ADR-0042 adds the physical-source-
aware revalidated write-plan/conflict preview. ADR-0043 proves preservation-
verified native-FLAC text preparation, ADR-0044 adds its journaled commit and
recovery executor, ADR-0045 adds the idempotent all-occurrence source-cache
transaction, and ADR-0046 makes startup recovery evidence visible while adding
bounded retained-backup undo/release. ADR-0047 adds the explicit cancellable
multi-source Apply job, ordered partial results, and fresh-preview retry.
ADR-0048 adds the versioned ordered transformation-chain boundary, its first
literal/remove/per-value/`tkfmt-1` actions, immutable final preview, and one-
transaction draft staging. ADR-0049 adds normalized saved-chain persistence
through the serialized worker plus exact append/copy/split/join semantics.
ADR-0050 exposes first-character Unicode capitalization as a typed saved action
whose remainder-preserving semantics are distinct from title case.
ADR-0051 exposes the saved catalog directly in a persistent checkable tagging
side panel. Checked definitions are composed in displayed deterministic order
against a temporary draft copy before the immutable write plan, so repeated
preview cannot compound non-idempotent actions and Apply remains explicit.
ADR-0052 makes Properties a temporary protected workspace tab instead of a
separately sized window and moves transformation file/step diagnostics beneath
expandable Field/Old/New change rows without changing exact preview semantics.
ADR-0053 fixes case-sensitive complete-value remove/replace semantics and
bounded consecutive numbering in captured file order while explicitly
deferring group resets and `TOTALTRACKS`. ADR-0064 adds exact bounded Unicode-
scalar prefix extraction as an exposed saved action without treating it as a
date parser. ADR-0065 adds a bounded Picard-style paste translator that
discards source after generating typed rules, plus dialect-qualified
conditional removal; it does not settle full chain interchange or external
script compatibility. ADR-0066 rejects separator-derived tag aliases: only an
explicit format mapping creates a semantic field, while freeform native fields
remain independently visible and mutable. It does not settle the typed native
identity required by future ID3v2 or MP4 writers. ADR-0054 fixes the shared
preparation-plan shape and separates reusable relative output layouts from explicit
destination roots so rename/move, ReplayGain, and conversion can share one
review vocabulary. ADR-0055 fixes the persisted schema-1 profile contracts,
exact `linux-v1` sanitization, and the pure lexical planner's revision, alias,
containment, and collision behavior against an explicit observation snapshot.
ADR-0056 fixes no-symlink fresh preflight, actual same/cross-filesystem
classification, and the numeric durable publication boundaries through
dependent-state commit and source removal. ADR-0057 fixes locked
descriptor-relative same-filesystem no-replace publication, rollback, and
startup replay. ADR-0059–0063 add the concrete dependent path transaction,
same-filesystem undo, cross-filesystem execution, active-playback relocation,
and bounded multi-source Apply. ADR-0067 exposes path-only Rename/Move through
one final-metadata/path review, bounded Apply, both startup recovery paths,
unified operation history, and same-filesystem undo; changed content plus paths
remains blocked until direct destination-artifact publication exists. Cross-
filesystem undo, portable/custom sanitization and Unicode normalization,
grouped numbering, richer match dialects, native chain interchange, other exact
format writers, artwork mutation, and sidecars remain open M5 decisions and
capability work. ADR-0068 separately fixes the `tkcapture-1` grammar, bounded
ambiguity policy, four source kinds, multi-target chain behavior, and schema-20
saved action.

## Needed to finish the unified workspace migration

1. Migrate committed search and stored-playlist tabs into Trackbench without
   weakening MPD/local authority selection.
2. Settle connection-profile ownership and migration between the compatibility
   shell and Trackbench.
3. Decide the compatibility shell's packaging and eventual launcher/removal
   story only after equivalent Trackbench surfaces are proven.

## Needed before M5–M8

1. Sidecar location/format and precedence relative to embedded metadata.
2. Portable/custom filename sanitization and Unicode normalization policy;
   `linux-v1` is fixed by ADR-0055.
3. ReplayGain true-peak and Opus output-gain/storage policy.
4. Initial exact read/write/preservation claims per local format.
5. Converter's shipped codec/device presets, resampler quality settings, and
   source-root inference UX.
6. MusicBrainz web-service rate-limit/cache policy and whether AcoustID
   fingerprinting earns its dependency (M6).
7. Shipped destination-profile defaults, including whether an accessible MPD
   music root earns a convenience preset.

## Deferred

- Whether a local-tool-owned library index is ever needed ("no database for
  now, maybe later").
- Cross-authority conveniences: opening a mapped server item as an explicit
  local source and offering an explicit MPD database update after publication.
- Deeper query/autoplaylist language.
- Plugin ABI/distribution.
- A transactional Melody import/upload protocol and destination adapter; no
  such extension is currently claimed.
- CD ripping, internet radio, streaming-service integration, and non-Linux
  ports.
