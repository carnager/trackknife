# ADR-0005: CMake, C++23, and a conservative Linux baseline

- Status: accepted
- Date: 2026-08-23
- Owners: Trackknife project

## Context

The development machine is a rolling Linux system with newer tools than many
users have. Its installed versions must not silently become Trackknife's
minimum. The project needs reproducible developer presets, distro packaging,
sanitizer builds, and a path to Flatpak without downloading dependencies during
ordinary configuration.

## Decision

- Use CMake 3.28 or newer and Ninja for documented developer builds.
- Require C++23 with GCC 13+ or Clang 18+.
- Require Qt 6.4 or newer for the desktop UI; use APIs available in that
  baseline unless an ADR deliberately raises it.
- Use dynamically linked system dependencies discovered through CMake or
  `pkg-config`. The normal build does not fetch source from the network.
- Treat Ubuntu 24.04 x86_64 with its distribution packages as the initial CI
  compatibility baseline. Also test current Clang on a rolling environment as
  CI capacity permits.
- Support Linux only initially. Avoid depending on a particular desktop shell.
  x86_64 is the first tested architecture; aarch64 is supported once CI and
  audio-device testing exist.
- Provide developer, sanitizer, and release CMake presets. CTest is the common
  unit/integration test entry point.
- Native packages are the development/reference path. Flatpak is the intended
  portable distribution path, but is implemented only after real runtime
  dependencies and portal behavior can be tested.

Version floors for media adapters are recorded when each adapter first links;
the selections in ADR-0006 do not make every backend an M0 build dependency.

## Consequences

- Contributors on older LTS distributions may need a newer compiler while still
  using distro Qt.
- Dependency updates remain visible to packagers and can be tested against
  distro patches.
- CMake target boundaries are the source of truth; include paths and compiler
  definitions are not process-global.

## Validation

- A clean environment configures, builds, and tests using documented presets.
- CI exercises both a normal compiler build and sanitizer build.
- Release builds do not contain sanitizer instrumentation or download code.

## Revisit when

- an actual packaging target cannot supply the baseline;
- a required Qt feature justifies raising the minimum;
- the supported architecture or operating-system scope expands.
