# ADR-0127: Local list ordering and duplicate removal

- Status: accepted
- Date: 2026-09-06
- Owners: Trackknife project

## Decision

The user requested the next queue-editing roadmap slice: local list sorting,
reversing, and duplicate removal, integrated with undo/redo. M5 remains active.

Edit and local track context menus expose **Sort list**, **Reverse list**, and
**Remove duplicate entries**. Commands affect the entire current local list,
regardless of selection. MPD rows and Properties tabs cannot be destinations.
Sorting presets cover title, artist/album/track, album/track, disc/track number,
and path. Custom sorting opens a native non-modal bar for a `tkfmt-1` expression
and ascending/descending choice. Expression text and direction are session-only.

Sort expressions compile once in the existing sort host. Ordered effective
metadata retains its existing provenance and multi-value semantics. Cached
column values supply missing display fields; a missing title falls back to the
escaped filename stem. `$info` exposes escaped path, directory, filename,
filename with extension, and extension without filesystem access. No new
language functions, aliases, or dialect behavior are introduced.

Rendered keys use repository-owned Unicode simple lowercase, then natural
comparison of ASCII digit runs without integer conversion or overflow. Other
text compares by UTF-8 bytes. This is locale-independent, with no normalization
or full Unicode case folding. Leading zeros do not distinguish equal numeric
runs. Equal keys retain their original order in both directions. Number/total
track values remain intact and compare naturally; formatting expressions do not
silently turn those tag values into integers. These are list comparison rules,
not changes to `tkfmt-1` evaluation.

Duplicate identity is the exact raw path, optional logical reference, optional
stream and subsong selection, and optional sample range (including an open end).
Keep the first occurrence in current list order. Metadata, filesystem revision,
and artwork are not duplicate identity. Different logical tracks and raw paths
stay distinct, including symlink/hardlink paths: no filesystem resolution, audio
comparison, or file deletion occurs.

Each successful command creates one named edit in ADR-0123's bounded history.
No-ops preserve the redo branch. Sort/reverse remap persistent indexes, selected
occurrences, and the playing occurrence without resetting the model. Duplicate
removal has ordinary row-removal semantics: if the playing occurrence is a later
duplicate, removing it detaches that occurrence; undo does not start playback or
reattach it automatically. The resulting list uses normal dirty-state saving and
survives restart; its undo history does not.

## Implementation boundary and limits

The Qt-free `lists` module computes permutation/removal plans over detached cached
records. The UI captures at most 128 rows or roughly 256 KiB per event-loop batch
with a 4 ms time budget, checking a 64 KiB per-row and 128 MiB estimated snapshot
payload limit before copying. Reverse needs only the row count. One private
worker handles planning; sort keys have a 64 MiB total bound and 64 KiB per-key
bound, and requests allow at most one million rows and 4096 expression bytes.
Oversized or invalid requests report an error without applying a partial edit.

Progress and Cancel remain available in the inline bar. Sort comparisons,
duplicate comparison, and row planning check cancellation. Structural or metadata
changes, tab changes, source destruction, and window closure invalidate pending
plans. Current-row/artwork notifications do not. The worker never accesses models
or widgets, and stale completions cannot edit another list. A command received
while a cancelled worker is still stopping asks the user to retry instead of
queuing more background jobs. Once a current complete plan is ready, the model
applies it as one edit through the established permutation/removal path.

No schema migration or external dependency is added. MPD playlist operations,
randomizing, selected-subset ordering, and add/replace/cross-tab undo remain
separate follow-ups.

## Verification

The local-list-edit suite covers natural numeric/Unicode sorting, stable
ascending/descending ties, custom multi-value expressions, raw paths, distinct
logical sources, invalid expressions and cancellation, persistent playing and
selected indexes, named undo/redo and no-op preservation, stale captures, tab
switches, 10k-row event-loop yielding, and explicit resource-limit errors.
The owned `tkfmt-1` corpus records shipped sort expressions and raw-value behavior.
Workspace coverage exercises menu actions, MPD gating, native expression-text
undo, dirty state, persisted order, and cancellation before close-time saving.
The existing conditional live PipeWire progression regression also applies and
undoes a reverse permutation while retaining the playing occurrence.

Validation: the warnings-as-errors development build and all 59 development
CTest targets passed. After the final field/info case-handling adjustment,
local-list-edit and the formatting corpus passed again, along with a fresh build,
repository formatting, and `git diff --check`. The 10k-row responsiveness checks
prove event-loop yielding; they do not claim a measured million-row acceptance
pass or milestone closure.
