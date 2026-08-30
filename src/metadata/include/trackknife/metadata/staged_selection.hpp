// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/metadata/document.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace trackknife::metadata {

enum class MetadataSelectionFieldState : std::uint8_t {
    common,
    mixed,
    missing,
    partial,
};

[[nodiscard]] std::string_view
metadata_selection_field_state_name(MetadataSelectionFieldState state);

// Immutable baseline captured when a properties workspace opens. Later staged
// patches remain sparse and refer back to this document/revision; the list
// cache is never sufficient authority for commit.
struct StagedMetadataSource {
    std::string raw_path;
    std::optional<core::LocalSourceRevision> source_revision;
    MetadataDocument baseline;

    friend bool operator==(const StagedMetadataSource&, const StagedMetadataSource&) = default;
};

struct StagedMetadataCell {
    std::string native_name;
    std::vector<std::string> values;
    FieldProvenance provenance{FieldProvenance::embedded};

    friend bool operator==(const StagedMetadataCell&, const StagedMetadataCell&) = default;
};

struct StagedMetadataField {
    std::string canonical_name;
    std::string display_name;
    MetadataSelectionFieldState state{MetadataSelectionFieldState::missing};
    std::size_t present_item_count{0U};
    // The first occurrence containing this field. Aggregate presentations can
    // use it to display a common value without scanning the complete selection.
    std::optional<std::size_t> representative_item_index;

    friend bool operator==(const StagedMetadataField&, const StagedMetadataField&) = default;
};

struct StagedMetadataSelectionLimits {
    std::size_t items{100'000U};
    std::size_t fields{4'096U};
};

struct StagedMetadataSubsetField {
    MetadataSelectionFieldState state{MetadataSelectionFieldState::missing};
    std::size_t present_item_count{0U};
    std::optional<std::size_t> representative_item_index;

    friend bool operator==(const StagedMetadataSubsetField&,
                           const StagedMetadataSubsetField&) = default;
};

// Sparse multi-item metadata projection. Preferred fields appear first even
// when absent everywhere; arbitrary fields follow in first-seen selection and
// document order. Cells retain exact ordered values and winning provenance.
class StagedMetadataSelection final {
  public:
    [[nodiscard]] static core::Result<StagedMetadataSelection>
    create(std::vector<StagedMetadataSource> sources,
           std::span<const std::string_view> preferred_fields = {},
           const StagedMetadataSelectionLimits& limits = {});

    [[nodiscard]] std::size_t item_count() const noexcept { return items_->size(); }
    [[nodiscard]] std::size_t field_count() const noexcept { return fields_.size(); }
    [[nodiscard]] std::size_t distinct_source_count() const noexcept {
        return distinct_source_count_;
    }
    [[nodiscard]] std::size_t item_revision_count() const noexcept { return item_revision_count_; }
    [[nodiscard]] const StagedMetadataSource& source(std::size_t item_index) const;
    [[nodiscard]] const StagedMetadataField& field(std::size_t field_index) const;
    [[nodiscard]] const StagedMetadataCell* cell(std::size_t item_index,
                                                 std::size_t field_index) const;
    [[nodiscard]] std::span<const std::size_t> present_field_indexes(std::size_t item_index) const;
    [[nodiscard]] std::optional<std::size_t> field_index(std::string_view name) const;
    // Extends only the field vocabulary; item baselines remain shared and
    // immutable. Copy a published selection before calling this method.
    [[nodiscard]] core::Result<std::size_t>
    ensure_missing_field(std::string_view name, std::string_view display_name,
                         std::size_t maximum_fields = StagedMetadataSelectionLimits{}.fields);
    [[nodiscard]] core::Result<std::vector<StagedMetadataSubsetField>>
    summarize_items(std::span<const std::size_t> item_indexes) const;

  private:
    struct Item {
        StagedMetadataSource source;
        std::unordered_map<std::size_t, StagedMetadataCell> cells;
        std::vector<std::size_t> present_field_indexes;
    };

    std::shared_ptr<const std::vector<Item>> items_{std::make_shared<const std::vector<Item>>()};
    std::vector<StagedMetadataField> fields_;
    std::unordered_map<std::string, std::size_t> field_positions_;
    std::size_t distinct_source_count_{0U};
    std::size_t item_revision_count_{0U};
};

} // namespace trackknife::metadata
