# ADR-0111 addendum: tolerant source locking

- Status: accepted
- Date: 2026-09-03
- Extends: ADR-0111 limited-filesystem publication tolerance

## Context

NFS emulates `flock` through POSIX byte-range locks, and exclusive POSIX
locks require a write-open descriptor — so the executors' exclusive locks
on read-only source descriptors fail with EBADF, blocking tag commits and
publications for files that live on the share. Filesystems without a lock
manager report ENOLCK.

## Decision

Both lock helpers (`metadata_commit` mutation sources, `file_publication`
locked sources and prepared copies) degrade in a ladder: exclusive, then
shared (which a read-open descriptor can express over NFS), then unlocked
where no lock can be taken at all. Locking is intra-host coordination; the
revision revalidation performed immediately before every mutation carries
correctness on every rung. Genuine contention (EWOULDBLOCK) still waits
and cancels exactly as before.

Test expectations became capability-aware the same way the code did:
ownership-preservation asserts only where the filesystem accepts `chown`,
and text-undo accepts the deliberate typed unavailability where
`RENAME_EXCHANGE` does not exist.

## Verification

With their temporaries on the real NFS NAS: the file-publication executor
suite passes completely, and the metadata-commit suite passes for all text
tag commits — undo reports its designed unavailability. The remaining
artwork undo chain on exchange-less filesystems is a recorded follow-up,
as is the MPD music-root mapping (now in M9) that this unblocks.
