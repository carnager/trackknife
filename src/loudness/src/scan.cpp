// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/loudness/scan.hpp"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace trackknife::loudness {
namespace {

[[nodiscard]] core::Error scan_error(const core::ErrorCode code, std::string message,
                                     const std::string& raw_path = {}) {
    core::Error result{.code = code, .message = std::move(message), .context = {}};
    if (!raw_path.empty()) {
        result.context.push_back({.key = "path", .value = core::escape_raw_path(raw_path)});
    }
    return result;
}

struct ItemOutcome {
    std::optional<LoudnessAnalyzer> analyzer;
    std::optional<TrackLoudness> loudness;
    std::optional<core::LocalSourceRevision> revision;
    std::optional<core::Error> issue;
};

[[nodiscard]] ItemOutcome analyze_item(const LoudnessScanItem& item,
                                       const LoudnessScanOptions& options,
                                       const core::CancellationToken& cancellation) {
    ItemOutcome outcome;
    auto revision_before = core::observe_local_source_revision(item.raw_path);
    if (!revision_before) {
        outcome.issue = std::move(revision_before.error());
        return outcome;
    }
    auto decoder =
        item.range
            ? formats::AudioDecoder::open_selected_segment(item.raw_path, item.selection,
                                                           *item.range, cancellation)
            : formats::AudioDecoder::open_selected(item.raw_path, item.selection, cancellation);
    if (!decoder) {
        outcome.issue = std::move(decoder.error());
        return outcome;
    }
    const auto& format = decoder->output_format();
    auto analyzer =
        LoudnessAnalyzer::create(format.sample_rate, format.channels, options.measure_true_peak);
    if (!analyzer) {
        outcome.issue = std::move(analyzer.error());
        return outcome;
    }
    while (true) {
        if (cancellation.is_cancellation_requested()) {
            outcome.issue = scan_error(core::ErrorCode::cancelled, "loudness scan was cancelled",
                                       item.raw_path);
            return outcome;
        }
        auto chunk = decoder->next_chunk();
        if (!chunk) {
            outcome.issue = std::move(chunk.error());
            return outcome;
        }
        if (!*chunk) {
            break;
        }
        auto added = analyzer->add_frames((*chunk)->interleaved_samples);
        if (!added) {
            outcome.issue = std::move(added.error());
            return outcome;
        }
    }
    auto finished = analyzer->finish();
    if (!finished) {
        outcome.issue = std::move(finished.error());
        return outcome;
    }
    auto revision_after = core::observe_local_source_revision(item.raw_path);
    if (!revision_after) {
        outcome.issue = std::move(revision_after.error());
        return outcome;
    }
    if (*revision_after != *revision_before) {
        outcome.issue =
            scan_error(core::ErrorCode::conflict,
                       "the source changed while its loudness was being measured", item.raw_path);
        return outcome;
    }
    outcome.analyzer = std::move(*analyzer);
    outcome.loudness = *finished;
    outcome.revision = *revision_after;
    return outcome;
}

} // namespace

std::size_t LoudnessScanResult::analyzed_track_count() const noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(
        tracks, [](const auto& track) { return track.state == LoudnessScanState::analyzed; }));
}

std::size_t LoudnessScanResult::failed_track_count() const noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(
        tracks, [](const auto& track) { return track.state == LoudnessScanState::failed; }));
}

