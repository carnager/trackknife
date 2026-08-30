// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/draft_document.hpp"

#include "trackknife/metadata/flac_mapping.hpp"

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>

namespace trackknife::metadata {
namespace {

[[nodiscard]] core::Error draft_error(const core::ErrorCode code, std::string message) {
    return core::Error{.code = code, .message = std::move(message), .context = {}};
}

void apply_patch(MetadataDocument& document, const StagedMetadataField& field,
                 const StagedMetadataPatch& patch) {
    const auto exact_native = field.exact_native_name;
    const auto matches = [&field, &exact_native](const MetadataField& candidate) {
        return exact_native ? canonicalize_native_field_name(candidate.native_name) == *exact_native
                            : candidate.canonical_name == field.canonical_name;
    };
    const auto first = std::ranges::find_if(document.fields, matches);
    const auto insertion = static_cast<std::size_t>(std::distance(document.fields.begin(), first));
    document.fields.erase(std::remove_if(document.fields.begin(), document.fields.end(), matches),
                          document.fields.end());
    if (patch.kind == StagedMetadataPatchKind::remove_field) {
        return;
    }

    auto native_name = exact_native.value_or(field.display_name);
    auto canonical_name = exact_native ? resolve_text_property_identity(native_name).canonical_name
                                       : field.canonical_name;
    auto projected = MetadataField{
        .canonical_name = std::move(canonical_name),
        .native_name = std::move(native_name),
        .values = patch.values,
        .qualifier = {},
        .provenance = FieldProvenance::embedded,
    };
    document.fields.insert(document.fields.begin() + static_cast<std::ptrdiff_t>(std::min(
                                                         insertion, document.fields.size())),
                           std::move(projected));
}

} // namespace

core::Result<std::vector<MetadataDocument>> materialize_metadata_draft(
    const StagedMetadataSelection& selection, const StagedMetadataPatchSet& patches,
    const std::span<const std::size_t> item_indexes, const core::CancellationToken& cancellation) {
    for (std::size_t position = 0U; position < item_indexes.size(); ++position) {
        if (item_indexes[position] >= selection.item_count() ||
            (position > 0U && item_indexes[position - 1U] >= item_indexes[position])) {
            return std::unexpected(
                draft_error(core::ErrorCode::invalid_argument,
                            "metadata draft items must be in range, sorted, and unique"));
        }
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(
            draft_error(core::ErrorCode::cancelled, "metadata draft projection was cancelled"));
    }

    const auto staged = patches.patches();
    std::vector<MetadataDocument> documents;
    documents.reserve(item_indexes.size());
    std::size_t patch_position = 0U;
    for (const auto item_index : item_indexes) {
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(
                draft_error(core::ErrorCode::cancelled, "metadata draft projection was cancelled"));
        }
        while (patch_position < staged.size() && staged[patch_position].item_index < item_index) {
            ++patch_position;
        }
        auto document = selection.source(item_index).baseline;
        auto item_patch_position = patch_position;
        while (item_patch_position < staged.size() &&
               staged[item_patch_position].item_index == item_index) {
            const auto& patch = staged[item_patch_position++];
            if (patch.field_index >= selection.field_count()) {
                return std::unexpected(
                    draft_error(core::ErrorCode::invariant,
                                "metadata draft contains a field outside its staged selection"));
            }
            apply_patch(document, selection.field(patch.field_index), patch);
        }
        patch_position = item_patch_position;
        documents.push_back(std::move(document));
    }
    return documents;
}

} // namespace trackknife::metadata
