// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"

#include <filesystem>
#include <string_view>

namespace trackknife::mpd {

// Performs the lexical, lossless part of MPD URI -> local path resolution.
// Filesystem identity and symlink containment must be revalidated when opening
// or mutating the returned path.
[[nodiscard]] core::Result<std::filesystem::path>
resolve_below_music_root(const std::filesystem::path& music_root, std::string_view mpd_uri);

} // namespace trackknife::mpd