core::Result<LoudnessScanResult> scan_loudness(const std::span<const LoudnessScanItem> items,
                                               const LoudnessScanOptions& options,
                                               const LoudnessScanProgressCallback& progress,
                                               const core::CancellationToken& cancellation) {
    if (options.maximum_parallelism == 0U ||
        options.maximum_parallelism > maximum_scan_parallelism) {
        return std::unexpected(scan_error(core::ErrorCode::invalid_argument,
                                          "loudness scanning requires 1-16 workers"));
    }
    LoudnessScanResult result;
    result.tracks.reserve(items.size());
    for (const auto& item : items) {
        result.tracks.push_back(LoudnessTrackScan{
            .item_index = item.item_index,
            .raw_path = item.raw_path,
            .state = LoudnessScanState::pending,
            .loudness = std::nullopt,
            .source_revision = std::nullopt,
            .issue = std::nullopt,
        });
    }
    std::vector<std::optional<LoudnessAnalyzer>> analyzers(items.size());

    std::atomic_size_t next_item{0U};
    std::mutex progress_mutex;
    // Guarded by progress_mutex: the completed count and its delivery must
    // be one step so concurrent completions never publish out of order.
    std::size_t completed_items{0U};
    const auto report = [&](const std::size_t position, const LoudnessScanState state,
                            const bool terminal) {
        const std::scoped_lock lock{progress_mutex};
        if (terminal) {
            ++completed_items;
        }
        if (!progress) {
            return;
        }
        progress(LoudnessScanProgress{
            .item_index = items[position].item_index,
            .raw_path = items[position].raw_path,
            .state = state,
            .completed_items = completed_items,
            .total_items = items.size(),
        });
    };

    const auto worker = [&] {
        while (true) {
            const auto position = next_item.fetch_add(1U);
            if (position >= items.size()) {
                return;
            }
            auto& track = result.tracks[position];
            if (cancellation.is_cancellation_requested()) {
                track.state = LoudnessScanState::cancelled;
                track.issue = scan_error(core::ErrorCode::cancelled,
                                         "loudness scan was cancelled before this item started",
                                         track.raw_path);
                report(position, track.state, true);
                continue;
            }
            track.state = LoudnessScanState::running;
            report(position, track.state, false);
            auto outcome = analyze_item(items[position], options, cancellation);
            if (outcome.issue) {
                track.state = outcome.issue->code == core::ErrorCode::cancelled
                                  ? LoudnessScanState::cancelled
                                  : LoudnessScanState::failed;
                track.issue = std::move(outcome.issue);
            } else {
                track.state = LoudnessScanState::analyzed;
                track.loudness = outcome.loudness;
                track.source_revision = outcome.revision;
                analyzers[position] = std::move(outcome.analyzer);
            }
            report(position, track.state, true);
        }
    };
    {
        const auto worker_count =
            std::min(options.maximum_parallelism, std::max<std::size_t>(items.size(), 1U));
        std::vector<std::jthread> workers;
        workers.reserve(worker_count);
        for (std::size_t index = 0U; index < worker_count; ++index) {
            workers.emplace_back(worker);
        }
    }

    // Album reduction over the retained analyzer states, in first-seen
    // input order. Incomplete programmes report a warning instead of a
    // number computed from a partial album.
    std::map<std::string, std::size_t> album_positions;
    for (std::size_t position = 0U; position < items.size(); ++position) {
        const auto& key = items[position].album_key;
        if (!key) {
            continue;
        }
        const auto [entry, inserted] = album_positions.emplace(*key, result.albums.size());
        if (inserted) {
            result.albums.push_back(LoudnessAlbumScan{
                .album_key = *key,
                .item_indexes = {},
                .integrated_lufs = std::nullopt,
                .sample_peak = 0.0,
                .true_peak = std::nullopt,
                .issue = std::nullopt,
            });
        }
        auto& album = result.albums[entry->second];
        album.item_indexes.push_back(items[position].item_index);
        const auto& track = result.tracks[position];
        if (track.state != LoudnessScanState::analyzed || !track.loudness) {
            if (!album.issue) {
                album.issue = scan_error(
                    core::ErrorCode::invalid_argument,
                    "the album programme is incomplete; a member track was not analyzed");
            }
            continue;
        }
        album.sample_peak = std::max(album.sample_peak, track.loudness->sample_peak);
        if (track.loudness->true_peak) {
            album.true_peak = std::max(album.true_peak.value_or(0.0), *track.loudness->true_peak);
        }
    }
    for (auto& album : result.albums) {
        if (album.issue) {
            continue;
        }
        std::vector<const LoudnessAnalyzer*> members;
        members.reserve(album.item_indexes.size());
        for (std::size_t position = 0U; position < items.size(); ++position) {
            if (items[position].album_key == album.album_key && analyzers[position]) {
                members.push_back(&*analyzers[position]);
            }
        }
        auto integrated = album_integrated_lufs(members);
        if (!integrated) {
            album.issue = std::move(integrated.error());
            continue;
        }
        if (*integrated > -70.0 && *integrated < 70.0) {
            album.integrated_lufs = *integrated;
        } else {
            album.issue = scan_error(core::ErrorCode::unsupported,
                                     "the album programme is too short for gated loudness");
        }
    }

    result.cancellation_requested = cancellation.is_cancellation_requested() ||
                                    std::ranges::any_of(result.tracks, [](const auto& track) {
                                        return track.state == LoudnessScanState::cancelled;
                                    });
    return result;
}

} // namespace trackknife::loudness
