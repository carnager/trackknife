// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::metadata {

enum class MetadataFieldSuggestionKind : std::uint8_t {
    present,
    recent,
    conventional,
    musicbrainz,
};

struct MetadataFieldSuggestionCandidate {
    std::string_view display_name;
    MetadataFieldSuggestionKind kind{MetadataFieldSuggestionKind::conventional};
};

struct MetadataFieldSuggestion {
    std::string display_name;
    std::string canonical_name;
    MetadataFieldSuggestionKind kind{MetadataFieldSuggestionKind::conventional};

    friend bool operator==(const MetadataFieldSuggestion&,
                           const MetadataFieldSuggestion&) = default;
};

// Conventional and MusicBrainz names supplied by Trackknife. Arbitrary names
// remain valid and never need to appear in this catalog before use.
[[nodiscard]] std::span<const MetadataFieldSuggestionCandidate>
metadata_field_suggestion_catalog() noexcept;

// Deterministic ASCII-aware discovery ranking. Exact, prefix, substring, and
// ordered-subsequence matches are returned in that order; separator spelling
// does not affect matching, but present freeform and semantic addresses remain
// distinct results.
[[nodiscard]] std::vector<MetadataFieldSuggestion>
suggest_metadata_field_names(std::string_view query,
                             std::span<const MetadataFieldSuggestionCandidate> candidates,
                             std::size_t maximum_results = 12U);

} // namespace trackknife::metadata
