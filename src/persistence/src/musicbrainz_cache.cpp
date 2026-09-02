// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/persistence/musicbrainz_cache.hpp"

#include "trackknife/persistence/list_repository.hpp"

#include <sqlite3.h>

#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace trackknife::persistence {
namespace {

constexpr std::size_t maximum_body_bytes = 8U * 1024U * 1024U;

[[nodiscard]] core::Error database_error(sqlite3* database, std::string message) {
    core::Error error{
        .code = core::ErrorCode::database,
        .message = std::move(message),
        .context = {},
    };
    if (database != nullptr) {
        error.context.push_back({.key = "sqlite", .value = sqlite3_errmsg(database)});
    }
    return error;
}

struct StatementDeleter {
    void operator()(sqlite3_stmt* statement) const noexcept {
        static_cast<void>(sqlite3_finalize(statement));
    }
};
using Statement = std::unique_ptr<sqlite3_stmt, StatementDeleter>;

[[nodiscard]] core::Result<Statement> prepare(sqlite3* database, const char* sql) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return std::unexpected(
            database_error(database, "Could not prepare MusicBrainz cache statement"));
    }
    return Statement{statement};
}

} // namespace

struct SqliteMusicBrainzResponseCache::Impl {
    sqlite3* database{nullptr};

    ~Impl() {
        if (database != nullptr) {
            static_cast<void>(sqlite3_close_v2(database));
        }
    }
};

core::Result<SqliteMusicBrainzResponseCache>
SqliteMusicBrainzResponseCache::open(const std::filesystem::path& path) {
    {
        auto migrated = ListRepository::open(path);
        if (!migrated) {
            return std::unexpected(std::move(migrated.error()));
        }
    }
    auto implementation = std::make_unique<Impl>();
    const auto& encoded = path.native();
    if (sqlite3_open_v2(encoded.c_str(), &implementation->database,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        return std::unexpected(
            database_error(implementation->database, "Could not open MusicBrainz cache"));
    }
    sqlite3_busy_timeout(implementation->database, 2'000);
    return SqliteMusicBrainzResponseCache{std::move(implementation)};
}

SqliteMusicBrainzResponseCache::SqliteMusicBrainzResponseCache(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}
SqliteMusicBrainzResponseCache::SqliteMusicBrainzResponseCache(
    SqliteMusicBrainzResponseCache&&) noexcept = default;
SqliteMusicBrainzResponseCache&
SqliteMusicBrainzResponseCache::operator=(SqliteMusicBrainzResponseCache&&) noexcept = default;
SqliteMusicBrainzResponseCache::~SqliteMusicBrainzResponseCache() = default;

core::Result<std::optional<std::vector<std::byte>>>
SqliteMusicBrainzResponseCache::load(const std::string_view url,
                                     const std::int64_t now_unix_seconds,
                                     const std::int64_t ttl_seconds) const {
    auto statement =
        prepare(implementation_->database, "SELECT body FROM musicbrainz_response_cache "
                                           "WHERE url = ?1 AND fetched_at_unix_seconds >= ?2");
    if (!statement) {
        return std::unexpected(std::move(statement.error()));
    }
    sqlite3_bind_blob(statement->get(), 1, url.data(), static_cast<int>(url.size()),
                      SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement->get(), 2, now_unix_seconds - ttl_seconds);
    const auto stepped = sqlite3_step(statement->get());
    if (stepped == SQLITE_DONE) {
        return std::optional<std::vector<std::byte>>{};
    }
    if (stepped != SQLITE_ROW) {
        return std::unexpected(
            database_error(implementation_->database, "Could not read the MusicBrainz cache"));
    }
    const auto* blob = sqlite3_column_blob(statement->get(), 0);
    const auto size = sqlite3_column_bytes(statement->get(), 0);
    std::vector<std::byte> body(static_cast<std::size_t>(size));
    if (size > 0 && blob != nullptr) {
        std::memcpy(body.data(), blob, static_cast<std::size_t>(size));
    }
    return std::optional{std::move(body)};
}

core::Result<void> SqliteMusicBrainzResponseCache::store(const std::string_view url,
                                                         const std::string_view body,
                                                         const std::int64_t now_unix_seconds,
                                                         const std::int64_t ttl_seconds,
                                                         const std::size_t maximum_entries) {
    if (url.empty() || body.size() > maximum_body_bytes || maximum_entries == 0U ||
        now_unix_seconds < 0 || ttl_seconds < 0) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "the MusicBrainz cache store request is out of bounds",
            .context = {},
        });
    }
    {
        auto statement = prepare(implementation_->database,
                                 "INSERT INTO musicbrainz_response_cache"
                                 "(url, body, fetched_at_unix_seconds) VALUES(?1, ?2, ?3) "
                                 "ON CONFLICT(url) DO UPDATE SET body = excluded.body, "
                                 "fetched_at_unix_seconds = excluded.fetched_at_unix_seconds");
        if (!statement) {
            return std::unexpected(std::move(statement.error()));
        }
        sqlite3_bind_blob(statement->get(), 1, url.data(), static_cast<int>(url.size()),
                          SQLITE_TRANSIENT);
        sqlite3_bind_blob(statement->get(), 2, body.data(), static_cast<int>(body.size()),
                          SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement->get(), 3, now_unix_seconds);
        if (sqlite3_step(statement->get()) != SQLITE_DONE) {
            return std::unexpected(database_error(implementation_->database,
                                                  "Could not store the MusicBrainz response"));
        }
    }
    {
        auto statement =
            prepare(implementation_->database, "DELETE FROM musicbrainz_response_cache "
                                               "WHERE fetched_at_unix_seconds < ?1");
        if (!statement) {
            return std::unexpected(std::move(statement.error()));
        }
        sqlite3_bind_int64(statement->get(), 1, now_unix_seconds - ttl_seconds);
        if (sqlite3_step(statement->get()) != SQLITE_DONE) {
            return std::unexpected(database_error(implementation_->database,
                                                  "Could not expire the MusicBrainz cache"));
        }
    }
    {
        auto statement = prepare(implementation_->database,
                                 "DELETE FROM musicbrainz_response_cache WHERE url NOT IN ("
                                 "SELECT url FROM musicbrainz_response_cache "
                                 "ORDER BY fetched_at_unix_seconds DESC, url ASC LIMIT ?1)");
        if (!statement) {
            return std::unexpected(std::move(statement.error()));
        }
        sqlite3_bind_int64(statement->get(), 1,
                           static_cast<sqlite3_int64>(std::min(
                               maximum_entries, static_cast<std::size_t>(
                                                    std::numeric_limits<sqlite3_int64>::max()))));
        if (sqlite3_step(statement->get()) != SQLITE_DONE) {
            return std::unexpected(
                database_error(implementation_->database, "Could not bound the MusicBrainz cache"));
        }
    }
    return {};
}

} // namespace trackknife::persistence
