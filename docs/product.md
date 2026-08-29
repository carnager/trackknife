# Product definition

## One sentence

The Trackknife project builds two fast, modern Qt 6 applications that share
one codebase and visual language: **Trackknife**, a keyboard-fluent tabbed
client for MPD and Melody, and **Trackbench**, a foobar2000-inspired
workstation for local files with
playback, album grouping, tagging, MusicBrainz support, ReplayGain,
conversion, and resampling.

## Product thesis

Linux has mature music servers but lacks a current native desktop client with
the speed, keyboard fluency, and dense information that made foobar2000 and
Cantata valuable — and it equally lacks a current native workstation for
preparing music files. These are two different jobs with different primary
objects: server queue occurrences versus files on disk. Per ADR-0025 they are
two applications, not two panes.

The MPD client is a joy to use every day: connect, browse, search, listen,
choose outputs, and shape queues without friction. Its basic form is finished
and validated against stock MPD and Melody.

Trackbench takes new music from download or rip to a well-organized
collection: audition it, group it by album, identify and tag it with
MusicBrainz support, analyze ReplayGain, convert or resample it, and publish
it into an organized destination — which may be an MPD music root, reached by
plain filesystem access, never by protocol. The key inspiration is still
foobar2000, but with the tabbed working-list approach and a primary focus on
performance and user experience. It maintains no library database for now;
it grows from direct filesystem navigation, and an index may be reconsidered
later if real use demands it.

Both applications share internal libraries — the `tkfmt-1` expression engine,
the FFmpeg decode boundary, the bounded playback core, the PipeWire adapter,
persistence infrastructure, and reusable Widgets components — so they look and
feel like siblings while never depending on each other at runtime.

## Priorities

### Priority 1: delightful MPD client (delivered)

- Reliable profiles, authentication, reconnect, capability discovery, and
  visible connection state.
- Fast server-library browsing and search with MusicBrainz-aware sorting and
  grouping where metadata permits it.
- Responsive transport, now playing, seek, volume, playback modes, ReplayGain
  mode, and output selection.
- Excellent live-queue editing using stable song IDs and incremental updates.
- Multiple queue/list tabs for scratch work, named lists, stored playlists, and
  the live server queue.
- A polished default Qt 6 Widgets workspace equally efficient with mouse or
  keyboard.

Melody-specific enhancements remain capability-driven and small. The client
targets standard MPD behavior; Melody's streaming outputs are exposed through
the output UI when advertised. The client contains no local file playback.

### Priority 2: local playback workspace

- First-class local playback through the proven FFmpeg core and PipeWire
  output: gapless transitions, exact seeking, per-list progression, volume,
  and device selection.
- Album-grouped, dense, high-performance tabbed track lists over direct
  filesystem navigation — no import step, no library scan.
- The same tab, shortcut, command-palette, and persistence ergonomics as the
  MPD client.

### Priority 3: metadata, MusicBrainz, and ReplayGain

- A non-modal, spreadsheet-like tag workspace built for many tracks and fields.
- Type-to-add fields, fast fuzzy field lookup, direct keyboard navigation,
  multi-cell paste, saved field layouts, and bulk transformations.
- Arbitrary ordered multi-value metadata and complete MusicBrainz identifier/
  sort metadata preservation.
- Online MusicBrainz identification and metadata proposals with provenance and
  confidence, entering the staged preview as explicit network operations.
- Correct track and album ReplayGain analysis, sample and optional true peak,
  review before writing, and sidecar fallback when a safe embedded mapping is
  absent.
- Previewed, conflict-detecting, recoverable file and metadata mutation.

### Priority 4: converter and organized output

- FFmpeg-backed bounded parallel conversion with useful codec/device presets
  and high-quality resampling.
- Preserve the source directory structure or generate a relative destination
  using a preset or `tkfmt-1` expression.
- Channel/bit-depth policy, dither, metadata/artwork mapping, optional DSP,
  verification, and output ReplayGain.
- Complete path/conflict preview and atomic publication, including into an
  MPD music root by plain filesystem access.

### Priority 5: Melody output endpoint (MPD client)

Trackknife can later register as a Melody streaming endpoint and play the
server-provided stream through the shared local audio engine. This is
deliberately after both applications' backbones are proven.

## Established requirements

- Two native Linux Qt 6 Widgets applications; Wine is not part of the product
  story.
- Neither application depends on the other at runtime; shared behavior lives
  in shared internal libraries.
- Standard MPD compatibility before optional Melody extensions.
- MPD is authoritative for its database, current queue, stored playlists,
  transport, and outputs. The MPD client is one of potentially several
  connected clients and reconciles server truth.
- Trackbench speaks no MPD protocol and maintains no library database for
  now; it never implies MPD membership for a local file.
- Queue/list tabs remain first-class persistent work surfaces in both
  applications.
- Local paths remain raw OS paths internally and need not be valid UTF-8.
- MusicBrainz identifiers, artist credits, sort names, release/disc identity,
  and related metadata remain intact and influence useful default organization.
- One versioned, pure `tkfmt-1` language serves display, sorting, grouping, and
  conversion/file naming in both applications.
- FFmpeg is the common decode/encode backbone; PipeWire is the primary local
  output backend.
