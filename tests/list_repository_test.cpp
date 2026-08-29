// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/persistence/list_repository.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

void list_documents_round_trip_transactionally() {
    namespace persistence = trackknife::persistence;
    const auto database_path = std::filesystem::temp_directory_path() /
                               ("trackknife-list-repository-" +
                                trackknife::core::StableId::random().to_string() + ".sqlite3");
    const auto cleanup = [&database_path] {
        std::error_code ignored;
        std::filesystem::remove(database_path, ignored);
        std::filesystem::remove(database_path.string() + "-wal", ignored);
        std::filesystem::remove(database_path.string() + "-shm", ignored);
    };
    cleanup();

    const auto profile_id = trackknife::core::StableId::random();
    const auto scratch_id = trackknife::core::StableId::random();
    const auto saved_id = trackknife::core::StableId::random();
    const std::string raw_local_path{"music/invalid-\xff.flac", 20U};
    const std::vector<persistence::ListDocument> expected{
        persistence::ListDocument{
            .id = scratch_id,
            .kind = persistence::ListKind::scratch,
            .name = "Scratch 1",
            .pinned = false,
            .dirty = true,
            .items =
                {
                    persistence::ListItem{
                        .source = persistence::ListSource::mpd,
                        .profile_id = profile_id,
                        .source_reference = "Artist/Album/01.flac",
                        .duration_ms = 180'125,
                        .fields = {{"Artist", "First"}, {"Artist", "Second"}, {"Title", "Song"}},
                    },
                    persistence::ListItem{
                        .source = persistence::ListSource::mpd,
                        .profile_id = profile_id,
                        .source_reference = "Artist/Album/01.flac",
                        .duration_ms = 180'125,
                        .fields = {{"Artist", "First"}, {"Title", "Song"}},
                    },
                    persistence::ListItem{
                        .source = persistence::ListSource::local,
                        .profile_id = std::nullopt,
                        .source_reference = raw_local_path,
                        .duration_ms = std::nullopt,
                        .fields = {{"Title", "Raw path"}},
                    },
                },
        },
        persistence::ListDocument{
            .id = saved_id,
            .kind = persistence::ListKind::saved,
            .name = "Tag work",
            .pinned = true,
            .dirty = false,
            .items = {},
        },
    };

    {
        auto opened = persistence::ListRepository::open(database_path);
        require(opened.has_value(), "list repository must create and migrate a new database");
        auto repository = std::move(*opened);
        require(repository.schema_version() == 3U, "state repository schema must be explicit");
        require(repository.replace_all(expected).has_value(),
                "valid list documents must commit in one transaction");
        require(repository.load_all() == expected,
                "list order, duplicates, raw paths, and repeated snapshot fields must round trip");

        auto invalid = expected;
        invalid.back().id = scratch_id;
        const auto rejected = repository.replace_all(invalid);
        require(!rejected && rejected.error().code == trackknife::core::ErrorCode::invalid_argument,
                "duplicate document IDs must be rejected before starting a write");
        require(repository.load_all() == expected,
                "a rejected replacement must leave the previous transaction intact");

        const std::vector profiles{
            persistence::ConnectionProfile{
                .id = profile_id,
                .name = "Melody",
                .host = "caprica",
                .port = 6600U,
                .local_music_root = std::string{"/srv/music"},
                .auto_connect = true,
            },
            persistence::ConnectionProfile{
                .id = trackknife::core::StableId::random(),
                .name = "Stock MPD",
                .host = "127.0.0.1",
                .port = 6601U,
                .local_music_root = std::nullopt,
                .auto_connect = false,
            },
        };
        require(repository.replace_profiles(profiles).has_value() &&
                    repository.load_profiles() == profiles,
                "connection profiles must round trip in stable display order");
        auto conflicting_profiles = profiles;
        conflicting_profiles.back().auto_connect = true;
        require(!repository.replace_profiles(conflicting_profiles) &&
                    repository.load_profiles() == profiles,
                "profile validation must preserve the preceding transaction");
        const std::vector presets{
            persistence::TrackViewPreset{.binding = "live-queue", .header_state = "qt-state"},
            persistence::TrackViewPreset{.binding = "local:" + scratch_id.to_string(),
                                         .header_state = std::string{"\0\xff", 2U}},
        };
        require(repository.replace_view_presets(presets).has_value() &&
                    repository.load_view_presets() == presets,
                "binary Qt track-view state must round trip transactionally");
        auto invalid_presets = presets;
        invalid_presets.back().binding = presets.front().binding;
        require(!repository.replace_view_presets(invalid_presets) &&
                    repository.load_view_presets() == presets,
                "invalid view presets must preserve the preceding transaction");
    }

    {
        auto reopened = persistence::ListRepository::open(database_path);
        require(reopened.has_value() && reopened->load_all() == expected,
                "persisted list documents must survive closing and reopening SQLite");
    }
    cleanup();
}

} // namespace

int main() {
    list_documents_round_trip_transactionally();
    return EXIT_SUCCESS;
}
