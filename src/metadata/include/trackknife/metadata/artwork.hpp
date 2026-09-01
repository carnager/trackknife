// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/core/revision.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::metadata {

enum class ArtworkRole : std::uint8_t {
    front,
    back,
    artist,
    disc,
    icon,
    other,
};

[[nodiscard]] std::string_view artwork_role_name(ArtworkRole role);

enum class ArtworkProvenance : std::uint8_t {
    embedded,
    external,
};

[[nodiscard]] std::string_view artwork_provenance_name(ArtworkProvenance provenance);

struct ExternalArtworkPattern {
    // An exact raw sibling basename. Separators, NUL, ".", and ".." are
    // invalid; matching is byte-exact and case-sensitive.
    std::string raw_basename;
    ArtworkRole role{ArtworkRole::front};

    friend bool operator==(const ExternalArtworkPattern&, const ExternalArtworkPattern&) = default;
};

struct ArtworkInventoryPolicy {
    std::vector<ExternalArtworkPattern> external_patterns;
    std::size_t maximum_items{64U};
    std::uint64_t maximum_item_bytes{16U * 1024U * 1024U};
    std::uint64_t maximum_total_bytes{64U * 1024U * 1024U};

    friend bool operator==(const ArtworkInventoryPolicy&, const ArtworkInventoryPolicy&) = default;
};

[[nodiscard]] ArtworkInventoryPolicy default_artwork_inventory_policy();

struct ArtworkImageFile {
    std::string raw_path;
    core::LocalSourceRevision source_revision;
    std::string mime_type;
    std::optional<std::uint32_t> width;
    std::optional<std::uint32_t> height;
    std::uint64_t byte_size{0U};
    core::ContentFingerprint content_fingerprint;
    // Present when raw_path is a native FLAC donor rather than a standalone
    // PNG/JPEG file. The encoded picture bytes are reread transiently from
    // this revision-qualified ordinal.
    std::optional<std::size_t> embedded_source_ordinal;

    friend bool operator==(const ArtworkImageFile&, const ArtworkImageFile&) = default;
};

// Inspects one exact PNG or JPEG replacement input under a revision bracket.
// Encoded bytes are hashed transiently but are not returned.
[[nodiscard]] core::Result<ArtworkImageFile>
read_artwork_image_file(const std::string& raw_path,
                        std::uint64_t maximum_bytes = 16U * 1024U * 1024U,
                        const core::CancellationToken& cancellation = {});

// Rereads one already inspected standalone or embedded image under its exact
// source revision and content fingerprint. The returned bytes are transient;
// callers must not persist them as plan or journal state.
[[nodiscard]] core::Result<std::vector<unsigned char>>
read_artwork_image_bytes(const ArtworkImageFile& image,
                         std::uint64_t maximum_bytes = 16U * 1024U * 1024U,
                         const core::CancellationToken& cancellation = {});

struct ArtworkInventoryItem {
    ArtworkRole role{ArtworkRole::other};
    // Exact adapter type, such as TagLib's "Front Cover". External patterns
    // have no native type and leave this empty.
    std::string native_type;
    std::string mime_type;
    std::string description;
    std::optional<std::uint32_t> width;
    std::optional<std::uint32_t> height;
    std::uint64_t byte_size{0U};
    core::ContentFingerprint content_fingerprint;
    ArtworkProvenance provenance{ArtworkProvenance::embedded};
    std::string raw_source_path;
    core::LocalSourceRevision source_revision;
    // Embedded picture order or external-pattern order, depending on
    // provenance. It remains stable only for the captured source revision.
    std::size_t source_ordinal{0U};
    // Earlier inventory item with identical encoded bytes, if any. Both
    // provenance records remain visible.
    std::optional<std::size_t> duplicate_of;

    friend bool operator==(const ArtworkInventoryItem&, const ArtworkInventoryItem&) = default;
};

struct ArtworkInventoryIssue {
    std::string raw_source_path;
    core::Error error;

    friend bool operator==(const ArtworkInventoryIssue&, const ArtworkInventoryIssue&) = default;
};

struct ArtworkReadCapabilities {
    bool embedded_readable{false};
    bool external_readable{true};

    friend bool operator==(const ArtworkReadCapabilities&,
                           const ArtworkReadCapabilities&) = default;
};

struct LocalArtworkInventory {
    std::string raw_media_path;
    core::LocalSourceRevision media_revision;
    std::string embedded_adapter_name;
    ArtworkReadCapabilities capabilities;
    std::vector<ArtworkInventoryItem> items;
    std::vector<ArtworkInventoryIssue> issues;

    friend bool operator==(const LocalArtworkInventory&, const LocalArtworkInventory&) = default;
};

// Inventories native-FLAC picture blocks and exact configured sibling images.
// Encoded bytes are inspected and hashed transiently but are not returned.
// Run this synchronous boundary on a bounded worker, never the UI thread.
[[nodiscard]] core::Result<LocalArtworkInventory> read_local_artwork_inventory(
    const std::string& raw_media_path,
    const ArtworkInventoryPolicy& policy = default_artwork_inventory_policy(),
    const core::CancellationToken& cancellation = {});

[[nodiscard]] std::string artwork_fingerprint_hex(const core::ContentFingerprint& fingerprint);

// Hashes the ordered semantic evidence for one embedded-picture inventory.
// Raw paths, source revisions, and duplicate annotations are deliberately not
// included, so the same verified inventory has the same digest after an
// atomic publication gives the FLAC a new inode.
[[nodiscard]] core::Result<core::ContentFingerprint>
fingerprint_embedded_artwork_inventory(std::span<const ArtworkInventoryItem> items);

} // namespace trackknife::metadata
