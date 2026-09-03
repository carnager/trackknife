# ADR-0108: Saved encoder presets

- Status: accepted
- Date: 2026-09-03
- Owners: Trackknife project
- Extends: ADR-0105 versioned presets, ADR-0107 converter dialog

## Context

The four built-in encode targets cover defaults, not taste — a phone
library wants 96 kbps Opus, an archive wants FLAC under another name.
Presets were designed as versioned values precisely so users could own
them.

## Decision

- `SavedEncoderPreset` persists a user-defined `EncoderPreset` in the
  workspace database (migration 27, table `encoder_presets`): stable id,
  preset identity/version, display name (unique, conflict on reuse),
  codec/container/extension, lossless flag, exactly one rate control
  (bit rate or codec VBR quality — both at once is rejected), and the
  sample-format hint. Load/upsert/remove are transactional with a
  256-preset bound, mirrored async on `ListPersistenceService`.
- The converter's preset combo lists the immutable built-ins first,
  then the saved presets after a separator, all probed for encoder
  availability alike. "New…" opens a small editor prefilled from
  whichever preset is selected — editing always saves a new profile,
  built-ins never change. The editor constrains choices to the
  qualified encode formats (FLAC, Opus, MP3, Ogg Vorbis) with
  per-format rate controls; Save requires a name and persists through
  the store, and the combo reloads with the new preset selected.
  Delete appears only for saved presets.

## Consequences

- A saved preset is exactly as reproducible as a built-in: the same
  versioned value drives conversion, and later journaling can record
  it verbatim.
- Tests pin the repository round-trip across restart (ordering, name
  conflict, dual-rate rejection, removal) and the dialog flow (editor
  gating, save into the combo, selection, deletion).
