# Trackknife

This repository builds **Trackbench**, a fast modern Qt 6 workspace with
separate MPD/Melody and local-file authorities (ADR-0058). MPD Queue and Local
Queue tabs share one visual language and complete track-view layout engine;
the active tab switches the sidebar, transport, and MPD/PipeWire output
selector. Local tagging and filesystem operations remain unavailable for MPD
rows. The standalone **Trackknife** MPD client remains as a compatibility shell
during migration.

The buildable foundation, `tkfmt-1` engine, asynchronous MPD backbone, M3 Qt
workspace, and Trackbench's local FFmpeg/PipeWire playback workspace
(sample-accurate logical tracks, bounded playback buffering, direct PipeWire
output) are implemented. M4 is complete; active M5 builds the fast metadata and
safe file-operation workspace. Start with
[`MILESTONES.md`](MILESTONES.md), then [`docs/README.md`](docs/README.md).

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

The MPD client executable is a pure Qt Widgets workspace with a tabbed left
Library pane for server navigation and playlists,
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

Trackbench is the primary executable with MPD and local queue contexts:

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
