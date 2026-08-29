# ADR-0004: Use C++23 with a Qt-free core

- Status: accepted
- Date: 2026-08-23
- Owners: Trackknife project

## Context

Trackknife needs a native Qt 6 desktop interface, real-time audio code, broad C
and C++ media-library integration, deterministic parsing, and high-throughput
background work. A mixed Rust/C++ application could provide a memory-safe core,
but it would also put an FFI and two build ecosystems across nearly every early
feature while the project has one primary contributor.

## Decision

Use C++23 for the application, core, and adapters.

- Domain and service targets use the C++ standard library and must not expose Qt
  types in their public interfaces.
- Qt belongs in the UI/application shell and in narrowly scoped platform
  adapters where Qt is the actual external API.
- Ownership is explicit. Prefer values, RAII, immutable snapshots,
  `std::unique_ptr`, and `std::shared_ptr`; avoid unowned cross-thread pointers.
- Fallible core APIs return `trackknife::core::Result<T>` rather than throwing
  across subsystem boundaries. Exceptions from dependencies are caught at
  adapters and converted to structured errors.
- Background work receives cancellation and progress objects explicitly.
- No stable binary plugin ABI is promised. A future plugin boundary will be
  based on observed built-in extension points and versioned separately.

## Alternatives considered

### Rust core with a C++/Qt shell

Rust is attractive for parsing and concurrency safety, but a bridge would be
needed for metadata, decoding, playback, database values, jobs, and UI models.
That complexity arrives before it buys user-facing capability and makes
profiling and packaging harder for a small project.

### Rust with Qt bindings

This reduces handwritten FFI but adds a less direct toolkit integration and a
second layer of binding/version constraints around Qt's model/view APIs.

## Consequences

- Sanitizers, static analysis, warnings, fuzzing, and disciplined ownership are
  mandatory rather than optional hardening.
- Qt-free unit tests and command-line tools can exercise the core without a
  display server.
- Native library adapters and Qt models have straightforward debugging and
  profiling.
- A later language split remains possible behind the same adapter boundaries,
  but is not designed in prematurely.

## Validation

- The build graph rejects Qt usage in `trackknife_core` by never linking or
  publishing Qt includes to that target.
- CI builds with GCC and Clang and runs AddressSanitizer/UndefinedBehaviorSanitizer.
- Parser, query, and operation-plan code gains fuzz and property tests as those
  modules arrive.

## Revisit when

- measured defect or security data shows the approach is not sustainable;
- a subsystem has a compelling isolated Rust implementation with a small stable
  C boundary;
- the contributor/tooling profile changes materially.
