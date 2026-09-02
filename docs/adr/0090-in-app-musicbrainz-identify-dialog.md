# ADR-0090: In-app MusicBrainz Identify dialog

- Status: accepted
- Date: 2026-09-02
- Owners: Trackknife project
- Extends: ADR-0086 typed proposal boundary, ADR-0088 MusicBrainz client,
  and ADR-0089 release matching

## Context

Picard cannot look a release up without a MusicBrainz id: text search
happens in a browser that posts results back to a local proxy. Trackbench
must do better — the whole identification flow lives inside the
application, works from nothing but the tags already on the files, and
presents every version of a release as a distinct, choosable candidate.
ADR-0088/0089 supplied the client, matching, and proposal bridge; this ADR
fixes the user-facing surface and its wiring into Properties.

## Decision

### One fetch boundary, injected like every other service

- `MusicBrainzLookupService` is a single `std::function` fetch boundary
  (`url → async Result<QByteArray>`). Properties receives it by value in
  its constructor; an empty service keeps Identify… disabled, and tests
  script it with fixture JSON without any network or pacing.
- The production implementation, `MusicBrainzFetchService`, lives in the
  bench layer and is created lazily by `BenchMainWindow` on first use. It
  composes the ADR-0088 paced client with the migration-25 response cache:
  a worker-thread cache read first (no SQL on the UI thread), then the
  serialized 1.1 s-paced network fetch, then a fire-and-forget
  worker-thread durable store. `QPointer` guards deliver a typed cancelled
  error instead of touching a destroyed service.

### The Identify dialog searches text, never ids

- Identify… in Properties builds `LocalTrackDescriptor`s synchronously
  from the current selection's baseline tags (title, artist, album, track
  and disc numbers, no file I/O beyond the already-loaded documents) and
  opens one window-modal `bench-musicbrainz-identify` dialog pre-filled
  with album artist (else artist) and album.
- Search issues the ADR-0088 Lucene-escaped release text query with the
  selection size as corroborating track count. Results are ranked by
  ADR-0089 `rank_release_candidates` and every release version is its own
  row: match score, album, credited artist, track count, media formats,
  and a version column joining date · country · disambiguation · label ·
  catalog number — the Picard-style version picker, in-app.
- "Use this version" (or double-click) fetches the full release lookup,
  runs ADR-0089 alignment and the proposal bridge, and hands the resulting
  `MetadataProposalSet` back to Properties. The dialog never writes
  anything; a no-confident-match outcome is reported in place so another
  version can be tried.

### Accepted versions stage like any other draft

- Properties routes the accepted set through the existing ADR-0086
  proposal preview worker and grid staging: one undoable transaction,
  ordinary ADR-0084 draft colors, the direct-apply contract unchanged.
  The status line names MusicBrainz as the source. While the dialog is
  open, Identify and Suggest stay disabled; closing it re-enables them.

## Consequences

- Files with no MusicBrainz tags at all can be identified, version-picked,
  reviewed as colored drafts, and applied without leaving Trackbench —
  the M6 capability Picard lacks.
- The scripted-service seam keeps the whole flow offscreen-testable
  end-to-end (search → picker → use → staged grid values) with zero
  network dependency.
- Cover Art Archive proposals and any AcoustID evaluation can reuse the
  same lookup-service seam and dialog pattern later in M6.
