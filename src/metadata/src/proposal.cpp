// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/proposal.hpp"

#include "trackknife/metadata/draft_document.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace trackknife::metadata {
namespace {

[[nodiscard]] core::Error proposal_error(const core::ErrorCode code, std::string message) {
    return core::Error{.code = code, .message = std::move(message), .context = {}};
}

[[nodiscard]] core::Error cancelled() {
    return proposal_error(core::ErrorCode::cancelled, "metadata proposal work was cancelled");
}

// Accepts "7", "07", and "7/12" track-number shapes; anything else is not a
// number this provider will reason about.
[[nodiscard]] std::optional<std::size_t> parse_track_number(const std::string_view text) {
    const auto slash = text.find('/');
    const auto digits = slash == std::string_view::npos ? text : text.substr(0U, slash);
    if (digits.empty() || digits.size() > 6U) {
        return std::nullopt;
    }
    std::size_t value = 0U;
    const auto* const begin = digits.data();
    const auto* const end = begin + digits.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value == 0U) {
        return std::nullopt;
    }
    return value;
}

// The values a write would actually compete with: the staged draft when one
// exists, otherwise only embedded-provenance baseline fields. Cached
// snapshots and stream projections can make a tag look present in the
// effective document while no writable tag exists in the file; treating such
// phantoms as satisfied silently skips exactly the files that need the tag.
[[nodiscard]] std::vector<std::string> writable_values(const StagedMetadataSelection& selection,
                                                       const StagedMetadataPatchSet& draft,
                                                       const std::size_t item_index,
                                                       const std::string& canonical_field) {
    auto field_index = selection.field_index(canonical_field);
    if (!field_index) {
        // Non-conventional names such as TRACKTOTAL live in the exact-native
        // registry once the draft ensures them; their patches count the same.
        field_index = selection.exact_native_field_index(canonical_field);
    }
    if (field_index) {
        if (const auto* patch = draft.patch(item_index, *field_index)) {
            return patch->kind == StagedMetadataPatchKind::remove_field ? std::vector<std::string>{}
                                                                        : patch->values;
        }
    }
    std::vector<std::string> values;
    for (const auto& field : selection.source(item_index).baseline.fields) {
        if (field.canonical_name == canonical_field &&
            field.provenance == FieldProvenance::embedded) {
            values.insert(values.end(), field.values.begin(), field.values.end());
        }
    }
    return values;
}

} // namespace

std::size_t MetadataProposalSet::field_proposal_count() const noexcept {
    std::size_t count = 0U;
    for (const auto& item : items) {
        count += item.fields.size();
    }
    return count;
}

