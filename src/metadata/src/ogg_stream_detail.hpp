// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace trackknife::metadata::ogg_stream_detail {

// The complete logical packets of one single-stream Ogg file, in order.
// Rewriting a Vorbis comment legitimately relayouts pages — lacing, page
// sequence numbers, and CRCs all change — so preservation is proven at the
// packet layer: every packet except the comment packet must be
// byte-identical between the source and the prepared copy (ADR-0114).
struct OggPacketStream {
    std::uint32_t serial_number{0U};
    std::vector<std::vector<unsigned char>> packets;
};

// Parses the packet stream, failing typed on anything the qualification
// cannot vouch for: a missing capture pattern, an unknown page version, a
// second bitstream serial (chained or multiplexed Ogg), or a truncated
// final packet.
[[nodiscard]] inline core::Result<OggPacketStream>
parse_packet_stream(const std::span<const unsigned char> bytes, const core::Error& malformed) {
    constexpr std::size_t page_header_size = 27U;
    OggPacketStream stream;
    std::vector<unsigned char> current;
    bool current_open = false;
    bool first_page = true;
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        if (bytes.size() - offset < page_header_size || bytes[offset] != 'O' ||
            bytes[offset + 1U] != 'g' || bytes[offset + 2U] != 'g' || bytes[offset + 3U] != 'S' ||
            bytes[offset + 4U] != 0U) {
            return std::unexpected(malformed);
        }
        const auto header_type = bytes[offset + 5U];
        const auto serial = static_cast<std::uint32_t>(bytes[offset + 14U]) |
                            (static_cast<std::uint32_t>(bytes[offset + 15U]) << 8U) |
                            (static_cast<std::uint32_t>(bytes[offset + 16U]) << 16U) |
                            (static_cast<std::uint32_t>(bytes[offset + 17U]) << 24U);
        if (first_page) {
            stream.serial_number = serial;
            first_page = false;
        } else if (serial != stream.serial_number) {
            return std::unexpected(malformed);
        }
        const auto continued = (header_type & 0x01U) != 0U;
        if (continued != current_open) {
            return std::unexpected(malformed);
        }
        const std::size_t segment_count = bytes[offset + 26U];
        auto lacing = offset + page_header_size;
        if (bytes.size() - lacing < segment_count) {
            return std::unexpected(malformed);
        }
        auto payload = lacing + segment_count;
        for (std::size_t segment = 0U; segment < segment_count; ++segment) {
            const std::size_t length = bytes[lacing + segment];
            if (bytes.size() - payload < length) {
                return std::unexpected(malformed);
            }
            current.insert(current.end(), bytes.begin() + static_cast<std::ptrdiff_t>(payload),
                           bytes.begin() + static_cast<std::ptrdiff_t>(payload + length));
            payload += length;
            current_open = true;
            if (length < 255U) {
                stream.packets.push_back(std::move(current));
                current.clear();
                current_open = false;
            }
        }
        offset = payload;
    }
    if (current_open) {
        return std::unexpected(malformed);
    }
    return stream;
}

} // namespace trackknife::metadata::ogg_stream_detail
