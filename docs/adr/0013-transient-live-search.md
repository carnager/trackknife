# ADR-0013: Use transient mixed live search with inline queue actions

- Status: accepted
- Date: 2026-08-25
- Owners: Trackknife project

## Context

Search has two distinct jobs. While typing, it should be a fast way to find an
album or track and immediately affect playback. After submission, it should be
a durable work surface whose rows can be selected, sorted, and used by later
file-oriented features. Treating both jobs as one permanent preview tab added
tab clutter and made common queue actions require extra UI transitions.

Melody's TUI demonstrates that albums and tracks can usefully share an
incremental search result list. Trackknife also requires native mouse and
keyboard parity, MusicBrainz-aware release grouping, and preserved independent
result tabs.

## Decision

- Typing in global search opens one transient, non-modal result panel anchored
  below the search field and outside the working-list tab strip. The panel
  overlays part of the workspace without replacing or changing the active
  queue/list tab; Escape closes it and returns focus to that still-visible work
  surface.
- Live results contain release-aware album groups and individual tracks.
  Albums use MusicBrainz release identity when present and deterministic tag
  fallbacks otherwise. Album rows show an immediate cover placeholder and load
  at most 32 covers asynchronously and serially from representative result
  URIs; decoding remains off the UI thread.
- Every actionable row exposes append, add-next, and replace-queue-and-play
  cells at its trailing edge.
- Mouse activation and keyboard activation operate on the same model rows.
  `Down` enters results, `Up` from the first result returns to the query,
  arrows reach the action cells, and Enter activates the focused action.
  Append is the default; one Right movement advances to add-next and another to
  replace. `Ctrl+Enter` directly replaces. Printable typing or Backspace from the result list returns
  focus to the query and continues editing it, so action shortcuts do not
  consume searchable characters.
- `Shift+Enter` from the query or results commits the current search to a named, independently
  closable classic result tab. The transient surface itself never occupies a
  tab.
- Both presentations consume typed models and controller methods; neither
  performs MPD protocol work or metadata parsing in a delegate.

## Consequences

- Immediate listening and queue construction do not create tabs, replace the
  current work surface, or open action dialogs.
- Search tabs remain deliberate working lists rather than transient navigation
  history.
- The live model needs result kinds and URI batches because an album action may
  target several tracks.
- Replace-and-play is a distinct controller operation and must preserve command
  ordering through the MPD session worker.
- Richer tag-qualified search syntax and result paging remain separate work;
  this decision does not define a query language.

## Validation

- Model tests cover release grouping, stable track order, section navigation,
  URI batches, action roles, and bounded serial artwork requests.
- Widget tests cover entering results from the query, horizontal action-cell
  navigation, returning to the query/work surface, and independent committed
  tabs.
- Hands-on testing must confirm icon clarity, native focus indication, action
  acknowledgement, and useful density with a real MPD/Melody library.

## Revisit when

- live search latency exceeds the workspace budgets on representative remote
  servers;
- accessibility testing shows that action cells cannot expose clear names and
  states;
- album actions need paged server-side expansion rather than the bounded URI
  set already present in a result.
