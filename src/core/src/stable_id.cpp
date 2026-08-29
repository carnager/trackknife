// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/core/stable_id.hpp"

#include <array>
#include <random>

namespace trackknife::core {
namespace {

[[nodiscard]] constexpr int hex_value(const char value) noexcept {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

} // namespace

StableId StableId::random() {
    thread_local std::mt19937_64 generator{std::random_device{}()};
    std::uniform_int_distribution<std::uint32_t> distribution{0U, 255U};

    Bytes bytes{};
    for (auto& byte : bytes) {
        byte = static_cast<std::uint8_t>(distribution(generator));
    }

    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U);
    return StableId{bytes};
}

Result<StableId> StableId::parse(const std::string_view text) {
    if (text.size() != 36U || text[8] != '-' || text[13] != '-' || text[18] != '-' ||
        text[23] != '-') {
        return std::unexpected(Error{ErrorCode::invalid_argument, "invalid stable ID shape", {}});
    }

    Bytes bytes{};
    std::size_t source = 0;
    std::size_t destination = 0;
    while (source < text.size()) {
        if (text[source] == '-') {
            ++source;
            continue;
        }
        if (source + 1U >= text.size() || destination >= bytes.size()) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "invalid stable ID length", {}});
        }

        const int high = hex_value(text[source]);
        const int low = hex_value(text[source + 1U]);
        if (high < 0 || low < 0) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "invalid stable ID hexadecimal digit", {}});
        }
        bytes[destination] = static_cast<std::uint8_t>((high << 4) | low);
        source += 2U;
        ++destination;
    }

    if (destination != bytes.size()) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "invalid stable ID bytes", {}});
    }
    return StableId{bytes};
}

std::string StableId::to_string() const {
    constexpr std::array<char, 16> digits{'0', '1', '2', '3', '4', '5', '6', '7',
                                          '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string result;
    result.reserve(36U);
    for (std::size_t index = 0; index < bytes_.size(); ++index) {
        if (index == 4U || index == 6U || index == 8U || index == 10U) {
            result.push_back('-');
        }
        const auto byte = bytes_[index];
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0FU]);
    }
    return result;
}

} // namespace trackknife::core
