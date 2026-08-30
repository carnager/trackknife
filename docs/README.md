# Trackknife knowledge base

This directory is the handoff package for humans and coding agents beginning
work on the Trackknife project, whose primary product is the unified
**Trackbench** MPD/Melody and local-file workspace (ADR-0058). The former
standalone **Trackknife** MPD client was retired in ADR-0071.

The ordered delivery plan is [`../MILESTONES.md`](../MILESTONES.md).

## Current continuation point

M5 is active, implementation is accepted through ADR-0071, and the persistence
schema is version 20. Path-only Rename/Move now derives strictly from captured
revision-qualified source tags, never manual drafts or automatic chains, when
Save tags is off. The transformation editor also exposes a canonical Raw script
view for the bounded cleanup subset with live typed translation and explicit
dirty-state protection.
Start metadata and file-operation work with
[`metadata-and-files.md`](metadata-and-files.md),
[`adr/0066-explicit-semantic-and-freeform-field-identity.md`](adr/0066-explicit-semantic-and-freeform-field-identity.md),
[`adr/0067-immutable-preparation-review-and-file-publication-ui.md`](adr/0067-immutable-preparation-review-and-file-publication-ui.md),
[`adr/0068-versioned-metadata-capture-patterns.md`](adr/0068-versioned-metadata-capture-patterns.md),
[`adr/0069-source-tag-authority-for-path-only-preparation.md`](adr/0069-source-tag-authority-for-path-only-preparation.md),
[`adr/0070-canonical-raw-transformation-editor.md`](adr/0070-canonical-raw-transformation-editor.md),
and
[`adr/0071-retire-trackknife-compatibility-shell.md`](adr/0071-retire-trackknife-compatibility-shell.md).
The development, ASan/UBSan, TSan, and clang-tidy builds and their complete
43/43 test suites pass at this continuation point (validated 2026-08-31).
The TSan-only bench test shim routes Qt 6.9+'s uninstrumented
`pthread_clockjoin_np` wait through TSan's intercepted blocking
`pthread_join`; normal Trackbench builds are unchanged, and CTest retains the
outer deadlock timeout.

The next implementation slice is native transformation-chain interchange,
followed by another preservation-proven writer or combined changed-content/
destination publication. The exact current
completion list and next marker live under **M5 — Fast tag workspace and safe
file operations** in
[`../MILESTONES.md`](../MILESTONES.md).

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