core::Result<MetadataTransformationPreview> metadata_proposal_preview(
    const StagedMetadataSelection& selection, const StagedMetadataPatchSet& draft,
    const MetadataProposalSet& proposals, const double minimum_confidence,
    const core::CancellationToken& cancellation, const MetadataProposalLimits& limits) {
    if (proposals.provider_name.empty()) {
        return std::unexpected(
            proposal_error(core::ErrorCode::invalid_argument, "a proposal set names its provider"));
    }
    if (proposals.items.size() > limits.items) {
        return std::unexpected(proposal_error(core::ErrorCode::limit_exceeded,
                                              "the proposal set exceeds the item limit"));
    }

    std::vector<std::size_t> item_indexes;
    item_indexes.reserve(proposals.items.size());
    std::set<std::size_t> seen_items;
    for (const auto& item : proposals.items) {
        if (item.item_index >= selection.item_count() ||
            !seen_items.insert(item.item_index).second) {
            return std::unexpected(proposal_error(
                core::ErrorCode::invalid_argument,
                "proposal items must reference distinct items inside the selection"));
        }
        if (item.fields.size() > limits.fields_per_item) {
            return std::unexpected(proposal_error(core::ErrorCode::limit_exceeded,
                                                  "a proposal exceeds the per-item field limit"));
        }
        for (const auto& field : item.fields) {
            if (field.match_mode != MetadataFieldMatchMode::logical ||
                field.canonical_field.empty() ||
                canonicalize_field_name(field.display_field) != field.canonical_field ||
                field.confidence < 0.0 || field.confidence > 1.0) {
                return std::unexpected(proposal_error(core::ErrorCode::invalid_argument,
                                                      "a field proposal is malformed"));
            }
            if (field.values.size() > limits.values_per_field) {
                return std::unexpected(proposal_error(core::ErrorCode::limit_exceeded,
                                                      "a field proposal exceeds the value limit"));
            }
            for (const auto& value : field.values) {
                if (value.size() > limits.value_bytes) {
                    return std::unexpected(
                        proposal_error(core::ErrorCode::limit_exceeded,
                                       "a proposed value exceeds the per-value byte limit"));
                }
            }
        }
        item_indexes.push_back(item.item_index);
    }

    MetadataTransformationPreview preview{
        .chain =
            MetadataTransformationChain{
                .schema_version = 1U,
                .name = proposals.provider_name,
                .actions = {},
            },
        .item_indexes = item_indexes,
        .cells = {},
        .changed_item_count = 0U,
        .unchanged_present_cell_count = 0U,
        .unchanged_missing_cell_count = 0U,
    };
    if (item_indexes.empty()) {
        return preview;
    }

    auto documents = materialize_metadata_draft(selection, draft, item_indexes, cancellation);
    if (!documents) {
        return std::unexpected(std::move(documents.error()));
    }

    std::set<std::size_t> changed_items;
    for (std::size_t position = 0U; position < proposals.items.size(); ++position) {
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(cancelled());
        }
        const auto& item = proposals.items[position];
        const auto& document = (*documents)[position];
        std::set<std::string> addressed;
        for (const auto& field : item.fields) {
            if (!addressed.insert(field.canonical_field).second) {
                return std::unexpected(
                    proposal_error(core::ErrorCode::invalid_argument,
                                   "a proposal addresses the same field twice for one item"));
            }
            if (field.confidence < minimum_confidence) {
                continue;
            }
            auto before = document.effective_values(field.canonical_field);
            // Unchanged means the WRITABLE state already matches; an equal
            // cached or stream projection is not a written tag.
            if (writable_values(selection, draft, item.item_index, field.canonical_field) ==
                field.values) {
                if (before.empty()) {
                    ++preview.unchanged_missing_cell_count;
                } else {
                    ++preview.unchanged_present_cell_count;
                }
                continue;
            }
            preview.cells.push_back(MetadataTransformationCellPreview{
                .item_index = item.item_index,
                .last_action_index = 0U,
                .canonical_field = field.canonical_field,
                .display_field = field.display_field,
                .match_mode = MetadataFieldMatchMode::logical,
                .before = before.empty() ? std::nullopt : std::optional{std::move(before)},
                .after = field.values,
            });
            changed_items.insert(item.item_index);
        }
    }
    preview.changed_item_count = changed_items.size();
    return preview;
}

