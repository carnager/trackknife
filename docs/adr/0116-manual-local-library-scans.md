# ADR-0116: Manual local library scans

- Status: accepted
- Date: 2026-09-05
- Owners: Trackknife project

## Decision

The user requested that library scans run only when they press **Refresh**.
This supersedes ADR-0115's startup, periodic, and post-operation scan triggers.

Startup displays the cached index. Adding a folder records it and prompts the
user to press Refresh. No timer or filesystem watcher starts a scan. Refresh
starts one asynchronous, cancellable incremental pass; completion and Stop
never queue another pass.

Committed metadata and relocation operations still update existing index
records through the shared transaction. Their UI notifications reload cached
queries only. External edits, newly converted files, and reconnected folders
are discovered by the next explicit Refresh.

## Verification

Real-file UI tests cover idle startup, adding a folder without scanning,
explicit Refresh, cancellation, and notifications during and after a scan.
Accelerated timers exercise the absence of recurring scans. Existing index
tests retain transaction, incremental indexing, and offline retention coverage.
