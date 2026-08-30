// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/metadata/staged_patch.hpp"

#include <span>
#include <string>
#include <string_view>

namespace trackknife::metadata {

struct FlacTextFieldMapping {
    std::string property_name;

    friend bool operator==(const FlacTextFieldMapping&, const FlacTextFieldMapping&) = default;
};

// Resolves one Trackbench field to the exact Xiph-comment key used by the
// native FLAC adapter. Existing native spelling wins; known newly added fields
// use the repository-owned conventional mapping; arbitrary additions retain
// their display spelling with ASCII letters uppercased. Exact empty values and
// artwork conventions are rejected instead of being silently reinterpreted.
[[nodiscard]] core::Result<FlacTextFieldMapping>
map_flac_text_field(std::string_view canonical_name, std::string_view display_name,
                    std::string_view existing_native_name, StagedMetadataPatchKind kind,
                    std::span<const std::string> values);

} // namespace trackknife::metadata
