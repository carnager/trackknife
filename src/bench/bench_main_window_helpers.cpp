// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window_helpers.hpp"

#include <algorithm>
#include <ranges>
#include <unordered_set>

namespace trackknife::bench {

QString trackColumnId(const int logical) {
    const auto found = std::ranges::find(track_column_specs, logical, &TrackColumnSpec::logical);
    return found == track_column_specs.end() ? QString{} : QString::fromLatin1(found->id);
}

int trackColumnLogical(const QString& id) {
    const auto found = std::ranges::find_if(
        track_column_specs, [&id](const auto& spec) { return id == QString::fromLatin1(spec.id); });
    return found == track_column_specs.end() ? -1 : found->logical;
}

QStringList trackColumnIds() {
    QStringList ids;
    ids.reserve(static_cast<qsizetype>(track_column_specs.size()));
    for (const auto& spec : track_column_specs) {
        ids.push_back(QString::fromLatin1(spec.id));
    }
    return ids;
}

QString displayText(const std::string& utf8) {
    return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
}

std::string utf8Bytes(const QString& text) {
    const auto encoded = text.toUtf8();
    return {encoded.constData(), static_cast<std::size_t>(encoded.size())};
}

QString formatTime(const qint64 milliseconds) {
    const auto total_seconds = std::max<qint64>(0, milliseconds) / 1'000;
    const auto minutes = total_seconds / 60;
    const auto seconds = total_seconds % 60;
    return QStringLiteral("%1:%2").arg(minutes).arg(seconds, 2, 10, QChar{'0'});
}

std::string lowercased_ascii(std::string name) {
    for (auto& character : name) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return name;
}

// FFmpeg exposes a few format-native fields through generic AVDictionary
// spellings. Those spellings are presentation projections, not additional
// native tag identities. Keep this adapter mapping explicit so ADR-0066's
// freeform-field identity never mistakes one for an independently writable
// property.
std::optional<std::string_view> probed_semantic_alias(const std::string_view native_name) {
    const auto name = lowercased_ascii(std::string{native_name});
    if (name == "track") {
        return "tracknumber";
    }
    if (name == "disc") {
        return "discnumber";
    }
    if (name == "album_artist") {
        return "albumartist";
    }
    return std::nullopt;
}

void remove_shadowed_probed_metadata(metadata::MetadataDocument& document) {
    std::unordered_set<std::string> embedded_semantic_names;
    embedded_semantic_names.reserve(document.fields.size());
    for (const auto& field : document.fields) {
        if (field.provenance == metadata::FieldProvenance::embedded) {
            embedded_semantic_names.insert(field.canonical_name);
        }
    }
    std::erase_if(document.fields, [&embedded_semantic_names](const auto& field) {
        if (field.provenance != metadata::FieldProvenance::stream) {
            return false;
        }
        const auto semantic = probed_semantic_alias(field.native_name);
        return semantic && embedded_semantic_names.contains(std::string{*semantic});
    });
}

std::string metadata_value(const metadata::MetadataDocument& document,
                           const std::initializer_list<std::string_view> candidate_names) {
    for (const auto name : candidate_names) {
        if (auto value = document.first_effective_value(name)) {
            return std::move(*value);
        }
    }
    return {};
}

void project_display_metadata(LocalTrackRow& row) {
    row.title = metadata_value(row.metadata, {"title"});
    row.artist = metadata_value(row.metadata, {"artist"});
    row.album = metadata_value(row.metadata, {"album"});
    row.album_artist = metadata_value(row.metadata, {"albumartist"});
    row.date = metadata_value(row.metadata, {"date", "year"});
    row.track_number = metadata_value(row.metadata, {"tracknumber", "track"});
}

} // namespace trackknife::bench
