# ADR-0118: Local library track numbers and covers

- Status: accepted
- Date: 2026-09-05
- Owners: Trackknife project

## Decision

The user requested track numbers and covers in the local library, following
the MPD library's presentation. The existing index already stores numeric
track positions; query results now expose them to the shipped `tkfmt-1`
labels. Numbered tracks render as `03. Title`, including artist-qualified
search results. Missing numbers omit the prefix instead of inventing one.
No schema migration or formatting-language change is needed.

Album rows show local covers through the shared library delegate. One artwork
worker resolves a representative available indexed file in disc/track/path
order, reads its embedded artwork, then falls back to conventional sibling
`cover`, `folder`, or `front` JPEG/PNG images (case-insensitive names). Library
and local-list artwork use the same worker-only reader. There is no network
lookup, tag write, or library scan involved in displaying a thumbnail.

Only visible album rows request artwork. Requests are serial, independent of
the scan/query workers, and cancellable when the view hides, changes, or
scrolls away. A 256-entry LRU stores thumbnails and missing-image results;
model rows consult it rather than retaining unbounded independent images.
Thumbnails fit within 128 pixels while preserving aspect ratio. Encoded input
is bounded at 16 MiB, source dimensions at 16 million pixels and 32,768 pixels
per side, and sibling traversal at 10,000 entries. Missing, corrupt, or
oversized pictures retain the record placeholder.

An explicit Refresh or committed local-operation notification invalidates
thumbnails. Generation and cancellation checks discard stale worker results.
Ordinary search/expansion reuses cached covers. ADR-0116's manual-only library
scanning policy remains in force.

## Verification

Real FLAC tests cover numeric and slash-total positions, missing numbers,
tree/search labels, embedded artwork precedence, folder fallback, raw filename
bytes, thumbnail aspect ratio, operation/manual refresh, hidden-view loading,
cancellation, corrupt images, and oversized fallback files. Existing local
intake and format artwork tests exercise the shared reader's consumers.
