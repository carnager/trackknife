# ADR-0123: Local list removal and reorder history

- Status: accepted
- Date: 2026-09-06
- Owners: Trackknife project

## Decision

**Trackknife decision:** deliver the first queue-editing roadmap item as
per-tab, in-memory undo/redo for local row removal and drag rearrangement.
Ctrl+Z undoes; Ctrl+Shift+Z or Ctrl+Y redoes. Edit and local row menus expose
named actions and their availability. Shortcuts belong to local track views,
so search fields retain native text undo and Properties retains tag-draft undo.
MPD queue commands remain server-owned and cannot invoke local list history.

History stores removed occurrences with their original positions and inverse
reorder permutations. Duplicates, raw path bytes, decoder selections, and
logical ranges remain distinct. New edits discard the redo branch; no-op
moves/removals do not. Restored removals are selected. Undo/redo marks the tab
dirty and uses the existing asynchronous workspace save path. It never starts
playback or mutates files. History is discarded when its tab closes or the
application exits; the resulting list contents persist normally.

Retain at most 100 edits and 64 MiB of estimated retained payload per tab.
Old entries expire first, with a status message when the limit is reached.
Verified metadata/path publication also updates detached removed rows, retaining
logical overlays. If a removed snapshot cannot be safely reconciled, discard
its history rather than resurrecting a stale path or blocking live-row refresh.

Reorders remap persistent model indexes, including non-adjacent selections;
they do not reset the model. The existing contiguous move notifications remain.
Playback order and gapless preload refresh from the preserved playing occurrence.
Removal invalidates only removed occurrences; undo does not automatically
restart or reattach playback to a formerly removed playing row.

## First-slice boundaries

Adding/replacing list contents, cross-tab moves, automatic Consume removal,
and a probe expanding one provisional file into multiple logical rows establish
a new positional baseline and clear affected history. They are not undoable in
this slice. Ordinary metadata enrichment and verified commits preserve history.
Adding/replacing contents and atomic cross-tab undo remain follow-up work, not
part of the completed removal/reorder claim. No schema or dependency change.

## Verification

Model and workspace regressions cover scattered moves without resets, retained
playing indexes, duplicate removals, undo/redo branching, no-op edits, bounded
history, raw paths, logical ranges, fresh metadata/path restoration, restored
selection, authority gating, and native text undo. The real WAV/PipeWire
progression regression also replays removal and scattered moves while playing
before verifying the next tracks and gapless progression.
