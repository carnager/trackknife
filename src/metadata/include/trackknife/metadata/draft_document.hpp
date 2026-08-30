// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/metadata/document.hpp"
#include "trackknife/metadata/staged_patch.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace trackknife::metadata {

// Materializes the effective metadata context seen by pure downstream
// planning. The returned documents preserve the immutable baseline's
// unsupported-object inventory while applying every sparse draft patch for
// the requested items. This is a naming/preview projection, not a writable
// native container representation.
[[nodiscard]] core::Result<std::vector<MetadataDocument>> materialize_metadata_draft(
    const StagedMetadataSelection& selection, const StagedMetadataPatchSet& patches,
    std::span<const std::size_t> item_indexes, const core::CancellationToken& cancellation = {});

} // namespace trackknife::metadata
