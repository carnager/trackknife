// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/staged_selection.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace trackknife::metadata {

std::string_view metadata_selection_field_state_name(const MetadataSelectionFieldState state) {
    switch (state) {
    case MetadataSelectionFieldState::common:
        return "common";
    case MetadataSelectionFieldState::mixed:
        return "mixed";
    case MetadataSelectionFieldState::missing:
        return "missing";
    case MetadataSelectionFieldState::partial:
        return "partial";
    }
    return "missing";
}

core::Result<StagedMetadataSelection>
StagedMetadataSelection::create(std::vector<StagedMetadataSource> sources,
                                const std::span<const std::string_view> preferred_fields,
                                const StagedMetadataSelectionLimits& limits) {
    const auto limit_error = [](const std::string_view subject, const std::size_t limit) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::limit_exceeded,
            .message = "staged metadata " + std::string{subject} + " limit was exceeded",
            .context = {{.key = "limit", .value = std::to_string(limit)}},
        });
    };
    if (sources.size() > limits.items) {
        return limit_error("item", limits.items);
    }

    StagedMetadataSelection selection;
    std::vector<Item> items;
    items.reserve(sources.size());
    selection.field_positions_.reserve(std::min(preferred_fields.size(), limits.fields));
    std::vector<std::optional<std::vector<std::string>>> first_values;
    std::vector<bool> values_differ;
    first_values.reserve(std::min(preferred_fields.size(), limits.fields));
    values_differ.reserve(std::min(preferred_fields.size(), limits.fields));
    bool field_limit_hit = false;

    const auto add_field = [&selection, &first_values, &values_differ, &field_limit_hit, &limits](
                               const std::string_view name,
                               const std::string_view display_name) -> std::optional<std::size_t> {
        auto canonical_name = canonicalize_field_name(name);
        if (canonical_name.empty()) {
            return std::nullopt;
        }
        if (const auto existing = selection.field_positions_.find(canonical_name);
            existing != selection.field_positions_.end()) {
            return existing->second;
        }
        if (selection.fields_.size() == limits.fields) {
            field_limit_hit = true;
            return std::nullopt;
        }
        const auto position = selection.fields_.size();
        selection.field_positions_.emplace(canonical_name, position);
        selection.fields_.push_back(StagedMetadataField{
            .canonical_name = std::move(canonical_name),
            .display_name = std::string{display_name},
            .state = MetadataSelectionFieldState::missing,
            .present_item_count = 0U,
            .representative_item_index = std::nullopt,
        });
        first_values.emplace_back(std::nullopt);
        values_differ.push_back(false);
        return position;
    };
    for (const auto preferred : preferred_fields) {
        static_cast<void>(add_field(preferred, preferred));
        if (field_limit_hit) {
            return limit_error("field", limits.fields);
        }
    }

    std::unordered_set<std::string> distinct_sources;
    distinct_sources.reserve(sources.size());
    for (auto& source : sources) {
        distinct_sources.insert(source.raw_path);
        if (source.source_revision) {
            ++selection.item_revision_count_;
        }
        Item item{.source = std::move(source), .cells = {}, .present_field_indexes = {}};
        const auto effective = item.source.baseline.effective_fields();
        item.cells.reserve(effective.size());
        item.present_field_indexes.reserve(effective.size());
        for (const auto& field : effective) {
            const auto position =
                add_field(field.canonical_name,
                          field.native_name.empty() ? field.canonical_name : field.native_name);
            if (field_limit_hit) {
                return limit_error("field", limits.fields);
            }
            if (!position) {
                continue;
            }
            item.cells.emplace(*position, StagedMetadataCell{
                                              .native_name = field.native_name,
                                              .values = field.values,
                                              .provenance = field.provenance,
                                          });
            item.present_field_indexes.push_back(*position);
            auto& summary = selection.fields_[*position];
            if (!summary.representative_item_index) {
                summary.representative_item_index = items.size();
            }
            ++summary.present_item_count;
            if (!first_values[*position]) {
                first_values[*position] = field.values;
            } else if (*first_values[*position] != field.values) {
                values_differ[*position] = true;
            }
        }
        items.push_back(std::move(item));
    }
    selection.distinct_source_count_ = distinct_sources.size();

    for (std::size_t index = 0U; index < selection.fields_.size(); ++index) {
        auto& field = selection.fields_[index];
        if (field.present_item_count == 0U) {
            field.state = MetadataSelectionFieldState::missing;
        } else if (field.present_item_count != items.size()) {
            field.state = MetadataSelectionFieldState::partial;
        } else if (values_differ[index]) {
            field.state = MetadataSelectionFieldState::mixed;
        } else {
            field.state = MetadataSelectionFieldState::common;
        }
    }
    selection.items_ = std::make_shared<const std::vector<Item>>(std::move(items));
    return selection;
}

