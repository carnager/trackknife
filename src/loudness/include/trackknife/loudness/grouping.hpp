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
};

struct LoudnessGrouping {
    LoudnessGroupingMode mode{LoudnessGroupingMode::release};
    std::string expression;

    friend bool operator==(const LoudnessGrouping&, const LoudnessGrouping&) = default;
};

// Assigns one optional album key per document, aligned with the input.
// Pure and deterministic: the result feeds LoudnessScanItem::album_key.
[[nodiscard]] core::Result<std::vector<std::optional<std::string>>>
assign_loudness_groups(const LoudnessGrouping& grouping,
                       std::span<const metadata::MetadataDocument* const> documents,
                       const core::CancellationToken& cancellation = {});

} // namespace trackknife::loudness
