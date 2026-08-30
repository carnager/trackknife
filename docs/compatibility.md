# Compatibility and inspiration

Since ADR-0025 the repository builds two applications: Trackknife, the pure
MPD/Melody client, and Trackbench, the foobar2000-inspired local-file
workstation. The MPD/Melody sections below describe Trackknife; the
foobar2000-derived sections describe Trackbench.

## MPD compatibility

Trackknife's primary interoperability target is the documented MPD protocol.
It discovers commands and tag types at connection time and must work with stock
MPD without Melody extensions. The server remains authoritative for its
database, current queue, playback, mixer/options, outputs, and stored playlists.

Melody is a first-class server target because it speaks MPD. Trackknife may use
advertised Melody additions such as richer output state and exclusive output
handoff, but those additions remain capability-gated and cannot change stock
MPD behavior. Protocol fixtures name whether they come from official MPD
documentation, stock MPD, or the local `../melody` implementation.

**Trackknife decision:** an `idle` event invalidates cached state; it does not
itself confirm the result of a command. A pending playback action is confirmed
only when the refreshed status matches the requested state. A stale refresh
must not visually roll back the action while later `player` events are pending;
failure or a bounded confirmation timeout ends the pending presentation.

## Product relationship to foobar2000

Trackbench is a spiritual successor to foobar2000, not a clone. It preserves
the high-value outcomes: tabbed playlist/working surfaces, fast interaction,
gapless local playback, ReplayGain, broad formats, powerful metadata workflows,
configurable views, conversion, and predictable bulk operations. It does not
reproduce foobar2000's UI, component ABI, private configuration formats, or
scripting quirks. Trackknife's reference points are different: Cantata's
interaction density and MPD/Melody protocol compatibility, not foobar2000.

## Formatting language: no external compatibility promise

ADR-0008 replaces the former 1:1 foobar2000 title-formatting target. The
shared `tkfmt-1` language, used by both applications, uses the familiar
`%field%` and `$function(arguments)` shape also used by MusicBrainz Picard,
but the project's own specification and executable corpus are normative.

Consequences:

- foobar2000 and Picard scripts may look similar but are not promised to run;
- there is no separate foobar truth flag or optional-section behavior;
- Picard metadata-mutating functions such as `$set` are intentionally absent
  from `tkfmt-1`; ADR-0065's bounded paste importer may translate a documented
  subset into ordinary Trackbench rules without executing Picard code;
- missing fields, escaping, integer conversion, multi-values, and every
  built-in follow `docs/title-formatting.md`;
- an importer or compatibility dialect must remain separate and must not
  change `tkfmt-1`.

## Search/query syntax

Foobar-style query syntax is deferred rather than a compatibility requirement.
MPD's advertised search/filter behavior serves the initial client. Any later
query language in either application needs its own versioned specification;
formatting expressions and queries remain different languages.

## Metadata and workflow parity

Trackbench needs equivalent outcomes for mass editing, arbitrary and
multi-value tags, value derivation, safe file operations, ReplayGain,
conversion, library views, playlists, and integrity checks. Equivalent outcomes
do not require matching dialog layouts, menu locations, preset file formats, or
implementation details.

## Deliberately unsupported compatibility surfaces

- foobar2000 component binaries or SDK ABI;
- Default UI/Columns UI layouts, themes, and color-control strings;
- proprietary configuration databases and `.fpl` as native storage;
- Windows-only path, shell, output, or codec behavior;
- undocumented title-formatting and query quirks;
- Picard plugin APIs and metadata-mutating tagging scripts.

The ADR-0065 paste importer helps migrate a small documented cleanup subset by
translating it into Trackbench's versioned models. It does not make the
external language or source text canonical.

## Rich source references: retained lesson

A playlist and library need more than a path. Both applications use one shared
logical record:

```text
TrackRef
  source kind: remote MPD | mapped local | local-only
  remote identity: profile ID + exact MPD URI + optional queue song ID
  local identity: SourceId + subsong/segment + observed filesystem identity
  cached arbitrary ordered multi-value metadata
  cached technical information
  cached loudness with provenance
  availability and freshness state
```

A path or MPD queue position is mutable data, not identity. When a local move
or rename succeeds in Trackbench, its lists, local playback, statistics, and
sidecar references follow one logical transaction. Updating the MPD database
remains a separate visible server operation for any MPD client, including
Trackknife.

Metadata precedence is:

1. freshly read embedded/container data;
2. explicit Trackbench sidecar overrides;
3. a current library record, once Trackbench gains a local index;
4. a playlist snapshot only while the source is unavailable or unscanned.

Reconciliation compares source identity and revision before replacing cached
data, and the UI exposes stale/fallback state.

## Versioned persistence

Persist every formatting expression with:

- dialect and dialect version (`tkfmt`, `1`);
- exact original source;
- compiler-schema version;
- typed usage context;
- optional human name.

Never persist only a compiled AST. Parser implementations evolve, while users
must retain editable source and stable behavior.
