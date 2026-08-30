// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/write_plan.hpp"

#include "trackknife/metadata/flac_mapping.hpp"

#include <algorithm>
#include <map>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace trackknife::metadata {
namespace {

[[nodiscard]] core::Error planner_error(const core::ErrorCode code, std::string message,
                                        const std::string& raw_path = {}) {
    core::Error result{.code = code, .message = std::move(message), .context = {}};
    if (!raw_path.empty()) {
        result.context.push_back({.key = "path", .value = core::escape_raw_path(raw_path)});
    }
    return result;
}

void add_issue(MetadataWritePlanSource& source, const MetadataWritePlanIssueKind kind,
               core::Error error, const std::optional<std::size_t> field_index = std::nullopt,
               std::vector<std::size_t> item_indexes = {}) {
    source.issues.push_back(MetadataWritePlanIssue{
        .kind = kind,
        .error = std::move(error),
        .field_index = field_index,
        .item_indexes = std::move(item_indexes),
        .blocking = true,
    });
}

[[nodiscard]] bool same_intent(const MetadataWritePlanIntent& left,
                               const MetadataWritePlanIntent& right) {
    return left.kind == right.kind && left.values == right.values;
}

[[nodiscard]] bool is_non_embedded(const StagedMetadataSelection& selection,
                                   const MetadataWritePlanIntent& intent,
                                   const std::size_t field_index) {
    const auto* cell = selection.cell(intent.item_index, field_index);
    return cell != nullptr && cell->provenance != FieldProvenance::embedded;
}

[[nodiscard]] core::Result<MetadataWritePlan> cancelled() {
    return std::unexpected(core::Error{
        .code = core::ErrorCode::cancelled,
        .message = "metadata write-plan revalidation was cancelled",
        .context = {},
    });
}

} // namespace

std::string_view metadata_write_plan_issue_kind_name(const MetadataWritePlanIssueKind kind) {
    switch (kind) {
    case MetadataWritePlanIssueKind::missing_baseline_revision:
        return "missing baseline revision";
    case MetadataWritePlanIssueKind::inconsistent_baseline_revision:
        return "inconsistent baseline revision";
    case MetadataWritePlanIssueKind::source_revalidation_failed:
        return "source revalidation failed";
    case MetadataWritePlanIssueKind::source_changed:
        return "source changed";
    case MetadataWritePlanIssueKind::physical_source_alias:
        return "physical source alias";
    case MetadataWritePlanIssueKind::conflicting_logical_edits:
        return "conflicting logical edits";
    case MetadataWritePlanIssueKind::unresolved_non_embedded_target:
        return "unresolved non-embedded target";
    case MetadataWritePlanIssueKind::writer_unavailable:
        return "writer unavailable";
    case MetadataWritePlanIssueKind::preservation_unproven:
        return "preservation unproven";
    case MetadataWritePlanIssueKind::unsupported_field_mapping:
        return "unsupported field mapping";
    }
    return "source revalidation failed";
}

bool MetadataWritePlanSource::ready() const noexcept {
    return std::ranges::none_of(issues, [](const auto& issue) { return issue.blocking; });
}

std::size_t MetadataWritePlanSource::blocking_issue_count() const noexcept {
    return static_cast<std::size_t>(
        std::ranges::count_if(issues, [](const auto& issue) { return issue.blocking; }));
}

bool MetadataWritePlan::ready() const noexcept {
    return !sources.empty() && std::ranges::all_of(sources, &MetadataWritePlanSource::ready);
}

std::size_t MetadataWritePlan::ready_source_count() const noexcept {
    return static_cast<std::size_t>(
        std::ranges::count_if(sources, &MetadataWritePlanSource::ready));
}

std::size_t MetadataWritePlan::blocking_issue_count() const noexcept {
    std::size_t count = 0U;
    for (const auto& source : sources) {
        count += source.blocking_issue_count();
    }
    return count;
}

