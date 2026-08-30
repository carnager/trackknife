// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::metadata {

enum class FieldProvenance : std::uint8_t {
    cached_snapshot,
    annotation,
    embedded,
    stream,
    segment,
    sidecar,
};

[[nodiscard]] std::string_view field_provenance_name(FieldProvenance provenance);

struct FieldQualifier {
    std::optional<std::string> language;
    std::optional<std::string> description;

    friend bool operator==(const FieldQualifier&, const FieldQualifier&) = default;
};

struct MetadataField {
    // Lookup identity is Trackbench-owned and stable across backend spelling.
    // native_name retains the adapter's exact exposed key for inspection and
    // later format-specific mapping.
    std::string canonical_name;
    std::string native_name;
    std::vector<std::string> values;
    FieldQualifier qualifier;
    FieldProvenance provenance{FieldProvenance::embedded};

    friend bool operator==(const MetadataField&, const MetadataField&) = default;
};

struct NativeObjectIdentity {
    std::string identity;

    friend bool operator==(const NativeObjectIdentity&, const NativeObjectIdentity&) = default;
};

struct EffectiveMetadataField {
    std::string canonical_name;
    std::string native_name;
    std::vector<std::string> values;
    FieldProvenance provenance{FieldProvenance::embedded};

    friend bool operator==(const EffectiveMetadataField&, const EffectiveMetadataField&) = default;
};

struct MetadataDocument {
    std::vector<MetadataField> fields;
    // Backend identities for objects that cannot be flattened into text
    // properties. Read support inventories them; no write capability is
    // implied.
    std::vector<NativeObjectIdentity> unsupported_native_objects;

    [[nodiscard]] std::vector<std::string> effective_values(std::string_view name) const;
    [[nodiscard]] std::optional<std::string> first_effective_value(std::string_view name) const;
    // Returns one entry per canonical name in first-seen document order. The
    // winning provenance and its ordered values follow the same precedence as
    // effective_values().
    [[nodiscard]] std::vector<EffectiveMetadataField> effective_fields() const;

    friend bool operator==(const MetadataDocument&, const MetadataDocument&) = default;
};

// ASCII lookup folding only: letters become lowercase and space, underscore,
// and hyphen separators are ignored. UTF-8 bytes and punctuation otherwise
// remain exact; native_name always retains the adapter spelling.
[[nodiscard]] std::string canonicalize_field_name(std::string_view name);

struct MusicBrainzIdentity {
    std::vector<std::string> artist_ids;
    std::vector<std::string> album_artist_ids;
    std::vector<std::string> recording_ids;
    std::vector<std::string> release_track_ids;
    std::vector<std::string> release_ids;
    std::vector<std::string> release_group_ids;
    std::vector<std::string> work_ids;
    std::vector<std::string> disc_ids;
    std::vector<std::string> artist_sort_names;
    std::vector<std::string> album_artist_sort_names;
    std::vector<std::string> artist_credits;
    std::vector<std::string> album_artist_credits;

    friend bool operator==(const MusicBrainzIdentity&, const MusicBrainzIdentity&) = default;
};

[[nodiscard]] MusicBrainzIdentity project_musicbrainz(const MetadataDocument& document);

} // namespace trackknife::metadata
