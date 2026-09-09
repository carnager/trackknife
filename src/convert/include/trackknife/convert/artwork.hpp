// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/convert/convert.hpp"
#include "trackknife/core/cancellation.hpp"

#include <optional>
#include <string>

namespace trackknife::convert {

// Picks one cover image for a conversion source, best-effort and read-only
// (ADR-0131): native-FLAC embedded pictures through the qualified inventory
// (front cover first, preserving role-level type and description), then any
// other container's first embedded picture through the FFmpeg reader, then
// exact-basename sibling images from the default inventory policy. Only
// PNG/JPEG up to the inventory byte bound qualifies; a source without usable
// artwork resolves to nullopt instead of failing. Synchronous I/O — run on a
// bounded worker.
[[nodiscard]] std::optional<ConversionArtwork>
resolve_conversion_artwork(const std::string& source_raw_path,
                           const core::CancellationToken& cancellation = {});

} // namespace trackknife::convert