core::Result<MetadataWritePlan> build_metadata_write_plan(
    const StagedMetadataSelection& selection, const StagedMetadataPatchSet& patches,
    const MetadataWritePlanReader& reader, const core::CancellationToken& cancellation) {
    if (cancellation.is_cancellation_requested()) {
        return cancelled();
    }
    if (!reader) {
        return std::unexpected(planner_error(core::ErrorCode::invalid_argument,
                                             "metadata write-plan reader is not configured"));
    }
    if (patches.empty()) {
        return std::unexpected(planner_error(core::ErrorCode::invalid_argument,
                                             "metadata write plan requires staged changes"));
    }

    MetadataWritePlan plan{.sources = {}, .patch_count = patches.patch_count()};
    std::unordered_map<std::string, std::size_t> source_positions;
    source_positions.reserve(std::min(patches.patch_count(), selection.item_count()));
    std::vector<std::unordered_map<std::size_t, std::size_t>> change_positions;

    const auto staged_patches = patches.patches();
    for (const auto& patch : staged_patches) {
        if (cancellation.is_cancellation_requested()) {
            return cancelled();
        }
        if (patch.item_index >= selection.item_count() ||
            patch.field_index >= selection.field_count()) {
            return std::unexpected(
                planner_error(core::ErrorCode::invariant,
                              "metadata write plan contains a patch outside its staged selection"));
        }
        const auto& staged_source = selection.source(patch.item_index);
        auto [source_position, inserted] =
            source_positions.emplace(staged_source.raw_path, plan.sources.size());
        if (inserted) {
            plan.sources.push_back(MetadataWritePlanSource{
                .raw_path = staged_source.raw_path,
                .occurrence_indexes = {},
                .expected_revision = std::nullopt,
                .observed_revision = std::nullopt,
                .adapter_name = {},
                .changes = {},
                .issues = {},
            });
            change_positions.emplace_back();
        }
        auto& source = plan.sources[source_position->second];
        auto& positions = change_positions[source_position->second];
        auto [change_position, change_inserted] =
            positions.emplace(patch.field_index, source.changes.size());
        if (change_inserted) {
            const auto& field = selection.field(patch.field_index);
            source.changes.push_back(MetadataWritePlanChange{
                .field_index = patch.field_index,
                .canonical_name = field.canonical_name,
                .display_name = field.display_name,
                .native_name = {},
                .original_present = false,
                .original_values = {},
                .intents = {},
                .conflicting_intents = false,
                .unresolved_non_embedded_target = false,
            });
        }
        source.changes[change_position->second].intents.push_back(MetadataWritePlanIntent{
            .item_index = patch.item_index,
            .kind = patch.kind,
            .values = patch.values,
        });
    }

    for (std::size_t item_index = 0U; item_index < selection.item_count(); ++item_index) {
        const auto found = source_positions.find(selection.source(item_index).raw_path);
        if (found != source_positions.end()) {
            plan.sources[found->second].occurrence_indexes.push_back(item_index);
        }
    }

    for (auto& source : plan.sources) {
        std::optional<core::LocalSourceRevision> expected;
        std::vector<std::size_t> missing_revision_items;
        std::vector<std::size_t> inconsistent_revision_items;
        for (const auto& change : source.changes) {
            for (const auto& intent : change.intents) {
                const auto& revision = selection.source(intent.item_index).source_revision;
                if (!revision) {
                    missing_revision_items.push_back(intent.item_index);
                } else if (!expected) {
                    expected = revision;
                } else if (*expected != *revision) {
                    inconsistent_revision_items.push_back(intent.item_index);
                }
            }
        }
        std::ranges::sort(missing_revision_items);
        missing_revision_items.erase(
            std::unique(missing_revision_items.begin(), missing_revision_items.end()),
            missing_revision_items.end());
        std::ranges::sort(inconsistent_revision_items);
        inconsistent_revision_items.erase(
            std::unique(inconsistent_revision_items.begin(), inconsistent_revision_items.end()),
            inconsistent_revision_items.end());
        source.expected_revision = expected;
        if (!missing_revision_items.empty()) {
            add_issue(source, MetadataWritePlanIssueKind::missing_baseline_revision,
                      planner_error(core::ErrorCode::conflict,
                                    "a staged occurrence has no captured source revision",
                                    source.raw_path),
                      std::nullopt, std::move(missing_revision_items));
        }
        if (!inconsistent_revision_items.empty()) {
            add_issue(source, MetadataWritePlanIssueKind::inconsistent_baseline_revision,
                      planner_error(core::ErrorCode::conflict,
                                    "staged occurrences disagree about the source revision",
                                    source.raw_path),
                      std::nullopt, std::move(inconsistent_revision_items));
        }

        for (auto& change : source.changes) {
            const auto& first = change.intents.front();
            change.conflicting_intents =
                std::ranges::any_of(change.intents, [&first](const auto& intent) {
                    return !same_intent(first, intent);
                });
            std::vector<std::size_t> intent_items;
            intent_items.reserve(change.intents.size());
            for (const auto& intent : change.intents) {
                intent_items.push_back(intent.item_index);
                change.unresolved_non_embedded_target =
                    change.unresolved_non_embedded_target ||
                    is_non_embedded(selection, intent, change.field_index);
            }
            if (change.conflicting_intents) {
                add_issue(source, MetadataWritePlanIssueKind::conflicting_logical_edits,
                          planner_error(
                              core::ErrorCode::conflict,
                              "logical occurrences stage different results for one physical field",
                              source.raw_path),
                          change.field_index, intent_items);
            }
            if (change.unresolved_non_embedded_target) {
                add_issue(
                    source, MetadataWritePlanIssueKind::unresolved_non_embedded_target,
                    planner_error(core::ErrorCode::unsupported,
                                  "the effective field is not embedded in the physical source",
                                  source.raw_path),
                    change.field_index, std::move(intent_items));
            }
        }
    }

    for (auto& source : plan.sources) {
        if (cancellation.is_cancellation_requested()) {
            return cancelled();
        }
        auto read = reader(source.raw_path, cancellation);
        if (!read) {
            if (read.error().code == core::ErrorCode::cancelled ||
                cancellation.is_cancellation_requested()) {
                return cancelled();
            }
            add_issue(source, MetadataWritePlanIssueKind::source_revalidation_failed,
                      std::move(read.error()));
            continue;
        }
        if (read->raw_path != source.raw_path) {
            add_issue(source, MetadataWritePlanIssueKind::source_revalidation_failed,
                      planner_error(core::ErrorCode::invariant,
                                    "metadata reader returned a different raw source path",
                                    source.raw_path));
            continue;
        }
        source.observed_revision = read->source_revision;
        source.adapter_name = read->adapter_name;
        if (source.expected_revision && *source.expected_revision != read->source_revision) {
            add_issue(source, MetadataWritePlanIssueKind::source_changed,
                      planner_error(core::ErrorCode::conflict,
                                    "the source changed after the metadata draft was captured",
                                    source.raw_path));
        }

        const auto effective = read->document.effective_fields();
        std::unordered_map<std::string, const EffectiveMetadataField*> effective_positions;
        effective_positions.reserve(effective.size());
        for (const auto& field : effective) {
            effective_positions.emplace(field.canonical_name, &field);
        }
        for (auto& change : source.changes) {
            if (const auto found = effective_positions.find(change.canonical_name);
                found != effective_positions.end()) {
                change.original_present = true;
                change.native_name = found->second->native_name;
                change.original_values = found->second->values;
            }
        }

        if (!read->capabilities.fields_writable) {
            add_issue(source, MetadataWritePlanIssueKind::writer_unavailable,
                      planner_error(core::ErrorCode::unsupported,
                                    "the selected metadata adapter has no proven field writer",
                                    source.raw_path));
        }
        if (!read->capabilities.unknown_data_preserved_on_write) {
            add_issue(source, MetadataWritePlanIssueKind::preservation_unproven,
                      planner_error(core::ErrorCode::unsupported,
                                    "unknown container data preservation is not proven",
                                    source.raw_path));
        }
        if (read->adapter_name == "taglib-flac-v1") {
            std::unordered_set<std::string> changed_fields;
            changed_fields.reserve(source.changes.size());
            for (const auto& change : source.changes) {
                changed_fields.insert(change.canonical_name);
            }
            const auto unrepresentable_untouched =
                std::ranges::find_if(effective, [&changed_fields](const auto& field) {
                    return !changed_fields.contains(field.canonical_name) &&
                           (field.values.empty() ||
                            std::ranges::any_of(field.values,
                                                [](const auto& value) { return value.empty(); }));
                });
            if (unrepresentable_untouched != effective.end()) {
                add_issue(
                    source, MetadataWritePlanIssueKind::unsupported_field_mapping,
                    planner_error(
                        core::ErrorCode::unsupported,
                        "an untouched FLAC field contains an exact empty value that the writer "
                        "cannot preserve",
                        source.raw_path));
            }
            for (const auto& change : source.changes) {
                if (change.intents.empty() || change.conflicting_intents) {
                    continue;
                }
                const auto& intent = change.intents.front();
                auto mapping = map_flac_text_field(change.canonical_name, change.display_name,
                                                   change.native_name, intent.kind, intent.values);
                if (!mapping) {
                    auto mapping_error = std::move(mapping.error());
                    mapping_error.context.push_back(
                        {.key = "path", .value = core::escape_raw_path(source.raw_path)});
                    add_issue(source, MetadataWritePlanIssueKind::unsupported_field_mapping,
                              std::move(mapping_error), change.field_index, [&change] {
                                  std::vector<std::size_t> indexes;
                                  indexes.reserve(change.intents.size());
                                  for (const auto& item : change.intents) {
                                      indexes.push_back(item.item_index);
                                  }
                                  return indexes;
                              }());
                }
            }
        }
    }

    std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<std::size_t>> physical_sources;
    for (std::size_t index = 0U; index < plan.sources.size(); ++index) {
        const auto& revision = plan.sources[index].observed_revision;
        if (revision) {
            physical_sources[{revision->device, revision->inode}].push_back(index);
        }
    }
    for (const auto& [identity, aliases] : physical_sources) {
        static_cast<void>(identity);
        if (aliases.size() < 2U) {
            continue;
        }
        for (const auto source_index : aliases) {
            auto& source = plan.sources[source_index];
            add_issue(source, MetadataWritePlanIssueKind::physical_source_alias,
                      planner_error(core::ErrorCode::conflict,
                                    "another staged raw path resolves to the same physical source",
                                    source.raw_path));
        }
    }

    return plan;
}

core::Result<MetadataWritePlan>
revalidate_metadata_write_plan(const StagedMetadataSelection& selection,
                               const StagedMetadataPatchSet& patches,
                               const core::CancellationToken& cancellation) {
    return build_metadata_write_plan(
        selection, patches,
        [](const std::string& raw_path, const core::CancellationToken& token) {
            return read_local_metadata(raw_path, token);
        },
        cancellation);
}

} // namespace trackknife::metadata
