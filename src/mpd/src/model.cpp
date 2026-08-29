// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/mpd/model.hpp"

#include "trackknife/core/unicode.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <compare>
#include <limits>
#include <tuple>

namespace trackknife::mpd {
namespace {

[[nodiscard]] char ascii_lower(char value) noexcept {
    const auto byte = static_cast<unsigned char>(value);
    if (byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z')) {
        return static_cast<char>(byte + static_cast<unsigned char>('a' - 'A'));
    }
    return value;
}

[[nodiscard]] std::vector<std::string> owned_values(const Metadata& metadata,
                                                    std::string_view name) {
    std::vector<std::string> result;
    for (const auto value : metadata.values(name)) {
        result.emplace_back(value);
    }
    return result;
}

struct TextSortKey {
    bool missing{true};
    std::string folded;
    std::string original;

    friend auto operator<=>(const TextSortKey&, const TextSortKey&) = default;
};

struct NumberSortKey {
    bool missing{true};
    std::uint64_t value{std::numeric_limits<std::uint64_t>::max()};

    friend auto operator<=>(const NumberSortKey&, const NumberSortKey&) = default;
};

struct SearchSortKey {
    TextSortKey album_artist;
    TextSortKey album;
    NumberSortKey disc;
    NumberSortKey track;
    TextSortKey title;
    std::string uri;

    friend auto operator<=>(const SearchSortKey&, const SearchSortKey&) = default;
};

struct KeyedTrack {
    SearchSortKey key;
    Track track;
};

[[nodiscard]] TextSortKey text_sort_key(const std::string_view value) {
    if (value.empty()) {
        return {};
    }
    auto folded = core::unicodeSimpleLower(value);
    return TextSortKey{
        .missing = false,
        .folded = folded ? std::move(*folded) : std::string{value},
        .original = std::string{value},
    };
}

[[nodiscard]] NumberSortKey number_sort_key(const std::optional<std::string_view> value) {
    if (!value) {
        return {};
    }
    auto text = *value;
    const auto first_digit = text.find_first_not_of(" \t");
    if (first_digit == std::string_view::npos) {
        return {};
    }
    text.remove_prefix(first_digit);
    std::uint64_t number{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
    if (parsed.ptr == text.data() || parsed.ec != std::errc{}) {
        return {};
    }
    return NumberSortKey{.missing = false, .value = number};
}

[[nodiscard]] std::string_view album_artist_sort_value(const Track& track) {
    if (!track.musicbrainz.album_artist_sort_names.empty()) {
        return track.musicbrainz.album_artist_sort_names.front();
    }
    if (!track.musicbrainz.artist_sort_names.empty()) {
        return track.musicbrainz.artist_sort_names.front();
    }
    if (const auto album_artist = track.metadata.first("AlbumArtist"); album_artist) {
        return *album_artist;
    }
    return track.metadata.first("Artist").value_or(std::string_view{});
}

[[nodiscard]] std::string_view metadata_sort_value(const Track& track,
                                                   const std::string_view sort_name,
                                                   const std::string_view display_name) {
    if (const auto sort_value = track.metadata.first(sort_name); sort_value) {
        return *sort_value;
    }
    return track.metadata.first(display_name).value_or(std::string_view{});
}

[[nodiscard]] SearchSortKey search_sort_key(const Track& track) {
    return SearchSortKey{
        .album_artist = text_sort_key(album_artist_sort_value(track)),
        .album = text_sort_key(metadata_sort_value(track, "AlbumSort", "Album")),
        .disc = number_sort_key(track.metadata.first("Disc")),
        .track = number_sort_key(track.metadata.first("Track")),
        .title = text_sort_key(metadata_sort_value(track, "TitleSort", "Title")),
        .uri = track.uri,
    };
}

} // namespace

bool ascii_case_equal(std::string_view left, std::string_view right) noexcept {
    return left.size() == right.size() &&
           std::ranges::equal(left, right, [](const char lhs, const char rhs) {
               return ascii_lower(lhs) == ascii_lower(rhs);
           });
}

Metadata::Metadata(std::vector<Pair> fields) : fields_(std::move(fields)) {}

std::vector<std::string_view> Metadata::values(std::string_view name) const {
    std::vector<std::string_view> result;
    for (const auto& field : fields_) {
        if (ascii_case_equal(field.name, name)) {
            result.emplace_back(field.value);
        }
    }
    return result;
}

std::optional<std::string_view> Metadata::first(std::string_view name) const {
    const auto found = std::ranges::find_if(
        fields_, [name](const Pair& field) { return ascii_case_equal(field.name, name); });
    if (found == fields_.end()) {
        return std::nullopt;
    }
    return found->value;
}

MusicBrainzIdentity project_musicbrainz(const Metadata& metadata) {
    return MusicBrainzIdentity{
        .artist_ids = owned_values(metadata, "MusicBrainzArtistId"),
        .album_artist_ids = owned_values(metadata, "MusicBrainzAlbumArtistId"),
        .recording_ids = owned_values(metadata, "MusicBrainzTrackId"),
        .release_track_ids = owned_values(metadata, "MusicBrainzReleaseTrackId"),
        .release_ids = owned_values(metadata, "MusicBrainzAlbumId"),
        .release_group_ids = owned_values(metadata, "MusicBrainzReleaseGroupId"),
        .work_ids = owned_values(metadata, "MusicBrainzWorkId"),
        .artist_sort_names = owned_values(metadata, "ArtistSort"),
        .album_artist_sort_names = owned_values(metadata, "AlbumArtistSort"),
    };
}

void sort_search_results(std::vector<Track>& tracks) {
    std::vector<KeyedTrack> keyed_tracks;
    keyed_tracks.reserve(tracks.size());
    for (auto& track : tracks) {
        auto key = search_sort_key(track);
        keyed_tracks.push_back(KeyedTrack{.key = std::move(key), .track = std::move(track)});
    }
    std::ranges::stable_sort(keyed_tracks, {}, &KeyedTrack::key);

    tracks.clear();
    tracks.reserve(keyed_tracks.size());
    for (auto& keyed : keyed_tracks) {
        tracks.push_back(std::move(keyed.track));
    }
}

bool Capabilities::supports_command(std::string_view command) const {
    return std::ranges::any_of(commands, [command](const std::string& candidate) {
        return ascii_case_equal(candidate, command);
    });
}

bool Capabilities::exposes_tag(std::string_view tag) const {
    return std::ranges::any_of(tag_types, [tag](const std::string& candidate) {
        return ascii_case_equal(candidate, tag);
    });
}

} // namespace trackknife::mpd
