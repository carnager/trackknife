// SPDX-License-Identifier: GPL-3.0-only

// Shared APEv2/ID3v1 trailer analysis for the prepared-copy writers whose
// containers carry tag trailers (WavPack today, MP3's optional trailing
// tags). Byte-exact binary-item accounting lives here exactly once.

#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/metadata/document.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace trackknife::metadata::apev2_trailer_detail {

inline constexpr std::size_t ape_footer_size = 32U;
inline constexpr std::size_t id3v1_size = 128U;
inline constexpr std::size_t maximum_ape_items = 4'096U;
inline constexpr std::uint32_t ape_header_present_flag = 0x8000'0000U;
inline constexpr std::uint32_t ape_item_kind_mask = 0x0000'0006U;

struct ApeItem {
    std::string key;
    std::vector<unsigned char> value;
    std::uint32_t flags{0U};
};

struct TrailerLayout {
    // Bytes before the trailing tags (the container's own data).
    std::size_t audio_end{0U};
    bool id3v1_present{false};
    // Every non-text APEv2 item (binary and external locators), by
    // case-folded key.
    std::map<std::string, ApeItem> preserved_items;
};

[[nodiscard]] inline std::uint32_t little_endian_32(const unsigned char* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

// Parses trailing tags from the end inward: an optional ID3v1 block, then an
// optional APEv2 tag whose non-text items are itemized for byte-exact
// comparison. Everything before them belongs to the container.
[[nodiscard]] inline core::Result<TrailerLayout>
parse_trailer_layout(const std::vector<unsigned char>& bytes, core::Error malformed_error) {
    TrailerLayout layout{.audio_end = bytes.size(), .id3v1_present = false, .preserved_items = {}};
    std::size_t trailer_end = bytes.size();
    if (trailer_end >= id3v1_size && bytes[trailer_end - id3v1_size] == 'T' &&
        bytes[trailer_end - id3v1_size + 1U] == 'A' &&
        bytes[trailer_end - id3v1_size + 2U] == 'G') {
        layout.id3v1_present = true;
        trailer_end -= id3v1_size;
        layout.audio_end = trailer_end;
    }
    if (trailer_end < ape_footer_size) {
        return layout;
    }
    const auto* footer = bytes.data() + trailer_end - ape_footer_size;
    static constexpr std::array<unsigned char, 8> preamble{'A', 'P', 'E', 'T', 'A', 'G', 'E', 'X'};
    if (!std::equal(preamble.begin(), preamble.end(), footer)) {
        return layout;
    }
    const auto tag_size = little_endian_32(footer + 12U);
    const auto item_count = little_endian_32(footer + 16U);
    const auto flags = little_endian_32(footer + 20U);
    const auto header_size = (flags & ape_header_present_flag) != 0U ? ape_footer_size : 0U;
    const std::uint64_t total = static_cast<std::uint64_t>(tag_size) + header_size;
    if (tag_size < ape_footer_size || total > trailer_end || item_count > maximum_ape_items) {
        return std::unexpected(std::move(malformed_error));
    }
    layout.audio_end = trailer_end - static_cast<std::size_t>(total);
    std::size_t cursor = layout.audio_end + header_size;
    const auto items_end = trailer_end - ape_footer_size;
    for (std::uint32_t index = 0U; index < item_count; ++index) {
        if (cursor + 8U > items_end) {
            return std::unexpected(std::move(malformed_error));
        }
        const auto value_size = little_endian_32(bytes.data() + cursor);
        const auto item_flags = little_endian_32(bytes.data() + cursor + 4U);
        cursor += 8U;
        const auto key_begin = cursor;
        while (cursor < items_end && bytes[cursor] != 0U) {
            ++cursor;
        }
        if (cursor >= items_end || value_size > items_end - cursor - 1U) {
            return std::unexpected(std::move(malformed_error));
        }
        std::string key{reinterpret_cast<const char*>(bytes.data() + key_begin),
                        cursor - key_begin};
        cursor += 1U;
        if ((item_flags & ape_item_kind_mask) != 0U) {
            ApeItem item{
                .key = std::move(key),
                .value = {bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                          bytes.begin() + static_cast<std::ptrdiff_t>(cursor + value_size)},
                .flags = item_flags,
            };
            auto folded = canonicalize_native_field_name(item.key);
            layout.preserved_items.emplace(std::move(folded), std::move(item));
        }
        cursor += value_size;
    }
    return layout;
}

// Compares the binary/external APEv2 items of two trailers byte-exactly.
[[nodiscard]] inline bool preserved_items_match(const TrailerLayout& source,
                                                const TrailerLayout& prepared) {
    if (source.preserved_items.size() != prepared.preserved_items.size()) {
        return false;
    }
    for (const auto& [key, item] : source.preserved_items) {
        const auto found = prepared.preserved_items.find(key);
        if (found == prepared.preserved_items.end() || found->second.value != item.value ||
            (found->second.flags & ape_item_kind_mask) != (item.flags & ape_item_kind_mask)) {
            return false;
        }
    }
    return true;
}

} // namespace trackknife::metadata::apev2_trailer_detail
