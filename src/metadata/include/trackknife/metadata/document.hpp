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
    // Semantic identity is assigned only by an explicit adapter mapping;
    // otherwise this retains the adapter's case-folded native identity.
    // native_name always retains the exact exposed key for inspection and
    // format-specific mutation.
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
    // Returns one entry per adapter-exposed field spelling, folding ASCII case
    // only. Separator differences remain distinct so cleanup operations can
    // address a legacy native key without touching a conventional neighbor.
    [[nodiscard]] std::vector<EffectiveMetadataField> effective_native_fields() const;
    [[nodiscard]] std::optional<EffectiveMetadataField>
    effective_native_field(std::string_view native_name) const;

    friend bool operator==(const MetadataDocument&, const MetadataDocument&) = default;
};

// Semantic-name and UI-query normalization only: ASCII letters become
// lowercase and space, underscore, and hyphen separators are ignored. This
// function never decides whether two native properties are aliases.
[[nodiscard]] std::string canonicalize_field_name(std::string_view name);

// Exact adapter-exposed identity folds ASCII case but preserves every
// separator and punctuation byte.
[[nodiscard]] std::string canonicalize_native_field_name(std::string_view name);

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