const StagedMetadataSource& StagedMetadataSelection::source(const std::size_t item_index) const {
    if (item_index >= items_->size()) {
        throw std::out_of_range{"staged metadata item index is out of range"};
    }
    return (*items_)[item_index].source;
}

const StagedMetadataField& StagedMetadataSelection::field(const std::size_t field_index) const {
    if (field_index >= fields_.size()) {
        throw std::out_of_range{"staged metadata field index is out of range"};
    }
    return fields_[field_index];
}

const StagedMetadataCell* StagedMetadataSelection::cell(const std::size_t item_index,
                                                        const std::size_t field_index) const {
    if (item_index >= items_->size()) {
        throw std::out_of_range{"staged metadata item index is out of range"};
    }
    static_cast<void>(field(field_index));
    const auto& item = (*items_)[item_index];
    const auto found = item.cells.find(field_index);
    return found == item.cells.end() ? nullptr : &found->second;
}

std::span<const std::size_t>
StagedMetadataSelection::present_field_indexes(const std::size_t item_index) const {
    if (item_index >= items_->size()) {
        throw std::out_of_range{"staged metadata item index is out of range"};
    }
    return (*items_)[item_index].present_field_indexes;
}

core::Result<std::size_t>
StagedMetadataSelection::ensure_missing_field(const std::string_view name,
                                              const std::string_view display_name,
                                              const std::size_t maximum_fields) {
    auto canonical_name = canonicalize_field_name(name);
    if (canonical_name.empty()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "metadata field name is empty after canonicalization",
            .context = {},
        });
    }
    if (const auto existing = field_positions_.find(canonical_name);
        existing != field_positions_.end()) {
        return existing->second;
    }
    if (fields_.size() >= maximum_fields) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::limit_exceeded,
            .message = "staged metadata field limit was exceeded",
            .context = {{.key = "limit", .value = std::to_string(maximum_fields)}},
        });
    }
    const auto field_index = fields_.size();
    field_positions_.emplace(canonical_name, field_index);
    fields_.push_back(StagedMetadataField{
        .canonical_name = std::move(canonical_name),
        .display_name = std::string{display_name.empty() ? name : display_name},
        .state = MetadataSelectionFieldState::missing,
        .present_item_count = 0U,
        .representative_item_index = std::nullopt,
    });
    return field_index;
}

core::Result<std::vector<StagedMetadataSubsetField>>
StagedMetadataSelection::summarize_items(const std::span<const std::size_t> item_indexes) const {
    std::vector<StagedMetadataSubsetField> summaries(fields_.size());
    std::vector<const std::vector<std::string>*> first_values(fields_.size(), nullptr);
    std::vector<bool> values_differ(fields_.size(), false);
    for (const auto item_index : item_indexes) {
        if (item_index >= items_->size()) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::invalid_argument,
                .message = "staged metadata subset item is out of range",
                .context = {{.key = "item", .value = std::to_string(item_index)}},
            });
        }
        const auto& item = (*items_)[item_index];
        for (const auto field_index : item.present_field_indexes) {
            const auto found = item.cells.find(field_index);
            if (found == item.cells.end()) {
                continue;
            }
            auto& summary = summaries[field_index];
            if (!summary.representative_item_index) {
                summary.representative_item_index = item_index;
                first_values[field_index] = &found->second.values;
            } else if (*first_values[field_index] != found->second.values) {
                values_differ[field_index] = true;
            }
            ++summary.present_item_count;
        }
    }
    for (std::size_t field_index = 0U; field_index < summaries.size(); ++field_index) {
        auto& summary = summaries[field_index];
        if (summary.present_item_count == 0U) {
            summary.state = MetadataSelectionFieldState::missing;
        } else if (summary.present_item_count != item_indexes.size()) {
            summary.state = MetadataSelectionFieldState::partial;
        } else if (values_differ[field_index]) {
            summary.state = MetadataSelectionFieldState::mixed;
        } else {
            summary.state = MetadataSelectionFieldState::common;
        }
    }
    return summaries;
}

std::optional<std::size_t> StagedMetadataSelection::field_index(const std::string_view name) const {
    const auto canonical_name = canonicalize_field_name(name);
    const auto found = field_positions_.find(canonical_name);
    return found == field_positions_.end() ? std::nullopt
                                           : std::optional<std::size_t>{found->second};
}

} // namespace trackknife::metadata
