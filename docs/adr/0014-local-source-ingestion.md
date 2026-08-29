# ADR-0014: Preserve raw paths during cancellable local-source ingestion

- Status: accepted
- Date: 2026-08-26
- Owners: Trackknife project

## Context

M4 introduces ad-hoc local files without creating a second indexed library
database. File, folder, drag/drop, recent-location, and command-line entry
points must converge on one behavior. Linux paths are arbitrary byte strings,
folders can be large, and filtering by extension would confuse a filename
convention with FFmpeg's actual probe result. Working lists also deliberately
preserve duplicates.

## Decision

- Store every selected local file as the exact raw OS path bytes in a
  `ListSource::local` working-list item. Presentation uses a reversible escaped
  form and never becomes the source identity.
- File and folder inputs use one Qt-free discovery function. Folder expansion
  is recursive and raw-byte sorted within each selected root; the order of
  roots and duplicate occurrences is preserved.
- Do not follow directory symlinks during recursive discovery. An explicitly
  selected regular-file symlink remains an explicit source path and is
  revalidated by the later opener before decode or mutation.
- Include regular files without extension filtering. The FFmpeg probe boundary,
  not the filename, will decide whether a source is decodable.
- Run discovery on a bounded shared worker pool, expose cancellation, cap one
  request at 100,000 files, and retain structured per-path issues. A cancelled
  discovery does not partially modify the destination list.
- Append to the active local working list, or the first available scratch list
  when another surface is active. A drop retains its visible insertion point.
- Keep at most ten recent raw input locations. Reopening a location repeats
  discovery rather than persisting a directory snapshot.

## Alternatives considered

### Convert paths to UTF-8 strings

This loses valid Linux filenames and violates the repository's raw-path
contract.

### Filter a fixed list of audio extensions

Extensions neither prove decodability nor cover every container FFmpeg can
probe. Capability belongs to the format adapter.

### Follow directory symlinks recursively

This makes cycle handling, surprising traversal outside the chosen tree, and
duplicate expansion the default. Explicitly selected symlink paths remain
available without that risk.

### Import folders into a local library database

Trackknife's product direction keeps MPD as the canonical library and uses
working lists for ad-hoc files. ADR-0017 later adds direct filesystem navigation
to the library pane without changing this no-import decision.

## Consequences

- Ingestion is useful before local decode lands and its stored identity will not
  need migration later.
- Opening overlapping roots intentionally creates duplicate occurrences.
- A folder can include non-audio regular files; the later asynchronous probe
  reports unsupported items truthfully.
- The mapped-MPD containment and symlink policy remains a separate decision
  because mapped sources cross a configured trust boundary.

## Validation

- Core tests cover recursive raw-byte discovery, deterministic order,
  duplicates, invalid UTF-8 path bytes, cancellation, and escaped display.
- Widget tests open a real folder asynchronously, display its items, and verify
  the persisted list retains local source kind and exact raw paths.
- CLI and native drag/drop feed the same discovery path as the dialogs.

## Revisit when

- measured folders make the 100,000-file cap or worker-pool policy unsuitable;
- a portal API can provide raw Linux filenames more faithfully than the Qt file
  dialog boundary;
- FFmpeg probing shows a cheap safe prefilter that does not become a capability
  claim.
