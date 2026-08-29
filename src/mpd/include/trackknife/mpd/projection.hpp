// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/mpd/model.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace trackknife::mpd {

// Parses one or more MPD song records. A `file` pair starts each record.
// Repeated and unknown metadata pairs remain ordered.
[[nodiscard]] core::Result<std::vector<Track>> project_tracks(std::span<const Pair> pairs);

// Parses the heterogeneous, ordered result returned by `lsinfo` and
// `listplaylists`. Unknown per-entry fields remain attached to their entry.
[[nodiscard]] core::Result<std::vector<DatabaseEntry>>
project_database_entries(std::span<const Pair> pairs);

// Reconstructs the authoritative queue described by `plchanges`. Any
// inconsistent shape tells the caller to fall back to `playlistinfo`.
[[nodiscard]] core::Result<std::vector<Track>> apply_queue_changes(std::span<const Track> current,
                                                                   std::span<const Track> changed,
                                                                   std::size_t new_length);

// Parses MPD's status response without discarding fields added by a newer
// server. Unknown pairs remain available to future capability adapters.
[[nodiscard]] core::Result<PlaybackStatus> project_status(std::span<const Pair> pairs);

// Parses one or more MPD output records. An `outputid` pair starts each record.
// Unknown attributes are retained and Melody fields are projected separately.
[[nodiscard]] core::Result<std::vector<Output>> project_outputs(std::span<const Pair> pairs);

} // namespace trackknife::mpd