core::Result<MetadataProposalSet> propose_selection_consistency(
    const StagedMetadataSelection& selection, const StagedMetadataPatchSet& draft,
    const std::span<const std::size_t> item_indexes, const core::CancellationToken& cancellation) {
    for (const auto item_index : item_indexes) {
        if (item_index >= selection.item_count()) {
            return std::unexpected(
                proposal_error(core::ErrorCode::invalid_argument,
                               "selection-consistency proposals require in-range item indexes"));
        }
    }

    MetadataProposalSet proposals{
        .provider_name = "Selection consistency",
        .provider_detail = "Derived from agreement across the selected files",
        .items = {},
    };
    if (item_indexes.size() < 2U) {
        return proposals;
    }

    auto documents = materialize_metadata_draft(selection, draft, item_indexes, cancellation);
    if (!documents) {
        return std::unexpected(std::move(documents.error()));
    }

    // Album groups use the exact effective album value; files without an
    // album stay ungrouped and receive no proposals.
    std::map<std::string, std::vector<std::size_t>> groups;
    for (std::size_t position = 0U; position < item_indexes.size(); ++position) {
        const auto album = (*documents)[position].first_effective_value("album");
        if (album && !album->empty()) {
            groups[*album].push_back(position);
        }
    }

    std::map<std::size_t, MetadataProposalItem> proposal_items;
    const auto propose = [&proposal_items,
                          &item_indexes](const std::size_t position, std::string display_field,
                                         std::vector<std::string> values, std::string rationale) {
        auto& item = proposal_items[position];
        item.item_index = item_indexes[position];
        item.fields.push_back(ProposedFieldValues{
            .canonical_field = canonicalize_field_name(display_field),
            .display_field = std::move(display_field),
            .match_mode = MetadataFieldMatchMode::logical,
            .values = std::move(values),
            .confidence = 1.0,
            .rationale = std::move(rationale),
        });
    };

    for (const auto& [album, positions] : groups) {
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(cancelled());
        }
        if (positions.size() < 2U) {
            continue;
        }

        // ALBUMARTIST: exact agreement only, judged over what the user sees.
        // Either every visible album artist matches, or nobody has one and
        // every track shares the identical complete artist value list.
        std::optional<std::vector<std::string>> agreed_album_artist;
        auto album_artist_consistent = true;
        auto album_artist_missing = false;
        for (const auto position : positions) {
            auto values = (*documents)[position].effective_values("albumartist");
            if (values.empty()) {
                album_artist_missing = true;
                continue;
            }
            if (!agreed_album_artist) {
                agreed_album_artist = std::move(values);
            } else if (*agreed_album_artist != values) {
                album_artist_consistent = false;
                break;
            }
        }
        if (album_artist_consistent && !agreed_album_artist && album_artist_missing) {
            std::optional<std::vector<std::string>> agreed_artist;
            auto artist_consistent = true;
            for (const auto position : positions) {
                auto values = (*documents)[position].effective_values("artist");
                if (values.empty()) {
                    artist_consistent = false;
                    break;
                }
                if (!agreed_artist) {
                    agreed_artist = std::move(values);
                } else if (*agreed_artist != values) {
                    artist_consistent = false;
                    break;
                }
            }
            if (artist_consistent && agreed_artist) {
                agreed_album_artist = std::move(agreed_artist);
            }
        }
        if (album_artist_consistent && agreed_album_artist) {
            // Receiving a proposal is decided by the writable state, so a
            // value that only exists as a cached or stream projection still
            // gets the real tag.
            std::string artist_rationale{"Every track of \""};
            artist_rationale += album;
            artist_rationale += "\" in this selection shares this artist";
            for (const auto position : positions) {
                if (writable_values(selection, draft, item_indexes[position], "albumartist") ==
                    *agreed_album_artist) {
                    continue;
                }
                propose(position, "Album Artist", *agreed_album_artist, artist_rationale);
            }
        }

        // Track totals: only when the group's track numbers are exactly the
        // contiguous run 1..N for the N selected tracks.
        std::set<std::size_t> numbers;
        auto numbers_valid = true;
        for (const auto position : positions) {
            const auto text = (*documents)[position].first_effective_value("tracknumber");
            const auto number = text ? parse_track_number(*text) : std::nullopt;
            if (!number || !numbers.insert(*number).second) {
                numbers_valid = false;
                break;
            }
        }
        if (numbers_valid && numbers.size() == positions.size() &&
            *numbers.rbegin() == positions.size()) {
            const auto total = std::to_string(positions.size());
            const std::vector<std::string> total_values{total};
            std::string rationale{"\""};
            rationale += album;
            rationale += "\" has the complete run of tracks 1–";
            rationale += total;
            rationale += " in this selection";
            // One logical totals field: TRACKTOTAL resolves to the same
            // canonical identity on read, and the FLAC writer emits both
            // paired spellings on its own.
            for (const auto position : positions) {
                const auto item_index = item_indexes[position];
                if (writable_values(selection, draft, item_index, "totaltracks") != total_values) {
                    propose(position, "Total Tracks", total_values, rationale);
                }
            }
        }
    }

    proposals.items.reserve(proposal_items.size());
    for (auto& [position, item] : proposal_items) {
        proposals.items.push_back(std::move(item));
    }
    return proposals;
}

} // namespace trackknife::metadata
