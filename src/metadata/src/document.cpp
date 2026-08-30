// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/document.hpp"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <utility>

namespace trackknife::metadata {
namespace {

[[nodiscard]] unsigned precedence(const FieldProvenance provenance) {
    switch (provenance) {
    case FieldProvenance::cached_snapshot:
        return 0U;
    case FieldProvenance::annotation:
        return 1U;
    case FieldProvenance::embedded:
    case FieldProvenance::stream:
        return 2U;
    case FieldProvenance::segment:
        return 3U;
    case FieldProvenance::sidecar:
        return 4U;
    }
    return 0U;
}

} // namespace

std::string_view field_provenance_name(const FieldProvenance provenance) {
    switch (provenance) {
    case FieldProvenance::cached_snapshot:
        return "cached snapshot";
    case FieldProvenance::annotation:
        return "annotation";
    case FieldProvenance::embedded:
        return "embedded";
    case FieldProvenance::stream:
        return "stream";
    case FieldProvenance::segment:
        return "segment";
    case FieldProvenance::sidecar:
        return "sidecar";
    }
    return "embedded";
}

std::string canonicalize_field_name(const std::string_view name) {
    std::string canonical;
    canonical.reserve(name.size());
    for (const auto character : name) {
        if (character == ' ' || character == '_' || character == '-') {
            continue;
        }
        canonical.push_back(character >= 'A' && character <= 'Z'
                                ? static_cast<char>(character - 'A' + 'a')
                                : character);
    }
    return canonical;
}

std::vector<std::string> MetadataDocument::effective_values(const std::string_view name) const {
    const auto canonical = canonicalize_field_name(name);
    auto selected_precedence = std::numeric_limits<unsigned>::max();
    for (const auto& field : fields) {
        if (field.canonical_name == canonical) {
            selected_precedence = selected_precedence == std::numeric_limits<unsigned>::max()
                                      ? precedence(field.provenance)
                                      : std::max(selected_precedence, precedence(field.provenance));
        }
    }
    if (selected_precedence == std::numeric_limits<unsigned>::max()) {
        return {};
    }

    std::vector<std::string> values;
    for (const auto& field : fields) {
        if (field.canonical_name == canonical &&
            precedence(field.provenance) == selected_precedence) {
            values.insert(values.end(), field.values.begin(), field.values.end());
        }
    }
    return values;
}

std::optional<std::string>
MetadataDocument::first_effective_value(const std::string_view name) const {
    auto values = effective_values(name);
    if (values.empty()) {
        return std::nullopt;
    }
    return std::move(values.front());
}

std::vector<EffectiveMetadataField> MetadataDocument::effective_fields() const {
    std::vector<EffectiveMetadataField> effective;
    effective.reserve(fields.size());
    std::unordered_map<std::string, std::size_t> positions;
    positions.reserve(fields.size());
    for (const auto& field : fields) {
        if (field.canonical_name.empty()) {
            continue;
        }
        const auto [position, inserted] = positions.emplace(field.canonical_name, effective.size());
        if (inserted) {
            effective.push_back(EffectiveMetadataField{
                .canonical_name = field.canonical_name,
                .native_name = field.native_name,
                .values = field.values,
                .provenance = field.provenance,
            });
            continue;
        }
        auto& selected = effective[position->second];
        if (precedence(field.provenance) > precedence(selected.provenance)) {
            selected.native_name = field.native_name;
            selected.values = field.values;
            selected.provenance = field.provenance;
        } else if (precedence(field.provenance) == precedence(selected.provenance)) {
            selected.values.insert(selected.values.end(), field.values.begin(), field.values.end());
        }
    }
    return effective;
}

MusicBrainzIdentity project_musicbrainz(const MetadataDocument& document) {
    return MusicBrainzIdentity{
        .artist_ids = document.effective_values("musicbrainz_artistid"),
        .album_artist_ids = document.effective_values("musicbrainz_albumartistid"),
        .recording_ids = document.effective_values("musicbrainz_trackid"),
        .release_track_ids = document.effective_values("musicbrainz_releasetrackid"),
        .release_ids = document.effective_values("musicbrainz_albumid"),
        .release_group_ids = document.effective_values("musicbrainz_releasegroupid"),
        .work_ids = document.effective_values("musicbrainz_workid"),
        .disc_ids = document.effective_values("musicbrainz_discid"),
        .artist_sort_names = document.effective_values("artistsort"),
        .album_artist_sort_names = document.effective_values("albumartistsort"),
        .artist_credits = document.effective_values("artists"),
        .album_artist_credits = document.effective_values("albumartists"),
    };
}

} // namespace trackknife::metadata
