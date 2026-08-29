// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/error.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::core {

struct LocalSourceIssue {
    std::string raw_path;
    Error error;

    friend bool operator==(const LocalSourceIssue&, const LocalSourceIssue&) = default;
};

struct LocalSourceDiscovery {
    std::vector<std::string> raw_files;
    std::vector<LocalSourceIssue> issues;
    std::size_t visited_entries{0U};
    bool cancelled{false};
    bool truncated{false};
};

// Expands regular files and directories without interpreting raw Linux path
// bytes as UTF-8. Directory traversal is recursive, deterministic, and does
// not follow directory symlinks. Input order and duplicate occurrences are
// preserved.
[[nodiscard]] LocalSourceDiscovery discover_local_sources(std::span<const std::string> raw_paths,
                                                          const CancellationToken& cancellation,
                                                          std::size_t file_limit = 100'000U);

// Produces valid UTF-8 ASCII for presentation while retaining every raw byte.
[[nodiscard]] std::string escape_raw_path(std::string_view raw_path);

} // namespace trackknife::core
