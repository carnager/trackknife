// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/metadata/document.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace trackknife::loudness {

enum class LoudnessGroupingMode : std::uint8_t {
    // Track gains only; no album programme.
    track,
    // The whole selection is one album programme.
    selection_album,
    // Release-aware: MUSICBRAINZ_ALBUMID identifies the programme; files
    // without one fall back deterministically to album + album artist, and
    // files with no identifying tags stay track-only rather than joining a
    // programme they do not belong to.
    release,
    // A tkfmt-1 expression evaluated per file; equal non-empty results
    // group, an empty result stays track-only.
    format_expression,
    // ADR-0146: like release, but the tag fallback key strips a trailing
    // disc designator from the album text so "Album (Disc 1)" and
    // "Album CD2" measure as one programme.
    release_merged_discs,
};

struct LoudnessGrouping {
    LoudnessGroupingMode mode{LoudnessGroupingMode::release};
    std::string expression;

    friend bool operator==(const LoudnessGrouping&, const LoudnessGrouping&) = default;
};

// ADR-0146: removes one trailing disc designator from an album text —
// "(Disc 2)", "[CD 1]", "- Disc 3", "Vol. 2", "CD2" and the like,
// case-insensitively with arabic numbering. Conservative: when removal
// would empty the text, the original is returned unchanged.
[[nodiscard]] std::string strip_disc_designator(std::string_view album);

// Assigns one optional album key per document, aligned with the input.
// Pure and deterministic: the result feeds LoudnessScanItem::album_key.
[[nodiscard]] core::Result<std::vector<std::optional<std::string>>>
assign_loudness_groups(const LoudnessGrouping& grouping,
                       std::span<const metadata::MetadataDocument* const> documents,
                       const core::CancellationToken& cancellation = {});

} // namespace trackknife::loudness
