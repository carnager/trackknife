// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/formats/decoder.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace trackknife::audio {

enum class LocalPlaybackState {
    stopped,
    buffering,
    playing,
    paused,
    draining,
    ended,
    failed,
};

struct PlaybackBufferConfig {
    std::size_t capacity_frames{0U};
    std::size_t start_threshold_frames{0U};

    friend bool operator==(const PlaybackBufferConfig&, const PlaybackBufferConfig&) = default;
};

struct PlaybackBufferDurationConfig {
    std::chrono::milliseconds capacity{750};
    std::chrono::milliseconds start_threshold{100};

    friend bool operator==(const PlaybackBufferDurationConfig&,
                           const PlaybackBufferDurationConfig&) = default;
};

struct LocalPlaybackSnapshot {
    LocalPlaybackState state{LocalPlaybackState::stopped};
    std::int64_t position_sample{0};
    std::optional<std::int64_t> end_sample;
    std::size_t buffered_frames{0U};
    std::uint64_t underrun_count{0U};

    friend bool operator==(const LocalPlaybackSnapshot&, const LocalPlaybackSnapshot&) = default;
};

class LocalPlayback final {
  public:
    LocalPlayback(LocalPlayback&&) noexcept;
    LocalPlayback& operator=(LocalPlayback&&) noexcept;
    LocalPlayback(const LocalPlayback&) = delete;
    LocalPlayback& operator=(const LocalPlayback&) = delete;
    ~LocalPlayback();

    [[nodiscard]] static core::Result<LocalPlayback>
    open(std::string raw_path, PlaybackBufferConfig buffer_config,
         core::CancellationToken cancellation = {});
    [[nodiscard]] static core::Result<LocalPlayback>
    open(std::string raw_path, PlaybackBufferDurationConfig buffer_config,
         core::CancellationToken cancellation = {});
    [[nodiscard]] static core::Result<LocalPlayback>
    open_segment(std::string raw_path, formats::SampleRange range,
                 PlaybackBufferConfig buffer_config, core::CancellationToken cancellation = {});
    [[nodiscard]] static core::Result<LocalPlayback>
    open_segment(std::string raw_path, formats::SampleRange range,
                 PlaybackBufferDurationConfig buffer_config,
                 core::CancellationToken cancellation = {});

    [[nodiscard]] const formats::PcmFormat& output_format() const noexcept;
    [[nodiscard]] const formats::SampleRange& sample_range() const noexcept;
    [[nodiscard]] LocalPlaybackSnapshot snapshot() const noexcept;

    [[nodiscard]] core::Result<void> play();
    void pause() noexcept;

    // The output consumer must be quiesced before stop, seek, move, or
    // destruction; these operations reset producer/consumer queue state.
    [[nodiscard]] core::Result<void> stop();
    [[nodiscard]] core::Result<void> seek_to_sample(std::int64_t target_sample);

    // Called by the non-real-time producer. It performs bounded decode work and
    // fills at most one configured ring capacity per call.
    [[nodiscard]] core::Result<void> fill_buffer();

    // Called by the single real-time consumer. It performs no allocation,
    // locking, file I/O, or decode work. Unavailable frames are zero-filled.
    [[nodiscard]] std::size_t render(std::span<float> interleaved_output) noexcept;

  private:
    struct Impl;
    explicit LocalPlayback(std::unique_ptr<Impl> implementation);

    std::unique_ptr<Impl> implementation_;
};

} // namespace trackknife::audio
