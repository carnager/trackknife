// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::formats {

struct PcmFormat {
    int sample_rate{0};
    int channels{0};
    std::string channel_layout;

    friend bool operator==(const PcmFormat&, const PcmFormat&) = default;
};

struct PcmChunk {
    std::int64_t start_sample{0};
    std::vector<float> interleaved_samples;

    [[nodiscard]] std::size_t frame_count(const int channels) const noexcept {
        return channels > 0 ? interleaved_samples.size() / static_cast<std::size_t>(channels) : 0U;
    }
};

struct SampleRange {
    std::int64_t start_sample{0};
    std::optional<std::int64_t> end_sample;

    friend bool operator==(const SampleRange&, const SampleRange&) = default;
};

class AudioDecoder final {
  public:
    AudioDecoder(AudioDecoder&&) noexcept;
    AudioDecoder& operator=(AudioDecoder&&) noexcept;
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;
    ~AudioDecoder();

    [[nodiscard]] static core::Result<AudioDecoder> open(std::string raw_path,
                                                         core::CancellationToken cancellation = {});
    [[nodiscard]] static core::Result<AudioDecoder>
    open_segment(std::string raw_path, SampleRange range,
                 core::CancellationToken cancellation = {});

    [[nodiscard]] const PcmFormat& output_format() const noexcept;
    [[nodiscard]] std::optional<std::int64_t> duration_samples() const noexcept;
    [[nodiscard]] const SampleRange& sample_range() const noexcept;
    [[nodiscard]] core::Result<void> seek_to_sample(std::int64_t target_sample);
    [[nodiscard]] core::Result<std::optional<PcmChunk>> next_chunk();

  private:
    struct Impl;
    explicit AudioDecoder(std::unique_ptr<Impl> implementation);

    std::unique_ptr<Impl> implementation_;
};

} // namespace trackknife::formats
