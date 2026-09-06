// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/formats/decoder.hpp"

#include <atomic>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::lists {
inline constexpr std::size_t m3u8_max_bytes = 32U * 1024U * 1024U;
inline constexpr std::size_t m3u8_max_entries = 100'000U;
inline constexpr std::size_t m3u8_max_line = 64U * 1024U;

struct PlaylistEntry {
    std::string raw_path;
    std::string title;
    std::optional<std::int64_t> duration_ms;
    std::optional<std::string> logical_reference;
    formats::AudioSourceSelection selection;
    std::optional<formats::SampleRange> segment;
};

// Strict UTF-8, local file references only. The absolute playlist path anchors
// relative entries without resolving symlinks or collapsing '..'. Missing files
// and repeated references are retained. Errors reject the complete import.
[[nodiscard]] core::Result<std::vector<PlaylistEntry>>
parse_m3u8(std::string_view bytes, const std::string& absolute_playlist_path,
           const core::CancellationToken& cancellation = {},
           std::atomic<std::size_t>* progress = nullptr);
[[nodiscard]] core::Result<std::vector<PlaylistEntry>>
read_m3u8(const std::string& absolute_playlist_path,
          const core::CancellationToken& cancellation = {},
          std::atomic<std::size_t>* progress = nullptr);

// Whole-file references only. Paths beneath the output directory are relative;
// other paths remain absolute. Ambiguous/non-UTF-8 paths use escaped file URIs.
// Logical selections fail with one-based row context before any output is made.
[[nodiscard]] core::Result<std::string>
serialize_m3u8(std::span<const PlaylistEntry> entries, const std::string& absolute_playlist_path,
               const core::CancellationToken& cancellation = {},
               std::atomic<std::size_t>* progress = nullptr);

// Publishes a complete, fsynced sibling temporary file using an atomic hard link.
// Never replaces any existing entry (including a dangling symlink). Filesystems
// without hard-link support fail safely. Cancellation before publication leaves
// no destination; after publication success is final. Run file adapters off UI.
[[nodiscard]] core::Result<void> write_m3u8_new(const std::string& absolute_playlist_path,
                                                std::string_view bytes,
                                                const core::CancellationToken& cancellation = {});
} // namespace trackknife::lists
