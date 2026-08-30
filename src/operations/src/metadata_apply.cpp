// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/operations/metadata_apply.hpp"

#include "trackknife/core/local_sources.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <ranges>
#include <thread>
#include <utility>
#include <vector>

namespace trackknife::operations {
namespace {

constexpr std::size_t maximum_apply_parallelism = 8U;

[[nodiscard]] core::Error apply_error(const core::ErrorCode code, std::string message,
                                      const std::string& raw_path = {}) {
    core::Error result{.code = code, .message = std::move(message), .context = {}};
    if (!raw_path.empty()) {
        result.context.push_back({.key = "source", .value = core::escape_raw_path(raw_path)});
    }
    return result;
}

} // namespace

std::size_t MetadataApplyResult::committed_source_count() const noexcept {
    return static_cast<std::size_t>(std::ranges::count(sources, MetadataApplySourceState::committed,
                                                       &MetadataApplySourceResult::state));
}

std::size_t MetadataApplyResult::failed_source_count() const noexcept {
    return static_cast<std::size_t>(std::ranges::count(sources, MetadataApplySourceState::failed,
                                                       &MetadataApplySourceResult::state));
}

std::size_t MetadataApplyResult::cancelled_source_count() const noexcept {
    return static_cast<std::size_t>(std::ranges::count(sources, MetadataApplySourceState::cancelled,
                                                       &MetadataApplySourceResult::state));
}

core::Result<MetadataApplyResult> apply_metadata_write_plan(
    const metadata::MetadataWritePlan& plan, const MetadataApplySourceCommitter& committer,
    const MetadataApplyProgressCallback& progress, const core::CancellationToken& cancellation,
    const MetadataApplyOptions& options) {
    if (!plan.ready() || !committer || options.maximum_parallelism == 0U ||
        options.maximum_parallelism > maximum_apply_parallelism) {
        return std::unexpected(apply_error(
            core::ErrorCode::invalid_argument,
            "metadata Apply requires an entirely ready plan, a committer, and 1–8 workers"));
    }

    MetadataApplyResult result;
    result.sources.reserve(plan.sources.size());
    for (std::size_t index = 0U; index < plan.sources.size(); ++index) {
        result.sources.push_back(MetadataApplySourceResult{
            .source_index = index,
            .raw_path = plan.sources[index].raw_path,
            .state = MetadataApplySourceState::pending,
            .commit = std::nullopt,
            .issue = std::nullopt,
        });
    }

    std::atomic_size_t next_source{0U};
    std::atomic_size_t completed_sources{0U};
    std::mutex progress_mutex;
    const auto report = [&](const std::size_t index, const MetadataApplySourceState state,
                            const std::size_t completed,
                            const std::optional<core::Error>& issue = std::nullopt) {
        if (!progress) {
            return;
        }
        const MetadataApplyProgress update{
            .source_index = index,
            .raw_path = plan.sources[index].raw_path,
            .state = state,
            .completed_sources = completed,
            .total_sources = plan.sources.size(),
            .issue = issue,
        };
        std::scoped_lock lock{progress_mutex};
        progress(update);
    };
    const auto worker = [&] {
        while (!cancellation.is_cancellation_requested()) {
            const auto index = next_source.fetch_add(1U, std::memory_order_relaxed);
            if (index >= plan.sources.size()) {
                return;
            }
            auto& source_result = result.sources[index];
            if (cancellation.is_cancellation_requested()) {
                source_result.state = MetadataApplySourceState::cancelled;
                source_result.issue = apply_error(core::ErrorCode::cancelled,
                                                  "metadata Apply was cancelled before this source "
                                                  "started",
                                                  source_result.raw_path);
            } else {
                source_result.state = MetadataApplySourceState::running;
                report(index, source_result.state,
                       completed_sources.load(std::memory_order_relaxed));
                auto committed = committer(plan.sources[index], cancellation);
                if (committed) {
                    source_result.state = MetadataApplySourceState::committed;
                    source_result.commit = std::move(*committed);
                } else {
                    source_result.issue = std::move(committed.error());
                    source_result.state = source_result.issue->code == core::ErrorCode::cancelled
                                              ? MetadataApplySourceState::cancelled
                                              : MetadataApplySourceState::failed;
                }
            }
            const auto completed = completed_sources.fetch_add(1U, std::memory_order_relaxed) + 1U;
            report(index, source_result.state, completed, source_result.issue);
        }
    };

    const auto worker_count = std::min(options.maximum_parallelism, plan.sources.size());
    std::vector<std::jthread> workers;
    workers.reserve(worker_count);
    for (std::size_t index = 0U; index < worker_count; ++index) {
        workers.emplace_back(worker);
    }
    workers.clear(); // jthread destruction joins every bounded worker.

    for (auto& source_result : result.sources) {
        if (source_result.state != MetadataApplySourceState::pending) {
            continue;
        }
        source_result.state = MetadataApplySourceState::cancelled;
        source_result.issue = apply_error(core::ErrorCode::cancelled,
                                          "metadata Apply was cancelled before this source started",
                                          source_result.raw_path);
        const auto completed = completed_sources.fetch_add(1U, std::memory_order_relaxed) + 1U;
        report(source_result.source_index, source_result.state, completed, source_result.issue);
    }
    result.cancellation_requested =
        cancellation.is_cancellation_requested() || result.cancelled_source_count() > 0U;
    return result;
}

} // namespace trackknife::operations
