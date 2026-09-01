# ADR-0087: Picard-paired totals identities

- Status: accepted
- Date: 2026-09-02
- Owners: Trackknife project
- Amends: ADR-0066 explicit adapter mappings (its "similarity never creates
  an alias" rule is unchanged; this adds two explicitly enumerated
  equivalences) and ADR-0086's provider-level spelling handling (now
  withdrawn in favor of this identity-level pairing)

## Context

Vorbis tagging grew two spellings for the same meaning: `TRACKTOTAL` (what
most rippers write) and `TOTALTRACKS` (the vocabulary Trackbench and
foobar2000 use), with `DISCTOTAL`/`TOTALDISCS` as the disc analog. Treating
them as distinct logical fields split one value across two grid rows, made
naming expressions and Suggest blind to half the world's files, and let one
album drift into mixed spellings. Picard's answer is proven: read either,
prefer the primary when both exist, write both.

## Decision

- `canonicalize_field_name` maps the `tracktotal` and `disctotal`
  canonicals onto `totaltracks`/`totaldiscs`. This is an explicitly
  enumerated identity, not similarity: everything keyed by canonical name —
  the grid, drafts, capture targets, naming expressions, providers — sees
  one logical totals field regardless of the file's spelling.
- The conventional FLAC mapping table gains the secondary spellings as
  aliases that resolve to the same canonical but never win the write name;
  `paired_flac_property_names` enumerates the pair, primary first.
- **Load rule** (in `read_local_metadata`): when a file carries both
  spellings, the primary is surfaced and the secondary field is dropped
  from the document; the secondary comment itself stays byte-preserved.
  A file carrying only `TRACKTOTAL` surfaces it as the one totals field
  with its exact native name intact.
- **Write rule** (in the prepared-copy FLAC writer): a replace of a paired
  canonical erases every alias spelling and writes the value under both
  paired names; a removal deletes both. Verification expects the primary
  spelling, which is exactly what the load rule surfaces on reread.
  Exact-native changes keep their single-name behavior.
- Suggest proposes one logical totals value again; the pairing lives below
  it where every write path benefits.

## Alternatives considered

### Presentation-only merging in the Fields table

Rejected. Hiding the duplicate row leaves naming expressions, capture, and
providers split-brained, and editing one row while the other spelling
drifts stale reproduces exactly the partial-tagging confusion this fixes.

### Provider-level double proposals (the interim fix)

Withdrawn. It only covered Suggest; manual edits and transformations still
wrote a single spelling, and the grid still showed two fields.

## Consequences

- One "Total Tracks" (and "Total Discs") row everywhere; any edit through
  any path lands in both native spellings, mirroring Picard.
- Old cached rows persisted under the `tracktotal` canonical simply stop
  matching and refresh on the next probe.
- Non-FLAC writers must implement the same pair when they arrive; the pair
  list is the single source of truth.

## Validation

- A real-FLAC round trip proves: staging totals writes both comments;
  reread surfaces one field with the primary native name and no duplicated
  effective values; a later edit refreshes both spellings; removal deletes
  both while disc totals survive untouched.
- Proposal tests prove `TRACKTOTAL`-only files satisfy the logical field
  and bare files receive exactly one proposal; the end-to-end scratch
  pipeline confirms mixed albums converge without touching satisfied files.

## Revisit when

- other formats gain writers (ID3 `TRCK` "n/total" packing is a different
  shape of the same problem);
- more paired vocabularies surface (e.g. `MOVEMENTTOTAL`).
