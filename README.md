# Trackknife

This repository builds two fast, modern Qt 6 applications that share internal
libraries and one visual language (ADR-0025):

- **Trackknife** — a keyboard-fluent tabbed client for MPD and Melody with
  excellent live-queue and working-list management. The basic client is
  finished and live-validated against stock MPD 0.24.14 and Melody 0.23.5.
- **Trackbench** — a foobar2000-inspired workstation for local files:
  playback with album grouping, tagging, MusicBrainz support, ReplayGain,
  conversion, and resampling. No library database for now; it grows from
  direct filesystem navigation.

MPD or Melody owns the server library; Trackknife is purely its client.
Trackbench speaks no MPD protocol and publishes prepared files by plain
filesystem access, even when the destination is an MPD music root.

The buildable foundation, `tkfmt-1` engine, asynchronous MPD backbone, M3 Qt
workspace, and the local FFmpeg/PipeWire engine (sample-accurate decode,
bounded playback buffering, direct PipeWire output) are implemented. The
active M4 milestone splits the codebase into the two applications and stands
up Trackbench's playback workspace. Start with
[`MILESTONES.md`](MILESTONES.md), then [`docs/README.md`](docs/README.md).

## Build

Requirements are CMake 3.28+, Ninja, a C++23 compiler, Qt 6.4+ with Gui,
Widgets, Concurrent, and Test modules, utf8proc 2.9+, libmpdclient 2.22+,
FFmpeg libavformat 60+/libavcodec 60+/libavutil 58+/libswresample 4+, plus
PipeWire 0.3.50+, and nlohmann/json 3.11+ for the expression-corpus runner.

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

The MPD client executable is a pure Qt Widgets workspace with a tabbed left
Library pane for server navigation and direct local folders,
live/scratch/search-result tabs, optional dock panels, a compact
two-row top player with current-track metadata and right-aligned search, an
album-grouped queue, and status-bar queue-summary/mode/output controls. Typing
opens a transient mixed album/track search surface; each result has append,
add-next, and replace-queue actions usable by mouse, focused action cells, or
Enter. Append is the default action, Right advances to add-next and then
replace, and `Ctrl+Enter` directly replaces. `Down` enters results, typing or
Backspace continues editing the query, `Up` returns to it, and `Escape` returns
to the previous work surface. `Shift+Enter` preserves the query as a separate
result tab from either focus location. Use **Connect** or `Ctrl+K` to start the
asynchronous live MPD/Melody session. Queue, search, folders, outputs, playback
modes, ReplayGain, and queue mutations are bound to the typed session
controller. Saved profiles are switchable from the Server menu, and the profile
marked for automatic connection is restored on startup. Passwords remain session-only until
desktop secret-service storage is implemented:

```sh
./build/dev/src/app/trackknife
```

Trackbench is a separate executable with its own tabbed local lists, folder
library dock, and local transport:

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
cmake --preset tidy && cmake --build --preset tidy && ctest --preset tidy
cmake --preset release && cmake --build --preset release
```

## License

The Trackknife project (both applications) is licensed under the GNU General
Public License version 3 only
(`GPL-3.0-only`). See [`LICENSE`](LICENSE).
