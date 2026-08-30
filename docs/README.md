# Trackknife knowledge base

This directory is the handoff package for humans and coding agents beginning
work on the Trackknife project, whose primary product is the unified
**Trackbench** MPD/Melody and local-file workspace (ADR-0058). The standalone
**Trackknife** MPD client remains as a compatibility shell during migration.

The ordered delivery plan is [`../MILESTONES.md`](../MILESTONES.md).

## Current continuation point

M5 is active, implementation is accepted through ADR-0066, and the persistence
schema is version 19. The latest completed slice replaces spelling-derived tag
aliases with explicit adapter mappings and separately mutable freeform native
fields. Start metadata work with
[`metadata-and-files.md`](metadata-and-files.md),
[`adr/0065-pasted-rule-script-translation.md`](adr/0065-pasted-rule-script-translation.md),
and
[`adr/0066-explicit-semantic-and-freeform-field-identity.md`](adr/0066-explicit-semantic-and-freeform-field-identity.md).
The handoff baseline is 44/44 development tests, 10/10 focused sanitizer tests,
and a clean full clang-tidy build.

The next implementation slice is to combine final metadata and planned paths
in one immutable review, then expose Rename and Move through the existing
ADR-0054–0063 planning, preflight, publication, recovery, and reconciliation
core. The exact current completion list and next marker live under **M5 — Fast
tag workspace and safe file operations** in
[`../MILESTONES.md`](../MILESTONES.md).

ADR-0058 supersedes ADR-0025's permanent process split. If an older feature
document still describes Trackbench as unable to speak MPD, read that as a
historical description of the local authority: the primary Trackbench process
hosts both authorities, while local mutation services remain structurally
unavailable to MPD rows. The standalone Trackknife executable remains a
compatibility shell during migration.

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
