// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/metadata/staged_patch.hpp"
#include "trackknife/metadata/staged_selection.hpp"
#include "trackknife/metadata/transformation.hpp"

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace trackknife::metadata {

// M5 provider boundary (work item 7): a provider examines the draft-effective
// selection and returns typed proposals — ordered field values (identifiers
// are ordinary fields), artwork references, provenance, and confidence.
// Proposals never mutate anything themselves; the UI converts accepted ones
// into one ordinary staged draft transaction, so review, undo, discard, and
// the direct-apply safety chain are identical to hand-typed edits. Online
// providers (M6 MusicBrainz) implement this same contract.

struct MetadataProposalLimits {
    std::size_t items{1'000U};
    std::size_t fields_per_item{64U};
    std::size_t values_per_field{64U};
    std::size_t value_bytes{4'096U};
};

struct ProposedFieldValues {
    std::string canonical_field;
    std::string display_field;
    MetadataFieldMatchMode match_mode{MetadataFieldMatchMode::logical};
    // Ordered replacement for the complete field, exactly like a manual edit.
    std::vector<std::string> values;
    // [0, 1]; providers must not emit proposals they cannot stand behind.
    double confidence{0.0};
    // Human-readable reason shown as provenance detail.
    std::string rationale;

    friend bool operator==(const ProposedFieldValues&, const ProposedFieldValues&) = default;
};

// A picture the provider can point at but never fetches or writes itself;
// consumers route accepted references through the artwork operations.
struct ProposedArtworkReference {
    std::string label;
    std::string reference;
    bool remote{false};
    double confidence{0.0};

    friend bool operator==(const ProposedArtworkReference&,
                           const ProposedArtworkReference&) = default;
};

struct MetadataProposalItem {
    std::size_t item_index{0U};
    std::vector<ProposedFieldValues> fields;
    std::vector<ProposedArtworkReference> artwork;

    friend bool operator==(const MetadataProposalItem&, const MetadataProposalItem&) = default;
};

struct MetadataProposalSet {
    std::string provider_name;
    std::string provider_detail;
    std::vector<MetadataProposalItem> items;

    [[nodiscard]] std::size_t field_proposal_count() const noexcept;

    friend bool operator==(const MetadataProposalSet&, const MetadataProposalSet&) = default;
};

using MetadataProposalProvider = std::function<core::Result<MetadataProposalSet>(
    const StagedMetadataSelection&, const StagedMetadataPatchSet&, std::span<const std::size_t>,
    const core::CancellationToken&)>;

// Validates a proposal set against the selection and limits and converts it
// into a stageable preview. Proposals below minimum_confidence and proposals
// whose values equal the draft-effective state are dropped; the returned
// preview stages as one undo transaction.
[[nodiscard]] core::Result<MetadataTransformationPreview> metadata_proposal_preview(
    const StagedMetadataSelection& selection, const StagedMetadataPatchSet& draft,
    const MetadataProposalSet& proposals, double minimum_confidence,
    const core::CancellationToken& cancellation = {}, const MetadataProposalLimits& limits = {});

// Internal provider proving the boundary without any network: album groups
// are derived from the draft-effective selection, missing ALBUMARTIST values
// are proposed from exact in-group agreement, and TOTALTRACKS is proposed
// when the group's track numbers are exactly the contiguous run 1..N.
[[nodiscard]] core::Result<MetadataProposalSet> propose_selection_consistency(
    const StagedMetadataSelection& selection, const StagedMetadataPatchSet& draft,
    std::span<const std::size_t> item_indexes, const core::CancellationToken& cancellation = {});

} // namespace trackknife::metadata
