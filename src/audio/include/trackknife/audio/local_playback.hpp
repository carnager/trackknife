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
    // Gapless continuation bookkeeping: whether a queued source is waiting to
    // take over at decode end, the produced-domain sample where the active
    // source began after a swap, and whether the consumer has crossed it.
    bool next_queued{false};
    std::optional<std::int64_t> chain_boundary_sample;
    bool chain_crossed{false};

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
    open_selected(std::string raw_path, formats::AudioSourceSelection selection,
                  PlaybackBufferDurationConfig buffer_config,
                  core::CancellationToken cancellation = {});
    [[nodiscard]] static core::Result<LocalPlayback>
    open_segment(std::string raw_path, formats::SampleRange range,
                 PlaybackBufferConfig buffer_config, core::CancellationToken cancellation = {});
    [[nodiscard]] static core::Result<LocalPlayback>
    open_segment(std::string raw_path, formats::SampleRange range,
                 PlaybackBufferDurationConfig buffer_config,
                 core::CancellationToken cancellation = {});
    [[nodiscard]] static core::Result<LocalPlayback>
    open_selected_segment(std::string raw_path, formats::AudioSourceSelection selection,
                          formats::SampleRange range, PlaybackBufferDurationConfig buffer_config,
                          core::CancellationToken cancellation = {});

    [[nodiscard]] const formats::PcmFormat& output_format() const noexcept;
    [[nodiscard]] const formats::SampleRange& sample_range() const noexcept;
    [[nodiscard]] LocalPlaybackSnapshot snapshot() const noexcept;

    [[nodiscard]] core::Result<void> play();
    void pause() noexcept;

    // Queues a source to continue seamlessly in the same ring the moment the
    // active source's decode ends. The queued source must match the active
    // output format exactly (rate, channels, layout); positions continue
    // monotonically in the produced domain past the recorded boundary. Only
    // one continuation may be queued at a time, and it must be queued before
    // the active decode ends.
    [[nodiscard]] core::Result<void> queue_next(std::string raw_path,
                                                core::CancellationToken cancellation = {});
    [[nodiscard]] core::Result<void> queue_next_selected(std::string raw_path,
                                                         formats::AudioSourceSelection selection,
                                                         core::CancellationToken cancellation = {});
    [[nodiscard]] core::Result<void> queue_next_segment(std::string raw_path,
                                                        formats::SampleRange range,
                                                        core::CancellationToken cancellation = {});
    [[nodiscard]] core::Result<void>
    queue_next_selected_segment(std::string raw_path, formats::AudioSourceSelection selection,
                                formats::SampleRange range,
                                core::CancellationToken cancellation = {});
    // Drops a queued continuation that has not yet taken over. A continuation
    // whose frames already entered the ring can no longer be withdrawn.
    void clear_next() noexcept;
    // Returns the boundary once the consumer has crossed into the queued
    // source and clears the crossing latch; the caller rebases its per-track
    // accounting on the returned produced-domain sample.
    [[nodiscard]] std::optional<std::int64_t> take_chain_crossing() noexcept;

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
