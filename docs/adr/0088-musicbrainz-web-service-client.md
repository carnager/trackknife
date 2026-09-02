# ADR-0088: MusicBrainz web-service client

- Status: accepted
- Date: 2026-09-02
- Owners: Trackknife project
- Extends: ADR-0086 typed metadata proposal boundary

## Context

M6 needs MusicBrainz lookups that never bypass staged write safety and never
abuse the shared service. MusicBrainz asks clients for at most one request
per second and an identifying User-Agent, answers 503 when throttled, and
serves slowly changing data that deserves caching. Crucially, Trackbench
must do what Picard does not: identify releases from plain text — artist
and album with no MusicBrainz id anywhere — and present the candidates,
including the different versions of a release, inside the application
instead of bouncing through a browser.

## Decision

### Typed requests and parsing, no hidden network

- A new `Trackknife::MusicBrainz` library owns the ws/2 boundary. URL
  builders produce the release *text search* (Lucene-escaped quoted artist
  and release phrases, optional track-count clause, bounded result limit)
  and the release lookup (`inc=artist-credits+recordings+release-groups+
  labels`), validating MusicBrainz ids structurally.
- Pure parsers turn `fmt=json` bodies into bounded typed structs. Search
  results keep everything a version picker needs per candidate: score,
  date, country, status, disambiguation, barcode, label and catalog
  number, media formats and track counts, artist credits with join
  phrases and ids, and the release-group id that groups versions of one
  album. Lookups add per-medium track listings with track and recording
  ids, positions, and lengths (track detail falls back to the recording).
  Malformed payloads and exceeded limits fail typed; absent optional
  detail is dropped, never invented.

### One paced, serialized, cache-first fetcher

- `MusicBrainzClient` holds one request in flight and spaces dispatches at
  least 1.1 seconds apart — slightly slower than MusicBrainz requires.
  Transport is injected, so pacing, ordering, caching, and error mapping
  are proven without a network; the production transport sets the
  identifying User-Agent and follows only same-origin redirects.
- Failure states are typed and honest: transport failure is `io`
  ("unreachable" — the offline state), 503 is `backend` ("asked to slow
  down"), 404 is `not_found`, anything else non-200 is `backend` with the
  status. Nothing retries silently.

### Bounded durable response cache

- Migration 25 adds `musicbrainz_response_cache`: exact response bodies
  keyed by request URL with their fetch time. Loads honour a 14-day TTL;
  stores prune expired rows and enforce a 10,000-entry bound oldest-first.
  A cache hit answers without touching the network or the pacing budget.
- The cache holds response bodies only — proposals still flow through the
  ADR-0086 boundary into ordinary staged, colored, undoable drafts, and
  nothing from the network ever writes to a file directly.

## Alternatives considered

### libcurl transport

Rejected. Qt Network is already idiomatic here, integrates with the event
loop the client paces on, and adds no new dependency class.

### File-based response cache

Rejected. The shared SQLite store already has migration discipline, crash
safety, and one place to bound; a directory of body files would need all
three rebuilt.

## Consequences

- M6 matching can be built entirely against fixture JSON; the network
  enters only through one paced, cached, typed chokepoint.
- Text-first search is the identification path: no MusicBrainz id is ever
  required, and release versions arrive as distinct, fully described
  candidates for in-app selection.
- The open rate-limit/cache policy decision is resolved (1.1 s spacing,
  14-day TTL, 10k entries); whether AcoustID earns its dependency remains
  open.

## Validation

- Fixture tests prove Lucene/percent escaping, id validation, version
  detail and track/recording alignment in parsing, and typed
  malformed/oversized failures.
- Client tests with a scripted transport prove cache hits bypass the
  network, requests serialize with enforced spacing, and each failure
  state maps to its typed error; success stores into the cache.
- Cache tests prove store/load round trips, TTL expiry, oldest-first
  bounding, and typed rejection of out-of-bounds stores.

## Revisit when

- recording-level search or Cover Art Archive fetches join (same client,
  new builders);
- AcoustID evaluation needs its own posting transport.
