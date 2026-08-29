// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/audio/local_playback.hpp"
#include "trackknife/audio/pipewire_output.hpp"
#include "trackknife/core/error.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/formats/decoder.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace trackknife::audio {

enum class LocalAuditionState {
    empty,
    loading,
    ready,
    buffering,
    playing,
    paused,
    draining,
    ended,
    failed,
};

struct LocalAuditionConfig {
    PlaybackBufferDurationConfig buffer;
    PipeWireOutputConfig output;
    std::chrono::milliseconds producer_period{5};
    std::size_t command_capacity{64U};

    friend bool operator==(const LocalAuditionConfig&, const LocalAuditionConfig&) = default;
};

struct LocalAuditionSnapshot {
    LocalAuditionState state{LocalAuditionState::empty};
    std::string raw_path;
    // Queued gapless continuation and the count of consumed takeovers; the
    // count increments exactly when raw_path flips to the queued source and
    // position/end rebase to the new track.
    std::string next_raw_path;
    std::uint64_t chain_transitions{0U};
    std::optional<formats::PcmFormat> format;
    std::int64_t position_sample{0};
    std::optional<std::int64_t> end_sample;
    std::size_t buffered_frames{0U};
    std::uint64_t underrun_count{0U};
    int volume_percent{100};
    std::optional<std::string> output_target;
    std::vector<PipeWireDevice> devices;
    PipeWireOutputSnapshot output;
    std::optional<core::Error> error;

    friend bool operator==(const LocalAuditionSnapshot&, const LocalAuditionSnapshot&) = default;
};

// Owns every blocking local-audio operation on one serialized worker. Public
// commands only enqueue bounded work and are safe to invoke from the UI thread.
class LocalAuditionService final {
  public:
    [[nodiscard]] static core::Result<std::unique_ptr<LocalAuditionService>>
    create(LocalAuditionConfig config = {});

    LocalAuditionService(const LocalAuditionService&) = delete;
    LocalAuditionService& operator=(const LocalAuditionService&) = delete;
    LocalAuditionService(LocalAuditionService&&) = delete;
    LocalAuditionService& operator=(LocalAuditionService&&) = delete;
    ~LocalAuditionService();

    [[nodiscard]] LocalAuditionSnapshot snapshot() const;

    [[nodiscard]] core::Result<void> load_and_play(std::string raw_path);
    // Queues the file to continue seamlessly when the current one ends. The
    // continuation must match the active PCM format exactly; on rejection the
    // snapshot's next_raw_path simply stays empty and the caller falls back
    // to an ordinary load at end-of-track. Seeks and loads drop the queue.
    [[nodiscard]] core::Result<void> queue_gapless_next(std::string raw_path);
    [[nodiscard]] core::Result<void> clear_gapless_next();
    [[nodiscard]] core::Result<void> play();
    [[nodiscard]] core::Result<void> pause();
    [[nodiscard]] core::Result<void> stop();
    [[nodiscard]] core::Result<void> seek_to_sample(std::int64_t target_sample);
    // Perceptual volume in percent [0, 100]; mapped cubically onto PipeWire's
    // linear stream mixer and reapplied when a new source connects.
    [[nodiscard]] core::Result<void> set_volume_percent(int percent);
    // Refreshes the snapshot's device list via one bounded registry roundtrip
    // on the worker.
    [[nodiscard]] core::Result<void> refresh_output_devices();
    // Selects the PipeWire sink for current and future sources; nullopt is the
    // system default. A loaded source reconnects in place, preserving its
    // position, volume, and play/pause state.
    [[nodiscard]] core::Result<void> set_output_target(std::optional<std::string> target);
    [[nodiscard]] core::Result<void> clear();

  private:
    struct Impl;
    explicit LocalAuditionService(std::unique_ptr<Impl> implementation);

    std::unique_ptr<Impl> implementation_;
};

} // namespace trackknife::audio
