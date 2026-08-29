# ADR-0019: Use a configurable, lazy MPD server-library tree

- Status: accepted
- Date: 2026-08-26
- Owners: Trackknife project

## Context

The Server tab was a flat launcher. Choosing Artists, Albums, or Genres replaced
the center view with one disconnected list of tag values. This did not provide
the persistent expandable library hierarchy expected from the left pane or the
useful interaction demonstrated by Cantata.

Cantata builds its hierarchy from a complete cached MPD database. Trackknife has
already decided that MPD remains the library authority, opening the server view
must not download the whole database, and a second canonical library index is
not required. A tree therefore needs to remain server-backed and lazy.

The hierarchy also cannot be hard-coded. ADR-0008 establishes one ordered
`tkfmt-1` expression per structural library-tree level.

## Decision

- The Server tab contains a real expandable tree. Folder and stored-playlist
  browsers remain adjacent explicit actions; they no longer masquerade as
  hierarchy levels.
- A versioned tree definition stores an ordered list of levels. Each level has
  a name, grouping expression, label expression, stable sort expression, and an
  optional `omit when single` policy. The final level produces track leaves.
- The definition separately records its MPD root tag. Query planning is not
  inferred from arbitrary rendered text. MPD lists only the root tag values;
  expanding one root performs one exact server-side track query for that value.
  Descendant groups are derived from that bounded branch response.
- A branch requests one sentinel row beyond the 10,000-track limit. An
  oversized result is reported instead of silently presenting a truncated tree.
- Definitions persist their schema and `tkfmt-1` dialect identity and are
  editable through the native tree editor. Invalid expressions are rejected
  before replacing the active definition, and the shipped default can always
  be restored.
- The shipped default is:
  **Album Artist → Album (oldest dated releases first, undated last) → Disc
  (only for multi-disc releases) → Tracks**. `Artist` is the root fallback when
  the server does not expose `AlbumArtist`.
- Selection and expansion stay inside the tree and never replace the center
  workspace. Every node exposes inline append-to-queue, insert-next, and
  replace-and-play actions; the same commands plus **Add to list** destinations
  are available from its context menu. An unloaded root action first completes
  that branch's bounded query, then performs exactly the requested action.
  Inline controls occupy an overlay on only the current or hovered row, rather
  than permanent action columns that compress every label. A branch row toggles
  on one click, while action hit targets suppress that disclosure behavior.
- An always-visible recursive filter shrinks the tree to matching rows without
  changing server membership. Filtering is strict: every visible row matches
  itself or contains a matching descendant, and a matched row never drags its
  whole subtree in. Branch rows match on their labels only — hint text is not
  searchable — while track rows also match on their descriptive tags
  (artist, album artist, album, date, genre, composer, performer), so an
  artist query keeps that artist's complete chain and an album query keeps
  exactly its own album path. Filtered results auto-expand so matches are
  immediately visible, and clearing the filter collapses back to roots.
  Because unloaded roots have no local descendants to
  match, a debounced query additionally runs one bounded any-tag server search
  (a single 200-track page) whose results reveal the exact root-tag values
  that contain matching descendants; those roots stay visible with their
  disclosure arrows, and are fetched automatically only while few enough
  roots are visible to keep the resulting traffic bounded. The server page is
  a visibility hint only — it never becomes tree membership, and formatting
  expressions never become an implicit query language.
  Level-specific two-line rows show album counts, dated
  album names, track counts, aggregate or per-track durations, and appropriate
  artist/album/track icons. Artist roots use a stable silhouette placeholder.
  Album nodes use compact thumbnails, show a deliberate record placeholder, and
  request their covers asynchronously, one at a time, after their branch loads.
  Lazy roots advertise their expandable state before loading so a filter never
  removes disclosure indicators from matching artists. A first expansion waits
  for inserted child rows before opening, allowing the same native animation as
  cached branches.

## Consequences

- The familiar library hierarchy remains visible while queue/list construction
  happens without taking focus or content away from the current center tab.
- Memory and network cost scale with expanded artist branches, not the complete
  MPD database.
- Fully arbitrary definitions remain constrained by MPD's advertised tag types
  at the root. Display/group/sort expressions can be richer than the query, but
  formatting expressions never become an implicit query language.
- A later branch cache may improve repeated expansion, but it remains a bounded
  projection invalidated by MPD database events rather than another authority.

## Validation

- Adapter coverage proves bounded exact tag lookup and its hard limit.
- Model coverage proves lazy root/branch requests, deterministic artist and
  chronological album order, optional disc elision, track order, expression
  validation, definition serialization, and level-specific summaries.
- Workspace coverage proves that the Server tab owns a filtered `QTreeView`,
  retains folder/playlist access, exposes inline queue actions, and no longer
  replaces the center view merely because selection changed.

## Revisit when

- measured libraries contain ordinary single root branches above 10,000 tracks;
- MPD gains a portable grouped/windowed query that can page one hierarchy
  branch without losing complete grouping semantics;
- users need multiple named presets rather than one active editable definition.
