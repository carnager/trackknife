# ADR-0105: Qualified audio conversion core

- Status: accepted
- Date: 2026-09-03
- Owners: Trackknife project
- Extends: ADR-0043 prepared-copy qualification discipline, M8 converter
  milestone

## Context

M8 converts lists and selections into predictable destinations with no
partial output. The first slice needs the trustworthy primitive everything
else composes over: one source in, one verified destination file out.

## Decision

- A new Qt-free `Trackknife::Convert` static library provides
  `convert_audio_file`: the project decoder (so container selections and
  cue-sheet sample ranges convert exactly like whole files) feeds
  libswresample into an FFmpeg encoder/muxer pair chosen by an
  `EncoderPreset`.
- Presets are versioned values, not FFmpeg option strings: a stable id, a
  preset version that increments whenever identical input would produce
  different output, encoder/muxer/extension names, and exactly one rate
  control (bit rate or codec VBR quality). Built-ins: `flac` (lossless,
  s32 → 24-bit until the bit-depth policy slice), `opus-192`, `mp3-v0`,
  `vorbis-q6`. `probe_encoder_preset` verifies at runtime that the system
  FFmpeg actually contains the encoder and muxer — libopus/libmp3lame are
  build options, never assumed.
- Format negotiation queries the encoder's supported configurations
  (FFmpeg 8 `avcodec_get_supported_config`): the source sample rate when
  the encoder accepts it, otherwise the smallest supported rate at or
  above it (Opus takes 44.1 kHz material up to 48 kHz, never down); the
  preset's sample-format hint when supported, otherwise float-first
  preference. An audio FIFO rechunks resampled frames so fixed-frame-size
  encoders always see whole frames.
- No partial output, proven: the encoder writes a hidden
  `.<name>.tk-part-<random>` sibling; the finished temporary is decoded
  end to end with the project decoder and must match the negotiated
  format and the rate-adjusted source duration within 200 ms; only then
  is it fsynced and renamed into place with `RENAME_NOREPLACE` (an
  existing destination is a typed conflict, checked up front and again
  atomically at publish). Every failure and cancellation path removes the
  temporary; the source is never touched.

## Consequences

- Later M8 slices (destination modes, metadata/artwork mapping, bounded
  parallel scheduling, ReplayGain handling) compose over one primitive
  whose atomicity and verification are already pinned by tests: every
  built-in preset converts and verifies, temporaries never survive,
  occupied destinations and missing directories fail typed, cancellation
  leaves nothing.
- A real cue-image segment (30 s of a 44.1 kHz FLAC album image)
  round-trips through all lossy and lossless presets with sample-exact
  durations, validating the segment path the cue-aware modes will use.
