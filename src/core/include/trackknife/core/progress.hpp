// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>

namespace trackknife::core {

struct Progress {
    std::uint64_t completed{0};
    std::optional<std::uint64_t> total;
    std::string stage;
    std::string detail;

    [[nodiscard]] std::optional<double> fraction() const noexcept {
        if (!total || *total == 0U) {
            return std::nullopt;
        }
        const auto value = static_cast<double>(completed) / static_cast<double>(*total);
        return std::clamp(value, 0.0, 1.0);
    }
};

} // namespace trackknife::core
