# ADR-0076: Bounded typed artwork inventory

- Status: accepted
- Date: 2026-08-31
- Owners: Trackknife project
- Extends: ADR-0033 typed local metadata reads and ADR-0043
  preservation-verified native-FLAC text preparation

## Context

Trackbench can load the first FFmpeg-attached picture for album-list display,
and its native-FLAC text writer proves that one real embedded picture survives
an unrelated tag rewrite. That display helper discards picture type,
description, declared MIME type, dimensions, source identity, additional
pictures, and external-file provenance. It is not sufficient input for an
artwork-management preview.

M5 needs a read-only inventory before replace, remove, export, or copy can be
qualified. The inventory must not turn a neighboring directory into an
unbounded implicit library scan or make backend objects part of Trackbench's
domain model.

## Decision

- `Trackknife::Metadata` owns a Qt-free artwork inventory distinct from text
  fields. Each item records Trackbench's typed role (`front`, `back`, `artist`,
  `disc`, `icon`, or `other`), the exact adapter-exposed type, MIME type,
  description, optional dimensions, byte size, SHA-256 content fingerprint,
  provenance, raw source path, source revision, and native ordinal.
- The initial embedded adapter qualifies native FLAC picture blocks through
  TagLib's picture API. Every picture is retained in native order. The exact
  ID3v2 picture-type value is mapped into the smaller Trackbench role set while
  its native name remains visible. Other embedded container mappings remain
  unreadable until they receive exact fixtures.
- External discovery examines only an ordered, caller-supplied list of exact
  sibling basenames. The default policy covers the established `cover`,
  `folder`, `Folder`, and `front` JPEG/PNG fallbacks. It does not glob, recurse,
  normalize case, decode raw path bytes as UTF-8, or infer an image from an
  arbitrary directory resident.
- Missing configured siblings are ordinary absence. A present but unreadable,
  changing, oversized, malformed, or unsupported external image produces a
  typed per-source issue while retaining independently proven inventory items.
  PNG and JPEG signatures provide the initial external MIME and dimension
  evidence.
- Reads are revision-bracketed and cancellable. Policy limits bound item count,
  individual encoded size, aggregate encoded size, pattern count, description
  text, and declared MIME/type text. Exceeding a global bound fails the
  inventory rather than returning an apparently complete mutation input.
- Encoded bytes are held only while inspecting and hashing one item and are not
  returned or persisted. Equal SHA-256 fingerprints retain both provenance
  records and identify the earlier item as the duplicate source; inventory does
  not silently discard either occurrence.
- Native FLAC now advertises picture-read capability. Picture-write capability
  remains false. This ADR exposes no replace, remove, export, or copy action and
  does not broaden the existing text-writer claim.

## Alternatives considered

### Extend the first-picture display helper

Rejected. FFmpeg's attached-picture projection is useful for display fallback,
but it neither exposes every native FLAC picture attribute nor supplies the
revision-qualified external provenance needed by safe operations.

### Store image bytes in list snapshots or SQLite

Rejected. The local workspace is not a media library, and durable image blobs
would duplicate user files. Fresh inventory plus bounded in-memory display
caches is sufficient.

### Scan every image in the source directory

Rejected. That makes results depend on unrelated files, scales poorly in large
folders, and provides no deterministic source-order policy.

### Enable FLAC picture writes with the reader

Rejected. A readable picture block does not prove replacement/removal mapping,
preservation of multiple unrelated pictures, journal composition, or recovery.

## Consequences

- Later artwork review and provider proposals have one backend-independent
  identity and provenance vocabulary.
- Embedded and external copies remain separately visible even when their bytes
  are equal.
- External pattern configuration can evolve without reinterpreting embedded
  picture identity.
- The existing artwork display cache may later consume the inventory but is not
  changed by this first slice.

## Validation

- The repository's real 64-by-64 PNG-in-FLAC fixture inventories its exact
  native `Other` picture type, MIME, dimensions, size, and fingerprint instead
  of silently promoting the first image to a front cover.
- Materializing those exact PNG bytes as a configured external sibling proves
  revision-qualified external provenance and cross-provenance duplicate
  linkage.
- A real external JPEG proves signature-based MIME and dimensions. Missing
  siblings remain silent, malformed candidates produce typed issues, global
  limits fail closed, and cancellation fails promptly.
- The existing real native-FLAC text-write fixture continues to require exact
  embedded picture bytes, all non-comment blocks, compressed audio, and decoded
  PCM to survive.

## Revisit when

- Resolved by ADR-0077: the Properties artwork section consumes the inventory
  lazily without persisting it;
- Resolved by ADRs 0078–0079: replace/remove receive an immutable plan,
  verified prepared-copy path, and durable journal/publication/recovery path;
- exact ID3v2, MP4, Vorbis-comment, APEv2, ASF, or Matroska artwork mappings are
  qualified;
- external source-order preferences become persisted workspace settings.
