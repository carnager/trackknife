# Trackknife knowledge base

This directory is the handoff package for humans and coding agents beginning
work on the Trackknife project, whose primary product is the unified
**Trackbench** MPD/Melody and local-file workspace (ADR-0058). The former
standalone **Trackknife** MPD client was retired in ADR-0071.

The ordered delivery plan is [`../MILESTONES.md`](../MILESTONES.md).

## User guides

- [Melody setup](melody.md) — configure the server, connect Trackknife, and add speakers.
- [Local library](local-library.md#using-the-library) — add folders and search your collection.
- [Formatting and scripts](tkfmt.md) — naming patterns and tag transformations.

## Current continuation point

ADR-0115 adds the optional [local music library](local-library.md), with
migration 28, a Qt-free index/query service, and a bounded background Library
panel beside Folders. Physical-file browsing/search and offline retention are
implemented; logical-title indexing and autoplaylists remain future work.
The following milestone history predates this requested workspace addition.

M5 is closing into M6, implementation is accepted through ADR-0114, and the persistence
schema is version 27. In-app MusicBrainz identification is end-to-end: Properties'
Identify… opens a no-id text-search dialog whose ranked rows are individual
release versions, and the chosen version stages as one undoable colored draft
through the ADR-0086 preview, fetched via a lazy cache-first paced service.
Apply is now pure WYSIWYG (ADR-0093): automatic scripts stage their edits
as colored undoable drafts when the grid loads, after Suggest/Identify
staging, and on check — never hidden at write time — and the write plan is
built from the staged draft alone.
Cover Art Archive front covers ride the same boundary: the artwork tab's
Fetch cover resolves one unambiguous draft-or-baseline release id to the
archive's front image and embeds it through the ordinary preservation-exact
add path, and the Covers… picker (ADR-0094) places any archive image —
front replacements, backs, booklets — by mapped role with thumbnails.
Identified releases now stage Picard's complete matched-release
tag set — separate sort names, release-group origin dates, lowercased
type/status, description fields, ISRCs, and work ids. The remaining M6
item — AcoustID — shipped after a ground-truth evaluation (ADR-0096):
Fingerprint files in the Identify dialog rescues selections with no usable
tags through fpcalc and coverage-ranked release candidates. Every planned
M6 work item is now complete, and M7 has opened with ADR-0097's validated
libebur128 loudness core: ITU-conformance-pinned measurement, native-rate
high-rate policy, programme album reduction, and honest unmeasurability
for sub-gate material; ADR-0098 adds the bounded parallel scan graph with
per-item revision capture and incomplete-album warnings, and ADR-0099 the
four grouping modes (track, selection, release-aware, tkfmt-1), and
ADR-0100 the user-facing scan: measured values stage as colored ReplayGain-
provenance drafts through the ordinary pipeline and WYSIWYG apply, with
C-locale value formatting and conventional REPLAYGAIN_* identities. Native WavPack is now the second
qualified text writer (ADR-0095): the shared prepared-copy core carries the
paired-totals rules once for every format, and the WavPack prepare proves
audio blocks and binary APEv2 items byte-exact while rejecting ID3v1
trailers, and MP3 is the third (ADR-0103): region-based audio proof through
ID3v2 resizes with one adapter-keyed prepare dispatch for every qualified
format. Path-only Rename/Move now derives strictly from captured
revision-qualified source tags, never manual drafts or automatic chains, when
Save tags is off. The transformation editor also exposes a canonical Raw script
view for the bounded cleanup subset with live typed translation and explicit
dirty-state protection. Complete typed tagging scripts now import and export
through the strict native version-1 JSON envelope without transferring saved
identity or automatic-enable state. The journal and single-source executor now
also qualify changed destination artifacts on either filesystem topology.
Bounded preparation Apply now composes that executor with one atomic
all-occurrence relocation and metadata refresh, so reviewed native-FLAC text
changes plus Rename/Move are enabled and recover through the same durable path.
Previously used destinations can be reused when only an unowned historical
metadata cache remains, and a rolled-back source no longer invalidates exact
directories that its executor created for the rest of the batch.
Native FLAC picture blocks and exact configured external PNG/JPEG fallbacks now
enter one bounded typed artwork inventory with source revisions, provenance,
dimensions, SHA-256 identity, and duplicate linkage. Properties exposes the
inventory lazily, follows the existing file selection, and caps inspection at
64 physical sources. Native FLAC supports freshly reviewed Add/Replace/Remove
plus Copy-to-Selection through one preservation-exact prepared-copy and
journaled Apply path. Schema 24 stores only compact kind/ordinal/count/SHA
recovery evidence; image bytes and inventory rows remain file/session-owned.
Embedded and external rows can also be exported by a bounded cancellable
no-overwrite job. Copy rereads embedded donors by revision/ordinal/hash without
creating a temporary image. External files and non-FLAC containers remain
unmodified donors/export sources. Repeated Rename/Move also reconciles an exact
target occurrence pre-resolved by older relocation history, so returning files
to their source paths outside Trackbench no longer turns a fully ready review
into an Apply-time all-source failure.
Start metadata and file-operation work with
[`metadata-and-files.md`](metadata-and-files.md),
[`adr/0066-explicit-semantic-and-freeform-field-identity.md`](adr/0066-explicit-semantic-and-freeform-field-identity.md),
[`adr/0067-immutable-preparation-review-and-file-publication-ui.md`](adr/0067-immutable-preparation-review-and-file-publication-ui.md),
[`adr/0068-versioned-metadata-capture-patterns.md`](adr/0068-versioned-metadata-capture-patterns.md),
[`adr/0069-source-tag-authority-for-path-only-preparation.md`](adr/0069-source-tag-authority-for-path-only-preparation.md),
[`adr/0070-canonical-raw-transformation-editor.md`](adr/0070-canonical-raw-transformation-editor.md),
[`adr/0071-retire-trackknife-compatibility-shell.md`](adr/0071-retire-trackknife-compatibility-shell.md),
[`adr/0072-native-metadata-transformation-chain-interchange.md`](adr/0072-native-metadata-transformation-chain-interchange.md),
[`adr/0073-journaled-destination-artifact-publication.md`](adr/0073-journaled-destination-artifact-publication.md),
[`adr/0074-composed-metadata-and-path-publication.md`](adr/0074-composed-metadata-and-path-publication.md),
[`adr/0075-reused-target-cache-and-batch-directory-evidence.md`](adr/0075-reused-target-cache-and-batch-directory-evidence.md),
[`adr/0076-bounded-artwork-inventory.md`](adr/0076-bounded-artwork-inventory.md),
[`adr/0077-read-only-properties-artwork-section.md`](adr/0077-read-only-properties-artwork-section.md),
[`adr/0078-immutable-native-flac-artwork-write-plan.md`](adr/0078-immutable-native-flac-artwork-write-plan.md),
[`adr/0079-journaled-native-flac-artwork-publication.md`](adr/0079-journaled-native-flac-artwork-publication.md),
[`adr/0080-properties-artwork-review-and-apply.md`](adr/0080-properties-artwork-review-and-apply.md),
[`adr/0081-native-flac-artwork-add-export-and-copy.md`](adr/0081-native-flac-artwork-add-export-and-copy.md),
[`adr/0082-pre-resolved-relocation-reconciliation.md`](adr/0082-pre-resolved-relocation-reconciliation.md),
[`adr/0083-direct-apply-with-trusted-live-preview.md`](adr/0083-direct-apply-with-trusted-live-preview.md),
[`adr/0084-silent-recovery-and-draft-color-semantics.md`](adr/0084-silent-recovery-and-draft-color-semantics.md),
[`adr/0085-artwork-thumbnails-and-direct-apply.md`](adr/0085-artwork-thumbnails-and-direct-apply.md),
[`adr/0086-typed-metadata-proposal-boundary.md`](adr/0086-typed-metadata-proposal-boundary.md),
[`adr/0087-picard-paired-totals-identities.md`](adr/0087-picard-paired-totals-identities.md),
[`adr/0088-musicbrainz-web-service-client.md`](adr/0088-musicbrainz-web-service-client.md),
[`adr/0089-release-matching-and-alignment.md`](adr/0089-release-matching-and-alignment.md),
[`adr/0090-in-app-musicbrainz-identify-dialog.md`](adr/0090-in-app-musicbrainz-identify-dialog.md),
[`adr/0091-cover-art-archive-front-cover-fetch.md`](adr/0091-cover-art-archive-front-cover-fetch.md),
[`adr/0092-picard-parity-release-proposals.md`](adr/0092-picard-parity-release-proposals.md),
[`adr/0093-wysiwyg-apply-and-staged-automatic-scripts.md`](adr/0093-wysiwyg-apply-and-staged-automatic-scripts.md),
[`adr/0094-cover-art-archive-image-picker.md`](adr/0094-cover-art-archive-image-picker.md),
[`adr/0095-prepared-copy-wavpack-text-writer.md`](adr/0095-prepared-copy-wavpack-text-writer.md),
[`adr/0096-acoustid-fingerprint-identification.md`](adr/0096-acoustid-fingerprint-identification.md),
[`adr/0097-validated-loudness-analysis-core.md`](adr/0097-validated-loudness-analysis-core.md),
[`adr/0098-parallel-loudness-scan-graph.md`](adr/0098-parallel-loudness-scan-graph.md),
[`adr/0099-loudness-grouping-modes.md`](adr/0099-loudness-grouping-modes.md),
[`adr/0100-replaygain-scan-action.md`](adr/0100-replaygain-scan-action.md),
[`adr/0101-folder-bookmarks-panel.md`](adr/0101-folder-bookmarks-panel.md),
[`adr/0102-mpd-go-to-artist-album.md`](adr/0102-mpd-go-to-artist-album.md),
[`adr/0103-prepared-copy-mp3-text-writer.md`](adr/0103-prepared-copy-mp3-text-writer.md),
[`adr/0104-grouped-numbering-transformation.md`](adr/0104-grouped-numbering-transformation.md),
[`adr/0105-qualified-audio-conversion-core.md`](adr/0105-qualified-audio-conversion-core.md),
[`adr/0106-conversion-metadata-and-parallel-scan.md`](adr/0106-conversion-metadata-and-parallel-scan.md),
[`adr/0107-trackbench-converter-dialog.md`](adr/0107-trackbench-converter-dialog.md),
[`adr/0108-saved-encoder-presets.md`](adr/0108-saved-encoder-presets.md),
[`adr/0109-conversion-resampling-option.md`](adr/0109-conversion-resampling-option.md),
[`adr/0110-conversion-bit-depth-policy.md`](adr/0110-conversion-bit-depth-policy.md),
[`adr/0111-limited-filesystem-publication-tolerance.md`](adr/0111-limited-filesystem-publication-tolerance.md),
[`adr/0112-settings-and-mpd-music-root.md`](adr/0112-settings-and-mpd-music-root.md),
[`adr/0113-library-latest-ordering.md`](adr/0113-library-latest-ordering.md),
and [`adr/0114-qualified-ogg-writers.md`](adr/0114-qualified-ogg-writers.md).
The development, ASan/UBSan, TSan, and clang-tidy builds and their complete
56/56 test suites pass at this continuation point (validated 2026-09-05).
The TSan-only bench test shim routes Qt 6.9+'s uninstrumented
`pthread_clockjoin_np` wait through TSan's intercepted blocking
`pthread_join`; normal Trackbench builds are unchanged, and CTest retains the
outer deadlock timeout. The preset also carries symbol-specific suppressions
for two Qt 6.11 inline QtConcurrent instances — `PagedTrackModel::Page`
dispatch and the probe queue's `ProbeOutcome` future result store — because
their real happens-before edges live inside the intentionally uninstrumented
Qt shared library; the workers and all application payloads remain
instrumented.

The next implementation slice builds the M6 in-app identification UI: a
search surface over the ADR-0088 client presenting ADR-0089's ranked release
versions, staging the chosen candidate through the ADR-0086 proposal
boundary. The WavPack writer qualification
remains the next M5 preservation-matrix slice. The exact current completion
list and next marker live in [`../MILESTONES.md`](../MILESTONES.md).

ADR-0058 supersedes ADR-0025's permanent process split. If an older feature
document still describes Trackbench as unable to speak MPD, read that as a
historical description of the local authority: the primary Trackbench process
hosts both authorities, while local mutation services remain structurally
unavailable to MPD rows. The former standalone Trackknife executable was
retired in ADR-0071.

## Reading order

1. [`product.md`](product.md) — identity, principles, scope, and priorities.
2. [`compatibility.md`](compatibility.md) — what “in the spirit of foobar2000”
   does and does not mean.
3. [`mpd-client.md`](mpd-client.md) — MPD sessions, source mapping, live queue,
   queue/list tabs, and Melody capabilities.
4. [`feature-matrix.md`](feature-matrix.md) — required capabilities and phases.
5. [`ui-workspace.md`](ui-workspace.md) — default MPD layout, queue/list tabs,
   reusable views, and performance budgets.
6. [`m3-validation.md`](m3-validation.md) — automated evidence and final live
   MPD/Melody acceptance pass for the current milestone.
7. [`title-formatting.md`](title-formatting.md) — the versioned Trackknife
   formatting language and its implementation model.
8. [`metadata-and-files.md`](metadata-and-files.md) — tagger, bulk transforms,
   artwork, renaming, moving, and transaction safety.
9. [`replaygain.md`](replaygain.md) — analysis, storage, playback, and conversion.
10. [`playback-library-conversion.md`](playback-library-conversion.md) — remote/
   local playback, server library, lists, conversion, and integrity tools.
   [`local-library.md`](local-library.md) specifies the optional indexed local
   collection and its refresh/availability rules.
11. [`query-language.md`](query-language.md) — deferred deeper search and
   autoplaylist design.
12. [`architecture.md`](architecture.md) — subsystem boundaries and
   unresolved technology choices.
13. [`open-decisions.md`](open-decisions.md) — choices that still need ADRs.
14. [`adr/`](adr/) — accepted and proposed architecture decisions.
15. [`sources.md`](sources.md) — source register and research caveats.

## Status vocabulary

- **Required**: product requirement already established in conversation.
- **Compatibility requirement**: observable external behavior to reproduce.
- **Proposed**: recommended Trackknife design awaiting implementation feedback.
- **Deferred**: valuable, but not necessary for the first useful release.
- **Unknown**: must be decided or measured.

External products may inform the design, but Trackknife behavior is defined by
these specifications, ADRs, and executable tests unless a document explicitly
declares a compatibility requirement.
