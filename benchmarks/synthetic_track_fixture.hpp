// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string>

namespace trackknife::benchmarks {

struct SyntheticTrack {
    std::uint64_t index;
    std::uint64_t album_index;
    std::uint32_t track_number;
    std::uint32_t duration_seconds;
    std::string artist;
    std::string title;
    std::string album;
};

class SyntheticTrackFixture final {
  public:
    explicit constexpr SyntheticTrackFixture(const std::uint64_t size) : size_(size) {}

    [[nodiscard]] constexpr std::uint64_t size() const noexcept { return size_; }

    [[nodiscard]] SyntheticTrack at(const std::uint64_t index) const {
        const auto album_index = index / 11U;
        return SyntheticTrack{
            .index = index,
            .album_index = album_index,
            .track_number = static_cast<std::uint32_t>(index % 11U + 1U),
            .duration_seconds = static_cast<std::uint32_t>(145U + index % 235U),
            .artist = "Artist " + std::to_string(album_index % 8'192U),
            .title = "Synthetic track " + std::to_string(index + 1U),
            .album = "Album " + std::to_string(album_index + 1U),
        };
    }

  private:
    std::uint64_t size_;
};

} // namespace trackknife::benchmarks
