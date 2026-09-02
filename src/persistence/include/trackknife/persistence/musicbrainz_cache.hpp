// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::persistence {

// Bounded MusicBrainz response cache (ADR-0088). Rows are exact response
// bodies keyed by request URL; loads honour a caller-supplied TTL and stores
// prune expired rows and enforce the entry bound oldest-first.
class SqliteMusicBrainzResponseCache final {
  public:
    [[nodiscard]] static core::Result<SqliteMusicBrainzResponseCache>
    open(const std::filesystem::path& path);

    SqliteMusicBrainzResponseCache(SqliteMusicBrainzResponseCache&&) noexcept;
    SqliteMusicBrainzResponseCache& operator=(SqliteMusicBrainzResponseCache&&) noexcept;
    ~SqliteMusicBrainzResponseCache();

    [[nodiscard]] core::Result<std::optional<std::vector<std::byte>>>
    load(std::string_view url, std::int64_t now_unix_seconds, std::int64_t ttl_seconds) const;

    [[nodiscard]] core::Result<void> store(std::string_view url, std::string_view body,
                                           std::int64_t now_unix_seconds, std::int64_t ttl_seconds,
                                           std::size_t maximum_entries);

  private:
    struct Impl;
    explicit SqliteMusicBrainzResponseCache(std::unique_ptr<Impl> implementation);
    std::unique_ptr<Impl> implementation_;
};

} // namespace trackknife::persistence
