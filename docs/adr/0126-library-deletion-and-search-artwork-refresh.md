# ADR-0126: Library deletion and search artwork refresh

- Status: accepted
- Date: 2026-09-06
- Owners: Trackknife project

## Decision

The user reported deleted subfolders lingering as Unavailable inside an online
network mount, and updated covers appearing in library browsing but remaining
stale in search. M5 remains active.

This supersedes ADR-0115's unconditional retention of missing files. After a
complete explicit Refresh, the local index forgets confirmed missing files.
Artist and album groups derived from those records disappear naturally. Offline
roots and incomplete, failed, or cancelled traversals retain their cached entries.
Cleanup changes only the rebuildable library index, never files or working lists.

A missing record is eligible for removal only when its path reports ENOENT and
the nearest existing parent is a directory on the file's recorded device.
An accessible empty mountpoint on another device is insufficient evidence of
deletion. Permission errors, symlink parents, changed devices, and other uncertain
observations retain unavailable entries. Returned files restore availability on
Refresh. Device numbers are filesystem observations, not permanent volume IDs;
same-device bind-mount substitutions are not distinguishable by this evidence.

Cleanup reads at most 200 candidate path/revision pairs per batch and performs
filesystem checks outside write transactions. The raw-path primary-key index
supports keyset traversal without repeatedly sorting all missing entries. Each
delete rechecks the captured path, revision, unseen status, root, and scan token,
so newer scans and committed metadata/relocation updates remain authoritative.
Cancellation stops admission between batches; already committed cleanup remains.
No schema migration is needed.

The reproduced artwork defect is in MPD search: database/update notifications
reload the server browse model but previously left the retained search artwork
unchanged. Every browse-model reset now invalidates search images, missing-image
results, and request tokens, then resumes the existing serial bounded artwork
requests. Search text, result rows, selection, and expanded albums stay intact.
Late artwork responses from earlier requests cannot restore stale images.

Local browse and search already share one thumbnail cache. Their existing manual
Refresh and committed-operation invalidation remains the common refresh path;
additional real-file coverage checks transitions between the two views. Neither
authority starts a local filesystem scan merely by searching or fetching artwork.

## Verification

Before the fixes, regressions reproduced retained deleted albums and the absence
of new MPD search artwork requests after a database notification. The same cases
pass with the implementation.

Local tests cover deleted subfolders and final-album removal, incomplete and
cancelled scans, offline/reconnected roots, simulated changed-device evidence at
an accessible empty mountpoint, raw paths, multiple cleanup pages, and unchanged
saved list occurrences. Real FLAC and PNG tests cover embedded-to-folder cover
replacement, operation/manual refresh, and repeated browse/search transitions.
The MPD workspace test checks refreshed cover pixels, preserved query and model
indexes, repeated searches, and rejection of obsolete artwork tokens.

Validation: the warnings-as-errors development build, all 58 development CTest
targets, repository formatting check, and `git diff --check` passed. The device
substitution test injects recorded device evidence; this does not claim a live
network-mount acceptance test.
