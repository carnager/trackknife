// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/core/stable_id.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace trackknife::persistence {

enum class ListKind : std::uint8_t { scratch, saved };
enum class ListSource : std::uint8_t { mpd, local };

struct SnapshotField {
    std::string name;
    std::string value;

    friend bool operator==(const SnapshotField&, const SnapshotField&) = default;
};

struct ListItem {
    ListSource source{ListSource::mpd};
    std::optional<core::StableId> profile_id;
    // MPD URIs and local raw OS paths are stored as SQLite BLOBs. No UTF-8 or
    // URL interpretation occurs at this boundary.
    std::string source_reference;
    std::optional<std::int64_t> duration_ms;
    std::vector<SnapshotField> fields;

    friend bool operator==(const ListItem&, const ListItem&) = default;
};

struct ListDocument {
    core::StableId id;
    ListKind kind{ListKind::scratch};
    std::string name;
    bool pinned{false};
    bool dirty{false};
    std::vector<ListItem> items;

    friend bool operator==(const ListDocument&, const ListDocument&) = default;
};

struct ConnectionProfile {
    core::StableId id;
    std::string name;
    std::string host;
    unsigned port{6600U};
    std::optional<std::string> local_music_root;
    bool auto_connect{false};

    friend bool operator==(const ConnectionProfile&, const ConnectionProfile&) = default;
};

struct TrackViewPreset {
    std::string binding;
    std::string header_state;

    friend bool operator==(const TrackViewPreset&, const TrackViewPreset&) = default;
};

class ListRepository final {
  public:
    ListRepository(ListRepository&&) noexcept;
    ListRepository& operator=(ListRepository&&) noexcept;
    ListRepository(const ListRepository&) = delete;
    ListRepository& operator=(const ListRepository&) = delete;
    ~ListRepository();

    [[nodiscard]] static core::Result<ListRepository> open(const std::filesystem::path& path);

    [[nodiscard]] core::Result<unsigned> schema_version() const;
    [[nodiscard]] core::Result<std::vector<ListDocument>> load_all() const;
    [[nodiscard]] core::Result<void> replace_all(std::span<const ListDocument> documents);
    [[nodiscard]] core::Result<std::vector<ConnectionProfile>> load_profiles() const;
    [[nodiscard]] core::Result<void> replace_profiles(std::span<const ConnectionProfile> profiles);
    [[nodiscard]] core::Result<std::vector<TrackViewPreset>> load_view_presets() const;
    [[nodiscard]] core::Result<void> replace_view_presets(std::span<const TrackViewPreset> presets);

  private:
    struct Impl;
    explicit ListRepository(std::unique_ptr<Impl> implementation);

    std::unique_ptr<Impl> implementation_;
};

} // namespace trackknife::persistence
