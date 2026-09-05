# Trackbench

Trackbench is a Qt 6 music workstation for Linux. It is one window with two
sides: an MPD client for the daemon that plays your library, and a local-file
workspace for the work your library needs — tagging, MusicBrainz lookups,
ReplayGain, format conversion, and file organization. The two sides share the
same views and never blur: local operations act on files, MPD operations go
through the protocol, and nothing mutates the other side implicitly.

It exists because foobar2000 runs under Wine and Cantata is dead. The goal is
to replace both with something native that takes file safety seriously.

## What it does

MPD side:

- Queue and server library browsing, with the library sortable alphabetically
  or by most recently added music (MPD 0.24 `Added`).
- Search, queue editing, priorities, repeat/random/single/consume, ReplayGain
  mode, independent output toggles, cover art.
- "Go to artist/album" from any queue row into the library tree.
- A configurable music-root mapping: right-click tracks or albums, load them
  as local files in a new tab, tag or convert them there, then ask MPD to
  rescan exactly the folder you touched.

Local side:

- Persistent list tabs, a filesystem tree with bookmarks, gapless playback
  through PipeWire, cue sheets and subsongs as first-class logical tracks.
- A multi-file tag editor with colored drafts, undo, and automatic
  transformation scripts that stage visibly before anything is written.
- MusicBrainz identification with AcoustID fingerprinting and Cover Art
  Archive fetching.
- ReplayGain scanning (EBU R128, track and album gain, true peak) with
  album grouping by release, tag, or expression.
- A parallel converter: FLAC, Opus, MP3, and Vorbis presets plus your own
  saved ones, optional resampling and 16/24-bit output with dither, tags
  carried into the output and verified by rereading the result.
- Rename/move publication driven by naming expressions, with journaling and
  crash recovery.

Tag writing is deliberately conservative. Writers exist only for formats where
the result can be proven: after every write the audio bytes are compared
against the original and the tags are reread and compared against the plan.
Currently that covers FLAC, WavPack, and MP3 (ID3v2). Formats without that
proof are read-only until they get one.

Network filesystems are treated as first-class: publication and conversion
degrade cleanly on NFS, sshfs, and FAT (no `RENAME_NOREPLACE`, no xattrs, no
`chown`) instead of failing, and report what could not be preserved.

## Scripting

Naming layouts, conversion paths, library tree levels, ReplayGain grouping,
and metadata transformations all use `tkfmt-1`, a small deterministic
formatting language in the spirit of foobar2000's title formatting:

```text
%albumartist%/%album%/$num(%tracknumber%,2) - %title%
```

See [docs/tkfmt.md](docs/tkfmt.md) for the language reference and
[docs/title-formatting.md](docs/title-formatting.md) for the formal contract.

## Status

Pre-release. There are no versioned releases yet; the schema migrates
automatically and every change lands with tests, but expect rough edges and
an opinionated feature set. Development is documented in
[MILESTONES.md](MILESTONES.md) and the decision records under
[docs/adr/](docs/adr/).

## Building

Dependencies: Qt 6 (base), FFmpeg, TagLib ≥ 2.0, libmpdclient, libopenmpt,
libutf8proc, PipeWire, libebur128, SQLite. Build tools: CMake ≥ 3.28 and
Ninja. Optional at runtime: chromaprint (`fpcalc`) for AcoustID
fingerprinting.

```sh
cmake --preset release
cmake --build build/release
./build/release/src/bench/trackbench
```

On Arch, `packaging/arch/` contains a PKGBUILD that builds from the
repository head:

```sh
cd packaging/arch && makepkg -si
```

Tests are opt-in for the package build (`TRACKBENCH_CHECK=1 makepkg`). For
development, four configure presets exist — `dev`, `asan`, `tsan`, and
`tidy` — and all of them are expected to pass the full suite:

```sh
cmake --preset dev && cmake --build build/dev
cd build/dev && ctest
```

## License

GPL-3.0-only. See [LICENSE](LICENSE).
