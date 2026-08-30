// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/error.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/metadata/staged_patch.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::metadata {

enum class MetadataWritePlanIssueKind {
    missing_baseline_revision,
    inconsistent_baseline_revision,
    source_revalidation_failed,
    source_changed,
    physical_source_alias,
    conflicting_logical_edits,
    unresolved_non_embedded_target,
    writer_unavailable,
    preservation_unproven,
    unsupported_field_mapping,
};

[[nodiscard]] std::string_view metadata_write_plan_issue_kind_name(MetadataWritePlanIssueKind kind);

struct MetadataWritePlanIssue {
    MetadataWritePlanIssueKind kind{MetadataWritePlanIssueKind::source_revalidation_failed};
    core::Error error;
    std::optional<std::size_t> field_index;
    std::vector<std::size_t> item_indexes;
    bool blocking{true};

    friend bool operator==(const MetadataWritePlanIssue&, const MetadataWritePlanIssue&) = default;
};

struct MetadataWritePlanIntent {
    std::size_t item_index{0U};
    StagedMetadataPatchKind kind{StagedMetadataPatchKind::replace_values};
    std::vector<std::string> values;

    friend bool operator==(const MetadataWritePlanIntent&,
                           const MetadataWritePlanIntent&) = default;
};

struct MetadataWritePlanChange {
    std::size_t field_index{0U};
    std::string canonical_name;
    std::string display_name;
    // Fresh adapter-exposed spelling for an existing physical field. Empty for
    // a newly added field, whose display_name is resolved by the writer.
    std::string native_name;
    bool original_present{false};
    std::vector<std::string> original_values;
    // One entry per staged logical occurrence. Every intent is retained even
    // when several occurrences resolve to the same physical source.
    std::vector<MetadataWritePlanIntent> intents;
    bool conflicting_intents{false};
    bool unresolved_non_embedded_target{false};

    friend bool operator==(const MetadataWritePlanChange&,
                           const MetadataWritePlanChange&) = default;
};

struct MetadataWritePlanSource {
    std::string raw_path;
    // Every Properties occurrence referring to raw_path, including unstaged
    // duplicates that a successful physical write would need to refresh.
    std::vector<std::size_t> occurrence_indexes;
    std::optional<core::LocalSourceRevision> expected_revision;
    std::optional<core::LocalSourceRevision> observed_revision;
    std::string adapter_name;
    std::vector<MetadataWritePlanChange> changes;
    std::vector<MetadataWritePlanIssue> issues;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::size_t blocking_issue_count() const noexcept;

    friend bool operator==(const MetadataWritePlanSource&,
                           const MetadataWritePlanSource&) = default;
};

struct MetadataWritePlan {
    std::vector<MetadataWritePlanSource> sources;
    std::size_t patch_count{0U};

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::size_t ready_source_count() const noexcept;
    [[nodiscard]] std::size_t blocking_issue_count() const noexcept;

    friend bool operator==(const MetadataWritePlan&, const MetadataWritePlan&) = default;
};

using MetadataWritePlanReader = std::function<core::Result<LocalMetadataRead>(
    const std::string&, const core::CancellationToken&)>;

// Builds one immutable preview from a staged snapshot. The reader is invoked
// once per distinct raw path on the caller's worker thread. Per-source read,
// revision, capability, and logical-merge failures become visible blockers;
// cancellation and invalid planner input remain top-level errors.
[[nodiscard]] core::Result<MetadataWritePlan> build_metadata_write_plan(
    const StagedMetadataSelection& selection, const StagedMetadataPatchSet& patches,
    const MetadataWritePlanReader& reader, const core::CancellationToken& cancellation = {});

// Production convenience using the active bounded local metadata reader.
// Callers must dispatch this synchronous filesystem work off the UI thread.
[[nodiscard]] core::Result<MetadataWritePlan>
revalidate_metadata_write_plan(const StagedMetadataSelection& selection,
                               const StagedMetadataPatchSet& patches,
                               const core::CancellationToken& cancellation = {});

} // namespace trackknife::metadata
