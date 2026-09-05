# ADR-0114: Qualified Ogg Vorbis and Opus text writers

- Status: accepted
- Date: 2026-09-05
- Owners: Trackknife project
- Extends: ADR-0043 prepared-copy qualification, ADR-0095 WavPack,
  ADR-0103 MP3

## Context

Ogg Vorbis and Ogg Opus were read-only: their tags live in a Vorbis
comment packet inside the Ogg stream, and rewriting it legitimately
relayouts pages — lacing, page sequence numbers, and CRCs all change —
so the byte-region preservation proof used by FLAC and MP3 cannot apply.

## Decision

- Preservation is proven at the logical packet layer. A shared Ogg
  packet parser reassembles the stream's packets (rejecting chained or
  multiplexed files, unknown page versions, and truncated packets), and
  the qualification requires: identical packet counts, the comment
  packet (index 1) carrying its codec magic on both sides, and every
  other packet — codec headers, Vorbis setup, and all audio —
  byte-identical between source and prepared copy.
- One writer serves both codecs behind the adapters `taglib-vorbis-v1`
  and `taglib-opus-v1`, reusing the shared text-writer core: exclusive
  prepared copies, plan-original verification, PropertyMap application,
  and exact reread comparison. Vorbis comments carry the Picard-paired
  totals spellings, so paired writing is enabled exactly as for FLAC.
- The reader detects both formats natively (TagLib type plus the OggS
  marker), reports fields writable with unknown data preserved, and
  keeps pictures FLAC-only.
- Tests round-trip edits on generated fixtures for both codecs and
  additionally decode source and prepared copy to bit-identical PCM —
  the packet proof and the decode proof pin the same guarantee from
  two directions.
