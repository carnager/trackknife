# ADR-0030: PipeWire device monitor and recovery

- Status: accepted
- Date: 2026-08-29
- Owners: Trackknife project
- Complements: ADR-0024 volume and device selection

## Context

ADR-0024 populated the output chooser with a bounded one-shot registry
roundtrip. Opening the menu refreshed it, but an already open Trackbench window
did not learn about added or removed sinks, and no policy defined what happened
to an active stream when its target disappeared. Repeating blocking registry
roundtrips also competes with decoder production on the serialized player
worker.

PipeWire exposes node add/remove events through its registry. The session
manager publishes the effective `default.audio.sink` as metadata and is
responsible for dynamically relinking an untargeted playback stream when the
system default changes.

## Decision

- A Qt-free `PipeWireDeviceMonitor` owns one persistent thread-loop, context,
  core, registry, and listener set. Its bounded initial synchronization
  collects `Audio/Sink` and `Audio/Duplex` node names/descriptions and binds the
  metadata object named `default`. Both the persistent and compatibility
  one-shot paths reject more than 256 projected output nodes.
- Registry node additions/removals and changes to `default.audio.sink`
  increment a monotonic generation. Monitor callbacks only update monitor-owned
  non-real-time state; they never touch a stream, decoder, or widget.
- The local-audition worker samples the immutable monitor snapshot at a bounded
  100 ms cadence, including while playback is paused or empty. It performs all
  resulting transport and stream transitions serially. The UI continues to
  read only `LocalAuditionSnapshot`.
- A system-default selection remains untargeted. Trackbench reflects the new
  default name but does not reconnect a healthy stream; the session manager's
  dynamic link policy owns that move.
- An explicit device choice is strict. If that node disappears, Trackbench does
  not silently play through another sink: it pauses the source, preserves its
  sample position, drops a prepared gapless successor, and removes the output
  adapter. Normal quiescence is attempted first; if the removed backend has
  already put the stream in an error state, adapter destruction stops and joins
  its thread loop before the source may change.
- When the explicit target returns, or a sink becomes available after the
  default graph was empty, Trackbench rebuilds the output around the paused
  source and reapplies volume. Playback does not resume automatically. A failed
  rebuild remains a recoverable device condition and retries no more than once
  per second; it does not turn the decoded source into a sticky playback
  failure.
- Snapshots distinguish target availability, device-triggered suspension,
  monitor failure, and output-recovery failure. The device menu keeps a checked
  unavailable explicit choice visible, while System default remains selectable.
  Header tooltips, status messages, and diagnostic window properties expose the
  same state.
- The chooser no longer starts a registry scan whenever it opens. The explicit
  Refresh action restarts the persistent monitor and its bounded initial sync.

## Alternatives considered

### Poll with repeated one-shot enumeration

Rejected. It repeatedly creates PipeWire connections and may block the same
worker that must keep the decoded ring fed. It also cannot observe default-sink
metadata precisely.

### Fall back silently when an explicit sink disappears

Rejected. Audio unexpectedly moving from headphones to speakers is more harmful
than a visible pause, and it changes the meaning of an explicit device choice.

### Resume automatically when the sink returns

Rejected. A device may return much later or in a different physical context.
Reconnecting paused preserves position without surprising the user with sound.

### Reconnect on every system-default change

Rejected. An untargeted autoconnect stream is already under session-manager
link policy. Rebuilding it introduces an unnecessary interruption and races the
component responsible for the move.

## Consequences

- The output menu stays current without UI-thread or repeated enumeration work.
- Explicit-target removal has deterministic, conservative behavior and a clear
  recovery path.
- The monitor is an additional long-lived PipeWire connection, independent of
  the current playback stream.
- A session manager that does not publish default metadata still gets live sink
  topology; the System default label simply has no resolved device subtitle.

## Validation

- The live PipeWire adapter regression connects the persistent monitor, checks
  its nonzero stable generation, sink projection, parsed default target when
  present, and clean destruction beside an active playback stream.
- The local-audition integration regression selects a deliberately absent
  explicit target, proves paused state and exact position retention with no
  output attached, then selects a real monitored target and proves a paused,
  available reconnection at the same position.
- Trackbench's offscreen tests retain the compact device chooser and observe
  availability, suspension, generation, and default-target properties without
  adding header chrome.

## Revisit when

- stream-to-device link inspection is needed to display the effective route in
  session managers that do not expose default metadata;
- device-clock projection or per-device buffer profiles are implemented;
- a controlled PipeWire test server is added for synthetic registry add/remove
  and metadata mutation tests.
