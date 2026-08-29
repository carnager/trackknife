// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace trackknife::core {

class StableId final {
  public:
    static constexpr std::size_t byte_count = 16;
    using Bytes = std::array<std::uint8_t, byte_count>;

    constexpr StableId() = default;
    explicit constexpr StableId(Bytes bytes) : bytes_(bytes) {}

    [[nodiscard]] static StableId random();
    [[nodiscard]] static Result<StableId> parse(std::string_view text);

    [[nodiscard]] constexpr std::span<const std::uint8_t, byte_count> bytes() const noexcept {
        return bytes_;
    }
    [[nodiscard]] constexpr bool is_nil() const noexcept {
        for (const auto byte : bytes_) {
            if (byte != 0U) {
                return false;
            }
        }
        return true;
    }
    [[nodiscard]] std::string to_string() const;

    friend constexpr auto operator<=>(const StableId&, const StableId&) = default;

  private:
    Bytes bytes_{};
};

} // namespace trackknife::core
