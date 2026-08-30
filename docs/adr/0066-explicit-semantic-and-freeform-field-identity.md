# ADR-0066: Explicit semantic and freeform metadata field identity

- Status: accepted
- Date: 2026-08-30
- Owners: Trackknife project
- Supersedes: the separator-derived alias identity in ADR-0033, ADR-0034,
  ADR-0039, ADR-0040, ADR-0043, and ADR-0048
- Extends: ADR-0065 pasted rule-script translation

## Context

The initial metadata model lowercased ASCII and discarded spaces, underscores,
and hyphens to obtain one canonical field identity. That made convenient UI
spellings such as `Album Artist` find `ALBUMARTIST`, but it also made unrelated
native properties aliases. A native-FLAC `ALBUM ARTIST` cleanup rule therefore
previewed and wrote against the conventional `ALBUMARTIST` field.

The same inference would be more dangerous in other formats. An ID3v2 `TPE2`
frame may have the semantic role `albumartist`, while a `TXXX` frame whose
description happens to be `Album Artist` is a separate freeform object. MP4
freeform atoms likewise include their mean/name identity. Similar text does not
prove equivalent storage identity or meaning.

Trackbench must expose arbitrary metadata for inspection, editing, and removal.
It must not need an ever-growing alias list to do so.

## Decision

- Metadata has two address classes:
  - a **semantic field** exists only when a format adapter explicitly maps a
    native representation to one Trackbench field;
  - a **freeform native field** retains the adapter's opaque native identity
    and is never promoted to a semantic field by spelling similarity.
- Every format adapter owns an explicit mapping table. ASCII case handling is
  defined by that format. Spaces, underscores, hyphens, punctuation, frame
  kinds, descriptions, atom namespaces, and other identity components are not
  discarded unless the adapter's documented mapping explicitly says so.
- The qualified native-FLAC adapter recognizes only its enumerated conventional
  Vorbis-comment keys, case-insensitively. Thus `ALBUMARTIST` maps to semantic
  `albumartist`, while `ALBUM ARTIST`, `ALBUM_ARTIST`, and other unlisted keys
  remain separate freeform fields.
- Properties displays freeform fields alongside conventional fields. Existing
  freeform fields can be edited and removed independently. Logical operations
  affect only explicitly mapped semantic properties; exact-native operations
  affect only their addressed native property.
- Pasted `$delete(name)` and `$unset(name)` rules use exact-native addressing.
  ASCII case is folded for native-FLAC keys, but separators and punctuation are
  preserved. Hand-authored **Remove field** remains a semantic operation.
- An exact-native address is retained through transformation preview, sparse
  draft staging, fresh write planning, prepared-copy verification, the durable
  mutation journal, commit recovery, and undo/reconciliation comparisons.
- Schema 19 stores exact-native remove modes in the existing transformation
  action integer argument and adds the optional exact-native address to
  operation-journal changes. Downgrade refuses while either representation is
  present.
- TagLib's generic `PropertyMap` is only an exposed-property boundary. It does
  not prove that every backend-specific object identity is represented. A
  writable ID3 adapter must model a frame ID plus the required descriptor or
  qualifiers, and a writable MP4 adapter must model its complete atom identity;
  neither may infer aliases from flattened names.

## Alternatives considered

### Maintain a large cross-format alias catalogue

Rejected. The catalogue would conflate presentation convenience with physical
identity, remain incomplete, and make destructive behavior depend on guesses.

### Keep separator folding and add a special case for `album artist`

Rejected. The reported field is one example of the general identity bug. Every
new custom spelling or format namespace would require another exception.

### Hide unknown/freeform fields

Rejected. A metadata workstation must let users inspect and clean arbitrary
tags without requiring an external tool.

## Consequences

- Completion may still rank separator-insensitive text for discovery, but it
  must retain distinct semantic and freeform results and never use ranking as
  mutation identity.
- Existing cached snapshots may contain the earlier folded name until the file
  is freshly read. They remain presentation caches and are never mutation
  authority.
- Format-specific mapping work becomes explicit, reviewable, and testable.
  Unknown fields remain useful instead of being rejected or silently aliased.
- An adapter can expose a native field before it can safely write it; capability
  and preservation proof remain independent.

## Validation

- Qt-free tests distinguish conventional and freeform property resolution and
  require imported deletion preview to target only the exact native field.
- A real FLAC round trip creates both `ALBUMARTIST` and `ALBUM ARTIST`, exposes
  both staged rows, removes the latter, and proves the former and all preserved
  file data remain intact.
- Journal and saved-chain round trips retain exact-native addresses across
  restart.

## Revisit when

- ID3v2, MP4, APEv2, or another format gains a typed writable adapter;
- the UI adds an explicit create-new-native-field affordance;
- title formatting gains a separate raw-native lookup function.
