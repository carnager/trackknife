// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace trackknife::core {

struct ContentFingerprint {
    std::array<std::uint8_t, 32> sha256{};

    friend bool operator==(const ContentFingerprint&, const ContentFingerprint&) = default;
};

struct Revision {
    std::uint64_t size_bytes{0};
    std::int64_t modified_time_ns{0};
    std::optional<ContentFingerprint> content_fingerprint;

    friend bool operator==(const Revision&, const Revision&) = default;
};

} // namespace trackknife::core