- Long work is asynchronous, cancellable, progress-reporting, and bounded.
- Metadata, ReplayGain, conversion, and filesystem writes use complete previews,
  revision checks, conflict detection, verification, and recovery journals.
- File operations accept only contained, revalidated local sources.

## Product principles

### Two tools, one language

Each application does one job completely. They share visual design,
interaction grammar, shortcuts, the expression language, and quality gates, so
moving between them costs nothing — but neither carries the other's
complexity.

### Tabs are working memory

Track lists share one high-performance presentation without losing their
distinct semantics. Tabs persist and make it cheap to branch, compare,
reorder, and act on selections — server queues and playlists in the MPD
client, local working lists in Trackbench.

### Power is native

Tagging, MusicBrainz identification, ReplayGain, conversion, file operations,
search, verification, and artwork management are first-class capabilities of
Trackbench. Basic competence does not depend on plugins.

### Preview before mutation

Every bulk action shows source and result values, warnings, conflicts, and the
affected count before commit. The preview and execution share one immutable
plan.

### Work stays responsive

No network, disk, decoder, tag parser, artwork load, or bulk formatting work
blocks the UI thread. Playback controls acknowledge immediately; performance
is a feature, measured against explicit budgets on large inputs.

### User data is more important than convenience

Unknown tags, embedded objects, timestamps where requested, list identity, and
file relationships survive operations. Partial failure is visible and
recoverable.

### Simple initially, deep on demand

The ordinary path is obvious. Advanced tools use progressive detail: the tag
matrix can expose per-file values, a conversion preset can open its pipeline,
and a simple sort can open its `tkfmt-1` expression.

### Desktop-efficient interaction

Cantata is the interaction-density reference for the everyday player
workspace; foobar2000 is the capability reference for the local workstation.
Neither is a visual skin or compatibility target. Common actions stay visible
and take one click or one keystroke; popups and menus are reserved for
genuinely infrequent or advanced choices. Use native-desktop proportions and
information density rather than touch-sized, web-style spacing.

### Automation is specified and testable

Familiar-looking syntax is not a compatibility promise. Each `tkfmt-1`
construct has repository-owned executable cases and persisted dialect behavior
changes only through a new version.

## MusicBrainz-aware organization

MusicBrainz knowledge is a metadata and organization advantage in both
applications and a first-class identification feature in Trackbench:

- retain recording, track, release, release-group, artist, work, and disc IDs;
- retain credited names separately from sort names and canonical identities;
- prefer release identity over album-title strings for grouping when present;
- order multi-disc releases by medium/disc and track positions;
- use deterministic fallbacks for ordinary files without MusicBrainz tags;
- make album/artist/release selections useful inputs for list construction;
- never overwrite a user's credited display text merely to normalize identity.

The online provider proposes MusicBrainz metadata, identifiers, and artwork
with provenance and confidence. Proposals enter the ordinary staged metadata
preview and cannot write files directly. Lookup, matching, and any acoustic
identification remain explicit network operations.

## Modern interaction requirements

- Each application's default screen exposes its complete workspace without
  layout construction.
- Every important action is available through a discoverable menu/command
  palette and a direct keyboard path.
- Track rows support multi-selection, drag reorder, add-next/end, remove,
  crop, copy/move between tabs, sort, reverse, randomize, and total duration.
- The tag editor behaves like a purpose-built data grid: type a field name,
  move by keyboard, paste rectangular data, and apply actions to a selection.
  It does not require clicking every target cell or hunting through a giant
  fixed dropdown.
- Long tasks live in a job center rather than modal progress dialogs.
- Transient errors appear as non-blocking toasts, retain successful work, and
  support retrying only failures.

## Anti-goals

- Reimplementing or embedding an MPD server.
- Speaking MPD protocol from Trackbench, or playing local files from the
  MPD client.
- Requiring a library scan or database before Trackbench is useful.
- Reproducing foobar2000's Windows UI, component ABI, or scripting quirks.
- Building a streaming-service storefront, DAW, waveform editor, or mastering
  suite.
- Treating metadata as a flat `string -> string` dictionary.
- Blocking the UI during network or collection work.
- Claiming complete file-format support because FFmpeg can decode it.

## Primary users and jobs

### MPD listener (Trackknife)

Connects to a local or remote MPD/Melody server, browses and searches quickly,
manages several working tabs and the live queue, controls outputs, and expects
all state to remain correct when another client changes the server.

### Collection maintainer (Trackbench)

Opens folders of new music, auditions album-grouped tracks, identifies them
through MusicBrainz, edits tags in bulk, preserves identifiers, scans
ReplayGain, manages art, and reorganizes or converts files safely into an
organized collection.

### Format/transcoding user (Trackbench)

Converts selected tracks with reusable codec and destination presets,
resampling, optional DSP, predictable tag transfer, verification, and output
loudness.

## Definition of excellent first public releases

The MPD client must be a client worth choosing: quick setup, reliable
sessions, fast browse/search, tabbed working lists, excellent live queue
control, transport and outputs, and polished keyboard/mouse behavior.
Trackbench must play album-grouped local music gapless and include the first
trustworthy versions of the tag grid, MusicBrainz identification, mass
ReplayGain scanner, and converter. A local library index, plugin ecosystem,
Melody endpoint, and unimplemented Melody upload protocol are not release
prerequisites.
