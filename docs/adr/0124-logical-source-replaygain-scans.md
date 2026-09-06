# ADR-0124: Preserve logical source identity in Properties ReplayGain scans

- Status: accepted
- Date: 2026-09-06
- Extends: ADR-0098, ADR-0100, ADR-0042

## Context

Properties captured a raw path and metadata baseline but discarded the local
row's decoder selection and sample range. Its ReplayGain request consequently
measured the whole default source for CUE tracks, container chapters, and
tracker subsongs. End-to-end regressions reproduced incorrect track gains for
all three cases. Different logical tracks may also share one writable physical
file: ordinary embedded ReplayGain fields cannot represent their individual
measurements, even when only one logical track is selected.

## Decision

**Trackknife decision:** Capture the typed audio stream/subsong selection and
optional end-exclusive sample range alongside each Properties source in the
existing bounded capture loop. Keep this immutable scan input aligned with
accepted Properties rows, including when unavailable rows are omitted. Workers
index it by the original Properties item index when scanning a subset; metadata
drafts continue to supply grouping context. No decoder or filesystem work moves
to the UI thread.

Scan results still enter visible, undoable drafts through the existing proposal
pipeline. Track gain/peak measure the selected signal, and album reduction uses
the selected logical programmes. Scanning remains independent of tag write
capability.

The Qt-free staged source records whether it represents a logical track. The
ordinary physical metadata planner reports an unresolved storage target for
the four conventional ReplayGain fields and the two R128 gain fields on such
sources. This applies to new and existing fields, logical and exact-native
addresses, and single-row selections. It prevents segment measurements from
becoming whole-file tags without changing ordinary metadata edit semantics.
An explicit selected stream is conservatively treated as a logical source.

## Validation and limits

The offscreen workspace tests ingest a generated two-level WAV with an external
CUE sheet, a real FLAC-in-Matroska chapter fixture, and the repository's two-
subsong MOD fixture. Properties track gains, peaks, and album gains must match
direct scans of the actual ingested source selections/ranges. Undo followed by
a scan of only the second row verifies subset addressing. Whole-file scan
regressions and physical write-plan tests cover the storage boundary.

Validation on 2026-09-06: the development warnings-as-errors build, all 57 CTest
tests, formatting, SPDX headers, and whitespace checks passed.

Durable logical-track sidecar/library storage and playback consumption remain
open. This change does not qualify universal embedded storage, add an Opus R128
policy, or implement shared-source single-pass segment decoding. M5 remains
the active milestone gate.
