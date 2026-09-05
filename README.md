# Trackknife

Trackknife plays music and helps you sort out your collection. Connect to MPD
or Melody, or open files directly. It runs on Linux and uses Qt 6.

The idea comes from foobar2000 and Cantata. The name refers to foobar2000's
reputation as a Swiss Army knife for audio files.

Local files play gaplessly through PipeWire. Browse folders as they are, or
add them to the library. Search finds both albums and tracks, locally and on
MPD. The two players keep separate queues.

For the files that need work, there's bulk tagging, MusicBrainz lookup,
ReplayGain scanning, conversion, and renaming. Tag edits support FLAC,
WavPack, MP3, Vorbis, and Opus; other formats are read-only.

Still in development, with no versioned releases yet.

![MPD queue with grouped albums and the server library](Screenshots/queue.png)

## Building

Dependencies: Qt 6 (base), FFmpeg, TagLib ≥ 2.0, libmpdclient, libopenmpt,
libutf8proc, PipeWire, libebur128, SQLite. Build tools: CMake ≥ 3.28 and
Ninja. Optional at runtime: chromaprint (`fpcalc`) for AcoustID
fingerprinting.

```sh
cmake --preset release
cmake --build build/release
./build/release/src/bench/trackknife
```

On Arch Linux:

```sh
cd packaging/arch && makepkg -si
```

## Setup and help

- [Melody setup](docs/melody.md): server config, connecting, and remote speakers.
- [Local library](docs/local-library.md#using-the-library): choose **Library** in
  the Local Queue sidebar, then **Folders…** to add music.
- [Scripting](docs/tkfmt.md): naming patterns and tag transformations using
  `tkfmt-1`, Trackknife's own language—not foobar2000 or Picard scripts.
- [Development progress](MILESTONES.md).

## More screenshots

![Album and track search](Screenshots/search.png)

![Bulk tag editing](Screenshots/tagger.png)

![MusicBrainz lookup](Screenshots/musicbrainz.png)

![Conversion](Screenshots/converter.png)

![Renaming](Screenshots/renaming.png)

## License

GPL-3.0-only. See [LICENSE](LICENSE).
