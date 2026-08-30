# Trackknife

This repository builds **Trackbench**, a fast modern Qt 6 workspace with
separate MPD/Melody and local-file authorities (ADR-0058). MPD Queue and Local
Queue tabs share one visual language and complete track-view layout engine;
the active tab switches the sidebar, transport, and MPD/PipeWire output
selector. Local tagging and filesystem operations remain unavailable for MPD
rows. The former standalone **Trackknife** MPD client was retired once
Trackbench reached parity (ADR-0071); Trackbench is the only shipped workspace
executable.

The buildable foundation, `tkfmt-1` engine, asynchronous MPD backbone, M3 Qt
workspace, and Trackbench's local FFmpeg/PipeWire playback workspace
(sample-accurate logical tracks, bounded playback buffering, direct PipeWire
output) are implemented. M4 is complete; active M5 builds the fast metadata and
safe file-operation workspace. Start with
[`MILESTONES.md`](MILESTONES.md), then [`docs/README.md`](docs/README.md).

## Current development handoff

M5 is active. The implementation and documentation are accepted through
ADR-0071 and SQLite schema 20. With Save tags off, path-only Rename/Move now
uses only captured revision-qualified source tags; neither manual drafts nor
checked automatic transformations can create a filename that misrepresents the
file. The transformation editor also provides canonical Raw script editing for
the bounded cleanup subset with live typed translation and Save/discard dirty
protection. The development, ASan/UBSan, TSan, and clang-tidy builds and their
complete 43/43 test suites pass at this continuation point (validated
2026-08-30).

The next vertical slice is the one named at the end of M5 in
[`MILESTONES.md`](MILESTONES.md): add native transformation-chain interchange
to the preparation workspace, then qualify
another preservation-proven writer or direct destination-artifact
publication for changed tags plus paths. ReplayGain remains deliberately
capability-gated until M7.

For a fresh coding session, ADR-0058 is authoritative over older descriptions
of Trackknife and Trackbench as permanently separate applications. Trackbench
is the primary combined workspace; MPD Queue and Local Queue remain distinct
authorities selected by the active primary tab. The former `trackknife`
compatibility shell was retired in ADR-0071.

## Build

Requirements are CMake 3.28+, Ninja, a C++23 compiler, Qt 6.4+ with Gui,
Widgets, Concurrent, and Test modules, utf8proc 2.9+, libmpdclient 2.22+,
FFmpeg libavformat 60+/libavcodec 60+/libavutil 58+/libswresample 4+,
libopenmpt 0.7+, TagLib 2.0+, PipeWire 0.3.50+, and nlohmann/json 3.11+ for the
expression-corpus runner.

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Trackbench is the only workspace executable, with MPD and local queue
contexts; the **MPD Queue** context hosts the MPD/Melody client workspace
(`docs/ui-workspace.md`) and the **Local Queue** context hosts local playback
and preparation:

```sh
./build/dev/src/bench/trackbench [files or folders…]
```

The same adapter has a headless diagnostic:

```sh
./build/dev/src/tools/trackknife-mpd-probe \
  --host 127.0.0.1 --port 6600 --music-root /path/to/music
```

Set `MPD_PASSWORD` in the environment when authentication is required; the
probe intentionally does not accept a password argument that would appear in
the process list.

The `tkfmt-1` CLI and native expression sandbox remain available for formatting
diagnostics.

For sanitizer, static-analysis, and optimized builds:

```sh
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
cmake --preset tidy && cmake --build --preset tidy && ctest --preset tidy
cmake --preset release && cmake --build --preset release
```

## License

The Trackknife project and all of its binaries are licensed under the GNU
General Public License version 3 only
(`GPL-3.0-only`). See [`LICENSE`](LICENSE).
