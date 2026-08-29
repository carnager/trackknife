// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::formats {

struct AudioStreamInfo {
    int stream_index{-1};
    std::string codec_name;
    std::string codec_description;
    std::string sample_format;
    std::string channel_layout;
    int sample_rate{0};
    int channels{0};
    std::int64_t bit_rate{0};

    friend bool operator==(const AudioStreamInfo&, const AudioStreamInfo&) = default;
};

struct MediaProbe {
    std::string raw_path;
    std::string container_names;
    std::string container_description;
    std::optional<std::int64_t> duration_ms;
    std::int64_t bit_rate{0};
    std::vector<AudioStreamInfo> audio_streams;
    std::optional<int> best_audio_stream;

    friend bool operator==(const MediaProbe&, const MediaProbe&) = default;
};

[[nodiscard]] core::Result<MediaProbe>
probe_local_media(const std::string& raw_path, const core::CancellationToken& cancellation = {});

} // namespace trackknife::formats
