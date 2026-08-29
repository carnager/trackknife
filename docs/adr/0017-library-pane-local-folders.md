# ADR-0017: Make the left pane a library tree with direct local-folder navigation

- Status: accepted
- Date: 2026-08-26
- Owners: Trackknife project
- Amended by: ADR-0020 keeps the two source surfaces and evolves their default
  names/purposes into the **MPD** and **Local** domains.

## Context

The shipped workspace already reserves its left pane for server navigation,
while local files currently enter only through open/drop/CLI ingestion into
working lists. The intended product layout is a library tree, and users also
need to walk local folders without repeatedly opening a folder chooser.

That navigation must not reverse ADR-0009's decision that MPD remains the
primary indexed library authority. Linux paths also remain arbitrary byte
strings, so a conventional UTF-8-only tree model would violate the established
source identity contract.

## Decision

- The left dock is the **Library** pane. It owns a stable tab strip with at
  least a server-backed library tab and a **Local Folders** tab.
- The server tab browses MPD folders and advertised tag dimensions. Its nodes
  retain MPD URI/profile identity and MPD remains authoritative for membership.
- **Local Folders** directly and lazily enumerates explicitly chosen and recent
  filesystem roots. It is a navigator, not an imported or indexed Trackknife
  collection.
- Local nodes retain exact raw OS path bytes and use the reversible escaped
  presentation already established by ADR-0014. Directory symlinks are not
  followed during expansion.
- File capability and metadata are resolved asynchronously through the same
  probe/job boundaries used by open/drop ingestion. A filename or tree presence
  never claims that FFmpeg can decode it.
- Activating local files or folders feeds a bound track view or explicit
  working-list action with local-source identity and badges. It never inserts
  them into MPD or implies server availability.
- Pane tab/root/expansion state may be persisted as workspace state, but no
  background whole-root scan or second canonical library database is created.

## Alternatives considered

### Keep local folders only in modal open dialogs

This preserves the smallest sidebar but makes repeated folder exploration
awkward and does not match the intended library-tree workspace.

### Import selected roots into a Trackknife library database

That creates a second collection authority, adds scan/reconciliation setup, and
contradicts the MPD-client-first direction. Direct navigation provides access
without owning collection membership.

### Mix local roots into the MPD folder hierarchy

This hides the source boundary and makes local availability look like server
membership. Separate tabs keep provenance and actions understandable.

## Consequences

- The default left pane is useful for both remote-only and local-file workflows
  while keeping the two sources visibly distinct.
- The tree model must support lazy asynchronous raw-path enumeration,
  cancellation, bounded results, lossless display, and disappearance/errors.
- M4 gains a visible Local Folders implementation slice; the existing local
  ingestion core remains the action boundary rather than being duplicated.
- A future indexed local collection would require a separate explicit product
  decision and migration design.

## Validation

- Widget tests keep server and local tabs distinct across profile changes and
  workspace restoration.
- A raw-byte fixture tree proves lazy expansion, stable escaped display,
  cancellation, and the no-directory-symlink traversal rule.
- Activating a local selection creates local working-list references without
  issuing MPD add commands or persisting library membership.

## Revisit when

- very large direct roots require a measured paging or watch strategy;
- desktop portals can provide raw Linux paths more faithfully than the current
  file-dialog boundary;
- users demonstrate a need for a separately indexed local collection.
