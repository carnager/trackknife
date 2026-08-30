// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/metadata/document.hpp"
#include "trackknife/titleformat/compiler.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::operations {

struct PolicyVersion {
    std::string name;
    std::uint32_t version{1U};

    friend bool operator==(const PolicyVersion&, const PolicyVersion&) = default;
};

struct OutputLayoutProfile {
    std::uint32_t schema_version{1U};
    std::string name;
    titleformat::DialectVersion dialect;
    std::string relative_directory_expression;
    std::string basename_expression;
    PolicyVersion sanitization_policy{"linux", 1U};

    friend bool operator==(const OutputLayoutProfile&, const OutputLayoutProfile&) = default;
};

struct DestinationProfile {
    std::uint32_t schema_version{1U};
    std::string name;
    // Raw absolute OS path. No UTF-8 interpretation occurs at this boundary.
    std::string root_raw_path;
    PolicyVersion containment_policy{"lexical-beneath-root", 1U};

    friend bool operator==(const DestinationProfile&, const DestinationProfile&) = default;
};

struct OutputPathOperationSelection {
    bool rename_files{false};
    bool move_files{false};

    friend bool operator==(const OutputPathOperationSelection&,
                           const OutputPathOperationSelection&) = default;
};

struct OutputPathPlanningItem {
    std::size_t item_index{0U};
    std::string source_raw_path;
    core::LocalSourceRevision source_revision;
    metadata::MetadataDocument final_metadata;
};

enum class ObservedOutputPathKind : std::uint8_t { file, directory, other };

struct ObservedOutputPath {
    std::string raw_path;
    ObservedOutputPathKind kind{ObservedOutputPathKind::file};
};

struct OutputPathPlanningSnapshot {
    std::vector<ObservedOutputPath> existing_paths;
    // Some mounted filesystems are case-insensitive. This first explicit
    // comparison policy folds ASCII only; a filesystem adapter must supply it.
    bool ascii_case_insensitive{false};
};

enum class OutputPathPlanIssueKind : std::uint8_t {
    invalid_source_path,
    expression_evaluation_failed,
    invalid_expression_output,
    absolute_relative_directory,
    component_too_long,
    path_too_long,
    containment_failure,
    shared_source_target_conflict,
    shared_source_revision_conflict,
    physical_source_alias,
    duplicate_target,
    existing_target,
    target_parent_not_directory,
    source_target_dependency,
    case_only_change,
};

[[nodiscard]] std::string_view output_path_plan_issue_kind_name(OutputPathPlanIssueKind kind);

struct OutputPathPlanIssue {
    OutputPathPlanIssueKind kind{OutputPathPlanIssueKind::invalid_source_path};
    bool blocking{true};
    std::string message;
    std::vector<std::size_t> item_indexes;
    std::optional<std::string> source_raw_path;
    std::optional<std::string> target_raw_path;

    friend bool operator==(const OutputPathPlanIssue&, const OutputPathPlanIssue&) = default;
};

struct PlannedOutputPathSource {
    std::string source_raw_path;
    core::LocalSourceRevision source_revision;
    std::string target_raw_path;
    std::string raw_relative_directory;
    std::string sanitized_relative_directory;
    std::string raw_basename;
    std::string sanitized_basename;
    std::vector<std::size_t> item_indexes;
    bool sanitized{false};
    bool no_change{false};

    friend bool operator==(const PlannedOutputPathSource&,
                           const PlannedOutputPathSource&) = default;
};

struct OutputPathPlan {
    OutputLayoutProfile layout;
    std::optional<DestinationProfile> destination;
    OutputPathOperationSelection operations;
    std::vector<PlannedOutputPathSource> sources;
    std::vector<OutputPathPlanIssue> issues;

    [[nodiscard]] bool ready() const noexcept;
};

struct OutputPathPlanningLimits {
    std::size_t items{100'000U};
    std::size_t issues{100'000U};
    std::size_t profile_name_bytes{1'024U};
    std::size_t expression_bytes{1U * 1'024U * 1'024U};
    std::size_t root_path_bytes{32U * 1'024U};
    std::size_t component_bytes{255U};
    std::size_t path_bytes{4'095U};
};

[[nodiscard]] core::Result<void>
validate_output_layout_profile(const OutputLayoutProfile& profile,
                               const OutputPathPlanningLimits& limits = {});

[[nodiscard]] core::Result<void>
validate_destination_profile(const DestinationProfile& profile,
                             const OutputPathPlanningLimits& limits = {});

// Pure planning boundary: callers provide the selected items, final metadata,
// and an explicit filesystem observation snapshot. The planner performs no I/O
// and never mutates a source.
[[nodiscard]] core::Result<OutputPathPlan> plan_output_paths(
    std::span<const OutputPathPlanningItem> items, OutputPathOperationSelection operations,
    OutputLayoutProfile layout, std::optional<DestinationProfile> destination,
    OutputPathPlanningSnapshot snapshot = {}, const core::CancellationToken& cancellation = {},
    const OutputPathPlanningLimits& limits = {});

} // namespace trackknife::operations
