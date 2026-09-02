// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/loudness/analyzer.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace trackknife::loudness {

inline constexpr std::size_t maximum_scan_parallelism = 16U;

struct LoudnessScanItem {
    // Caller-owned identity carried through unchanged (Properties item
    // indexes, list positions, …).
    std::size_t item_index{0U};
    std::string raw_path;
    formats::AudioSourceSelection selection;
    // Cue-segment bounds; empty analyzes the whole source.
    std::optional<formats::SampleRange> range;
    // Items sharing a key form one album programme; empty means track-only.
    std::optional<std::string> album_key;

    friend bool operator==(const LoudnessScanItem&, const LoudnessScanItem&) = default;
};

struct LoudnessScanOptions {
    bool measure_true_peak{true};
    std::size_t maximum_parallelism{2U};
};

enum class LoudnessScanState : std::uint8_t { pending, running, analyzed, failed, cancelled };

struct LoudnessTrackScan {
    std::size_t item_index{0U};
    std::string raw_path;
    LoudnessScanState state{LoudnessScanState::pending};
    std::optional<TrackLoudness> loudness;
    // Observed before decoding and re-verified afterwards; application must
    // gate on this so results never reach a changed source.
    std::optional<core::LocalSourceRevision> source_revision;
    std::optional<core::Error> issue;
};

struct LoudnessAlbumScan {
    std::string album_key;
    std::vector<std::size_t> item_indexes;
    // Gated programme loudness across every member — set only when every
    // member analyzed and the programme is measurable.
    std::optional<double> integrated_lufs;
    double sample_peak{0.0};
    std::optional<double> true_peak;
    // Present when the programme is incomplete (a member failed or was
    // cancelled) — the grid's incomplete-album warning.
    std::optional<core::Error> issue;

    [[nodiscard]] std::optional<double> album_gain_db() const noexcept {
        return integrated_lufs ? std::optional{replaygain_reference_lufs - *integrated_lufs}
                               : std::nullopt;
    }
};

struct LoudnessScanProgress {
    std::size_t item_index{0U};
    std::string raw_path;
    LoudnessScanState state{LoudnessScanState::pending};
    std::size_t completed_items{0U};
    std::size_t total_items{0U};
};

using LoudnessScanProgressCallback = std::function<void(const LoudnessScanProgress&)>;

struct LoudnessScanResult {
    // Aligned with the input items.
    std::vector<LoudnessTrackScan> tracks;
    // One entry per distinct album key, in first-seen input order.
    std::vector<LoudnessAlbumScan> albums;
    bool cancellation_requested{false};

    [[nodiscard]] std::size_t analyzed_track_count() const noexcept;
    [[nodiscard]] std::size_t failed_track_count() const noexcept;
};

// Decodes and analyzes every item on a bounded worker pool (1–16 workers,
// one decoder per worker so codec threading never multiplies), isolates
// per-item failures, and reduces album groups from the retained analyzer
// states as one gated programme. Cancellation stops cleanly after the
// items already in flight.
[[nodiscard]] core::Result<LoudnessScanResult>
scan_loudness(std::span<const LoudnessScanItem> items, const LoudnessScanOptions& options = {},
              const LoudnessScanProgressCallback& progress = {},
              const core::CancellationToken& cancellation = {});

} // namespace trackknife::loudness
