// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/error.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/metadata/write_plan.hpp"
#include "trackknife/operations/metadata_commit.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::operations {

enum class MetadataApplySourceState : std::uint8_t {
    pending,
    running,
    committed,
    failed,
    cancelled,
};

struct MetadataApplySourceResult {
    std::size_t source_index{0U};
    std::string raw_path;
    MetadataApplySourceState state{MetadataApplySourceState::pending};
    std::optional<MetadataCommitResult> commit;
    std::optional<core::Error> issue;

    friend bool operator==(const MetadataApplySourceResult&,
                           const MetadataApplySourceResult&) = default;
};

struct MetadataApplyProgress {
    std::size_t source_index{0U};
    std::string raw_path;
    MetadataApplySourceState state{MetadataApplySourceState::pending};
    std::size_t completed_sources{0U};
    std::size_t total_sources{0U};
    std::optional<core::Error> issue;

    friend bool operator==(const MetadataApplyProgress&, const MetadataApplyProgress&) = default;
};

struct MetadataApplyResult {
    std::vector<MetadataApplySourceResult> sources;
    bool cancellation_requested{false};

    [[nodiscard]] std::size_t committed_source_count() const noexcept;
    [[nodiscard]] std::size_t failed_source_count() const noexcept;
    [[nodiscard]] std::size_t cancelled_source_count() const noexcept;

    friend bool operator==(const MetadataApplyResult&, const MetadataApplyResult&) = default;
};

struct MetadataApplyOptions {
    // Independent native-FLAC sources may prepare in parallel, while each
    // source retains the commit executor's physical/advisory serialization.
    std::size_t maximum_parallelism{2U};
};

using MetadataApplySourceCommitter = std::function<core::Result<MetadataCommitResult>(
    const metadata::MetadataWritePlanSource&, const core::CancellationToken&)>;
using MetadataApplyProgressCallback = std::function<void(const MetadataApplyProgress&)>;

// Applies an entirely ready immutable plan on a bounded worker pool. Runtime
// failures are per-source results: unrelated successful publications remain
// committed. Cancellation stops admission of new sources; a source already
// past its atomic publication boundary is allowed to finish coherently. The
// committer may be invoked concurrently and the progress callback is serialized.
[[nodiscard]] core::Result<MetadataApplyResult> apply_metadata_write_plan(
    const metadata::MetadataWritePlan& plan, const MetadataApplySourceCommitter& committer,
    const MetadataApplyProgressCallback& progress = {},
    const core::CancellationToken& cancellation = {}, const MetadataApplyOptions& options = {});

} // namespace trackknife::operations
