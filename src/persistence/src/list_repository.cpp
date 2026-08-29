// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/persistence/list_repository.hpp"

#include <sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace trackknife::persistence {
namespace {

constexpr unsigned current_schema_version = 3U;
constexpr std::size_t maximum_documents = 1'024U;
constexpr std::size_t maximum_items_per_document = 1'000'000U;
constexpr std::size_t maximum_fields_per_item = 4'096U;

struct StatementDeleter {
    void operator()(sqlite3_stmt* statement) const noexcept {
        if (statement != nullptr) {
            sqlite3_finalize(statement);
        }
    }
};

using Statement = std::unique_ptr<sqlite3_stmt, StatementDeleter>;

[[nodiscard]] core::Error database_error(sqlite3* database, std::string message) {
    if (database != nullptr) {
        message += ": ";
        message += sqlite3_errmsg(database);
    }
    return core::Error{
        .code = core::ErrorCode::database, .message = std::move(message), .context = {}};
}

[[nodiscard]] core::Result<void> execute(sqlite3* database, const char* sql) {
    char* message = nullptr;
    const auto result = sqlite3_exec(database, sql, nullptr, nullptr, &message);
    if (result == SQLITE_OK) {
        return {};
    }
    auto error = database_error(database, message == nullptr ? "SQLite statement failed"
                                                             : std::string{message});
    sqlite3_free(message);
    return std::unexpected(std::move(error));
}

[[nodiscard]] core::Result<Statement> prepare(sqlite3* database, const char* sql) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK) {
        return std::unexpected(database_error(database, "Could not prepare SQLite statement"));
    }
    return Statement{raw};
}

[[nodiscard]] bool bind_blob(sqlite3_stmt* statement, const int index,
                             const std::string_view value) {
    return sqlite3_bind_blob64(statement, index, value.data(), value.size(), SQLITE_TRANSIENT) ==
           SQLITE_OK;
}

[[nodiscard]] bool bind_text(sqlite3_stmt* statement, const int index,
                             const std::string_view value) {
    return sqlite3_bind_text64(statement, index, value.data(), value.size(), SQLITE_TRANSIENT,
                               SQLITE_UTF8) == SQLITE_OK;
}

[[nodiscard]] std::string column_blob(sqlite3_stmt* statement, const int column) {
    const auto size = sqlite3_column_bytes(statement, column);
    const auto* data = static_cast<const char*>(sqlite3_column_blob(statement, column));
    return data == nullptr || size <= 0 ? std::string{} : std::string{data, data + size};
}

[[nodiscard]] std::string column_text(sqlite3_stmt* statement, const int column) {
    const auto size = sqlite3_column_bytes(statement, column);
    const auto* data = reinterpret_cast<const char*>(sqlite3_column_text(statement, column));
    return data == nullptr || size <= 0 ? std::string{} : std::string{data, data + size};
}

[[nodiscard]] core::Result<void> step_done(sqlite3* database, sqlite3_stmt* statement,
                                           std::string_view operation) {
    if (sqlite3_step(statement) != SQLITE_DONE) {
        return std::unexpected(database_error(database, std::string{operation}));
    }
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    return {};
}

[[nodiscard]] core::Result<void> migrate(sqlite3* database) {
    if (auto result = execute(database, "BEGIN IMMEDIATE"); !result) {
        return result;
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    if (auto result = execute(database, "CREATE TABLE IF NOT EXISTS schema_version ("
                                        "version INTEGER NOT NULL CHECK(version >= 0))");
        !result) {
        rollback();
        return result;
    }
    if (auto result = execute(database, "INSERT INTO schema_version(version) "
                                        "SELECT 0 WHERE NOT EXISTS (SELECT 1 FROM schema_version)");
        !result) {
        rollback();
        return result;
    }
    auto query = prepare(database, "SELECT version FROM schema_version LIMIT 1");
    if (!query) {
        rollback();
        return std::unexpected(std::move(query.error()));
    }
    if (sqlite3_step(query->get()) != SQLITE_ROW) {
        auto error = database_error(database, "Could not read SQLite schema version");
        rollback();
        return std::unexpected(std::move(error));
    }
    const auto version = sqlite3_column_int64(query->get(), 0);
    if (version > current_schema_version) {
        rollback();
        return std::unexpected(core::Error{
            .code = core::ErrorCode::unsupported,
            .message = "Trackknife state database was created by a newer application version",
            .context = {{"schema_version", std::to_string(version)}},
        });
    }
    if (version == 0) {
        constexpr auto migration =
            "CREATE TABLE list_documents ("
            "id TEXT PRIMARY KEY NOT NULL, kind INTEGER NOT NULL, name BLOB NOT NULL, "
            "pinned INTEGER NOT NULL CHECK(pinned IN (0,1)), "
            "dirty INTEGER NOT NULL CHECK(dirty IN (0,1)), position INTEGER NOT NULL UNIQUE);"
            "CREATE TABLE list_items ("
            "document_id TEXT NOT NULL REFERENCES list_documents(id) ON DELETE CASCADE, "
            "position INTEGER NOT NULL, source INTEGER NOT NULL, profile_id TEXT, "
            "source_reference BLOB NOT NULL, duration_ms INTEGER, "
            "PRIMARY KEY(document_id, position));"
            "CREATE TABLE list_item_fields ("
            "document_id TEXT NOT NULL, item_position INTEGER NOT NULL, position INTEGER NOT NULL, "
            "name BLOB NOT NULL, value BLOB NOT NULL, "
            "PRIMARY KEY(document_id, item_position, position), "
            "FOREIGN KEY(document_id, item_position) "
            "REFERENCES list_items(document_id, position) ON DELETE CASCADE);"
            "CREATE INDEX list_item_fields_item "
            "ON list_item_fields(document_id, item_position, position);"
            "UPDATE schema_version SET version = 1;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 1) {
        constexpr auto migration =
            "CREATE TABLE connection_profiles ("
            "id TEXT PRIMARY KEY NOT NULL, name BLOB NOT NULL, host BLOB NOT NULL, "
            "port INTEGER NOT NULL CHECK(port BETWEEN 1 AND 65535), local_music_root BLOB, "
            "auto_connect INTEGER NOT NULL CHECK(auto_connect IN (0,1)), "
            "position INTEGER NOT NULL UNIQUE);"
            "UPDATE schema_version SET version = 2;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 2) {
        constexpr auto migration = "CREATE TABLE track_view_presets ("
                                   "binding BLOB PRIMARY KEY NOT NULL, header_state BLOB NOT NULL, "
                                   "position INTEGER NOT NULL UNIQUE);"
                                   "UPDATE schema_version SET version = 3;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (auto result = execute(database, "COMMIT"); !result) {
        rollback();
        return result;
    }
    return {};
}

[[nodiscard]] core::Result<void> validate(std::span<const ListDocument> documents) {
    if (documents.size() > maximum_documents) {
        return std::unexpected(core::Error{.code = core::ErrorCode::limit_exceeded,
                                           .message = "At most 1024 list documents can be stored",
                                           .context = {}});
    }
    std::unordered_set<std::string> ids;
    ids.reserve(documents.size());
    for (const auto& document : documents) {
        const auto id = document.id.to_string();
        if (document.id.is_nil() || !ids.insert(id).second) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::invalid_argument,
                .message = "List document IDs must be non-nil and unique",
                .context = {},
            });
        }
        if (document.name.empty()) {
            return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                               .message = "List document names cannot be empty",
                                               .context = {}});
        }
        if (document.items.size() > maximum_items_per_document) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::limit_exceeded,
                .message = "A list document cannot contain more than 1000000 items",
                .context = {},
            });
        }
        for (const auto& item : document.items) {
            if (item.source_reference.empty()) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::invalid_argument,
                    .message = "List item source references cannot be empty",
                    .context = {},
                });
            }
            if (item.source == ListSource::mpd && !item.profile_id) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::invalid_argument,
                    .message = "Remote list items require an MPD profile identity",
                    .context = {},
                });
            }
            if (item.fields.size() > maximum_fields_per_item) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::limit_exceeded,
                    .message = "A list item cannot contain more than 4096 snapshot fields",
                    .context = {},
                });
            }
        }
    }
    return {};
}

} // namespace

struct ListRepository::Impl {
    sqlite3* database{nullptr};

    ~Impl() {
        if (database != nullptr) {
            sqlite3_close(database);
        }
    }
};

ListRepository::ListRepository(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}

ListRepository::ListRepository(ListRepository&&) noexcept = default;
ListRepository& ListRepository::operator=(ListRepository&&) noexcept = default;
ListRepository::~ListRepository() = default;

core::Result<ListRepository> ListRepository::open(const std::filesystem::path& path) {
    auto implementation = std::make_unique<Impl>();
    const auto& encoded = path.native();
    if (sqlite3_open_v2(encoded.c_str(), &implementation->database,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        return std::unexpected(
            database_error(implementation->database, "Could not open Trackknife state database"));
    }
    sqlite3_busy_timeout(implementation->database, 2'000);
    if (auto result = execute(implementation->database, "PRAGMA foreign_keys = ON"); !result) {
        return std::unexpected(std::move(result.error()));
    }
    if (auto result = execute(implementation->database, "PRAGMA journal_mode = WAL"); !result) {
        return std::unexpected(std::move(result.error()));
    }
    if (auto result = migrate(implementation->database); !result) {
        return std::unexpected(std::move(result.error()));
    }
    return ListRepository{std::move(implementation)};
}

core::Result<unsigned> ListRepository::schema_version() const {
    auto statement = prepare(implementation_->database, "SELECT version FROM schema_version");
    if (!statement) {
        return std::unexpected(std::move(statement.error()));
    }
    if (sqlite3_step(statement->get()) != SQLITE_ROW) {
        return std::unexpected(
            database_error(implementation_->database, "Could not read schema version"));
    }
    const auto version = sqlite3_column_int64(statement->get(), 0);
    if (version < 0 || version > std::numeric_limits<unsigned>::max()) {
        return std::unexpected(core::Error{.code = core::ErrorCode::database,
                                           .message = "Invalid SQLite schema version",
                                           .context = {}});
    }
    return static_cast<unsigned>(version);
}

core::Result<std::vector<ListDocument>> ListRepository::load_all() const {
    auto documents_query =
        prepare(implementation_->database,
                "SELECT id, kind, name, pinned, dirty FROM list_documents ORDER BY position");
    if (!documents_query) {
        return std::unexpected(std::move(documents_query.error()));
    }
    std::vector<ListDocument> documents;
    std::unordered_map<std::string, std::size_t> document_indices;
    while (sqlite3_step(documents_query->get()) == SQLITE_ROW) {
        const auto id_text = column_text(documents_query->get(), 0);
        auto id = core::StableId::parse(id_text);
        const auto kind = sqlite3_column_int(documents_query->get(), 1);
        if (!id || (kind != static_cast<int>(ListKind::scratch) &&
                    kind != static_cast<int>(ListKind::saved))) {
            return std::unexpected(core::Error{.code = core::ErrorCode::database,
                                               .message = "Invalid persisted list document",
                                               .context = {{"document_id", id_text}}});
        }
        document_indices.emplace(id_text, documents.size());
        documents.push_back(ListDocument{
            .id = *id,
            .kind = static_cast<ListKind>(kind),
            .name = column_blob(documents_query->get(), 2),
            .pinned = sqlite3_column_int(documents_query->get(), 3) != 0,
            .dirty = sqlite3_column_int(documents_query->get(), 4) != 0,
            .items = {},
        });
    }
    if (sqlite3_errcode(implementation_->database) != SQLITE_OK &&
        sqlite3_errcode(implementation_->database) != SQLITE_DONE) {
        return std::unexpected(
            database_error(implementation_->database, "Could not load list documents"));
    }

    auto items_query =
        prepare(implementation_->database,
                "SELECT document_id, position, source, profile_id, source_reference, duration_ms "
                "FROM list_items ORDER BY document_id, position");
    if (!items_query) {
        return std::unexpected(std::move(items_query.error()));
    }
    while (sqlite3_step(items_query->get()) == SQLITE_ROW) {
        const auto document_id = column_text(items_query->get(), 0);
        const auto found = document_indices.find(document_id);
        const auto position = sqlite3_column_int64(items_query->get(), 1);
        const auto source = sqlite3_column_int(items_query->get(), 2);
        if (found == document_indices.end() || position < 0 ||
            static_cast<std::size_t>(position) != documents[found->second].items.size() ||
            (source != static_cast<int>(ListSource::mpd) &&
             source != static_cast<int>(ListSource::local))) {
            return std::unexpected(core::Error{.code = core::ErrorCode::database,
                                               .message = "Invalid persisted list item order",
                                               .context = {{"document_id", document_id}}});
        }
        std::optional<core::StableId> profile_id;
        if (sqlite3_column_type(items_query->get(), 3) != SQLITE_NULL) {
            auto parsed = core::StableId::parse(column_text(items_query->get(), 3));
            if (!parsed) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Invalid persisted list item profile identity",
                    .context = {{"document_id", document_id}},
                });
            }
            profile_id = *parsed;
        }
        std::optional<std::int64_t> duration;
        if (sqlite3_column_type(items_query->get(), 5) != SQLITE_NULL) {
            duration = sqlite3_column_int64(items_query->get(), 5);
        }
        documents[found->second].items.push_back(ListItem{
            .source = static_cast<ListSource>(source),
            .profile_id = profile_id,
            .source_reference = column_blob(items_query->get(), 4),
            .duration_ms = duration,
            .fields = {},
        });
    }

    auto fields_query =
        prepare(implementation_->database,
                "SELECT document_id, item_position, position, name, value FROM list_item_fields "
                "ORDER BY document_id, item_position, position");
    if (!fields_query) {
        return std::unexpected(std::move(fields_query.error()));
    }
    while (sqlite3_step(fields_query->get()) == SQLITE_ROW) {
        const auto document_id = column_text(fields_query->get(), 0);
        const auto found = document_indices.find(document_id);
        const auto item_position = sqlite3_column_int64(fields_query->get(), 1);
        const auto field_position = sqlite3_column_int64(fields_query->get(), 2);
        if (found == document_indices.end() || item_position < 0 ||
            static_cast<std::size_t>(item_position) >= documents[found->second].items.size()) {
            return std::unexpected(core::Error{.code = core::ErrorCode::database,
                                               .message = "Invalid persisted snapshot field item",
                                               .context = {{"document_id", document_id}}});
        }
        auto& fields =
            documents[found->second].items[static_cast<std::size_t>(item_position)].fields;
        if (field_position < 0 || static_cast<std::size_t>(field_position) != fields.size()) {
            return std::unexpected(core::Error{.code = core::ErrorCode::database,
                                               .message = "Invalid persisted snapshot field order",
                                               .context = {{"document_id", document_id}}});
        }
        fields.push_back(SnapshotField{.name = column_blob(fields_query->get(), 3),
                                       .value = column_blob(fields_query->get(), 4)});
    }
    return documents;
}

core::Result<void> ListRepository::replace_all(const std::span<const ListDocument> documents) {
    if (auto result = validate(documents); !result) {
        return result;
    }
    auto* database = implementation_->database;
    if (auto result = execute(database, "BEGIN IMMEDIATE"); !result) {
        return result;
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    if (auto result = execute(database, "DELETE FROM list_documents"); !result) {
        rollback();
        return result;
    }
    auto insert_document = prepare(
        database,
        "INSERT INTO list_documents(id, kind, name, pinned, dirty, position) VALUES(?,?,?,?,?,?)");
    auto insert_item = prepare(
        database,
        "INSERT INTO list_items(document_id, position, source, profile_id, source_reference, "
        "duration_ms) VALUES(?,?,?,?,?,?)");
    auto insert_field = prepare(
        database, "INSERT INTO list_item_fields(document_id, item_position, position, name, value) "
                  "VALUES(?,?,?,?,?)");
    if (!insert_document || !insert_item || !insert_field) {
        auto error = !insert_document ? std::move(insert_document.error())
                     : !insert_item   ? std::move(insert_item.error())
                                      : std::move(insert_field.error());
        rollback();
        return std::unexpected(std::move(error));
    }

    for (std::size_t document_position = 0U; document_position < documents.size();
         ++document_position) {
        const auto& document = documents[document_position];
        const auto id = document.id.to_string();
        auto* document_statement = insert_document->get();
        if (!bind_text(document_statement, 1, id) ||
            sqlite3_bind_int(document_statement, 2, static_cast<int>(document.kind)) != SQLITE_OK ||
            !bind_blob(document_statement, 3, document.name) ||
            sqlite3_bind_int(document_statement, 4, document.pinned ? 1 : 0) != SQLITE_OK ||
            sqlite3_bind_int(document_statement, 5, document.dirty ? 1 : 0) != SQLITE_OK ||
            sqlite3_bind_int64(document_statement, 6,
                               static_cast<sqlite3_int64>(document_position)) != SQLITE_OK) {
            auto error = database_error(database, "Could not bind list document");
            rollback();
            return std::unexpected(std::move(error));
        }
        if (auto result = step_done(database, document_statement, "Could not store list document");
            !result) {
            rollback();
            return result;
        }

        for (std::size_t item_position = 0U; item_position < document.items.size();
             ++item_position) {
            const auto& item = document.items[item_position];
            auto* item_statement = insert_item->get();
            const auto profile_id = item.profile_id ? item.profile_id->to_string() : std::string{};
            const auto profile_bound = item.profile_id
                                           ? bind_text(item_statement, 4, profile_id)
                                           : sqlite3_bind_null(item_statement, 4) == SQLITE_OK;
            const auto duration_bound =
                item.duration_ms
                    ? sqlite3_bind_int64(item_statement, 6, *item.duration_ms) == SQLITE_OK
                    : sqlite3_bind_null(item_statement, 6) == SQLITE_OK;
            if (!bind_text(item_statement, 1, id) ||
                sqlite3_bind_int64(item_statement, 2, static_cast<sqlite3_int64>(item_position)) !=
                    SQLITE_OK ||
                sqlite3_bind_int(item_statement, 3, static_cast<int>(item.source)) != SQLITE_OK ||
                !profile_bound || !bind_blob(item_statement, 5, item.source_reference) ||
                !duration_bound) {
                auto error = database_error(database, "Could not bind list item");
                rollback();
                return std::unexpected(std::move(error));
            }
            if (auto result = step_done(database, item_statement, "Could not store list item");
                !result) {
                rollback();
                return result;
            }

            for (std::size_t field_position = 0U; field_position < item.fields.size();
                 ++field_position) {
                const auto& field = item.fields[field_position];
                auto* field_statement = insert_field->get();
                if (!bind_text(field_statement, 1, id) ||
                    sqlite3_bind_int64(field_statement, 2,
                                       static_cast<sqlite3_int64>(item_position)) != SQLITE_OK ||
                    sqlite3_bind_int64(field_statement, 3,
                                       static_cast<sqlite3_int64>(field_position)) != SQLITE_OK ||
                    !bind_blob(field_statement, 4, field.name) ||
                    !bind_blob(field_statement, 5, field.value)) {
                    auto error = database_error(database, "Could not bind snapshot field");
                    rollback();
                    return std::unexpected(std::move(error));
                }
                if (auto result =
                        step_done(database, field_statement, "Could not store snapshot field");
                    !result) {
                    rollback();
                    return result;
                }
            }
        }
    }
    if (auto result = execute(database, "COMMIT"); !result) {
        rollback();
        return result;
    }
    return {};
}

core::Result<std::vector<ConnectionProfile>> ListRepository::load_profiles() const {
    auto statement = prepare(implementation_->database,
                             "SELECT id, name, host, port, local_music_root, auto_connect "
                             "FROM connection_profiles ORDER BY position");
    if (!statement) {
        return std::unexpected(std::move(statement.error()));
    }
    std::vector<ConnectionProfile> profiles;
    int result = SQLITE_ROW;
    while ((result = sqlite3_step(statement->get())) == SQLITE_ROW) {
        const auto id_text = column_text(statement->get(), 0);
        auto id = core::StableId::parse(id_text);
        const auto port = sqlite3_column_int64(statement->get(), 3);
        if (!id || port < 1 || port > 65'535) {
            return std::unexpected(core::Error{.code = core::ErrorCode::database,
                                               .message = "Invalid persisted connection profile",
                                               .context = {{"profile_id", id_text}}});
        }
        std::optional<std::string> root;
        if (sqlite3_column_type(statement->get(), 4) != SQLITE_NULL) {
            root = column_blob(statement->get(), 4);
        }
        profiles.push_back(ConnectionProfile{
            .id = *id,
            .name = column_blob(statement->get(), 1),
            .host = column_blob(statement->get(), 2),
            .port = static_cast<unsigned>(port),
            .local_music_root = std::move(root),
            .auto_connect = sqlite3_column_int(statement->get(), 5) != 0,
        });
    }
    if (result != SQLITE_DONE) {
        return std::unexpected(
            database_error(implementation_->database, "Could not load connection profiles"));
    }
    return profiles;
}

core::Result<void>
ListRepository::replace_profiles(const std::span<const ConnectionProfile> profiles) {
    std::unordered_set<std::string> ids;
    ids.reserve(profiles.size());
    std::size_t auto_connect_count = 0U;
    for (const auto& profile : profiles) {
        if (profile.id.is_nil() || profile.name.empty() || profile.host.empty() ||
            profile.port < 1U || profile.port > 65'535U ||
            !ids.insert(profile.id.to_string()).second) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::invalid_argument,
                .message = "Connection profiles require unique IDs, names, hosts, and valid ports",
                .context = {},
            });
        }
        auto_connect_count += profile.auto_connect ? 1U : 0U;
    }
    if (auto_connect_count > 1U) {
        return std::unexpected(core::Error{.code = core::ErrorCode::conflict,
                                           .message = "Only one profile may auto-connect",
                                           .context = {}});
    }

    auto* database = implementation_->database;
    if (auto result = execute(database, "BEGIN IMMEDIATE"); !result) {
        return result;
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    if (auto result = execute(database, "DELETE FROM connection_profiles"); !result) {
        rollback();
        return result;
    }
    auto statement = prepare(
        database,
        "INSERT INTO connection_profiles(id, name, host, port, local_music_root, auto_connect, "
        "position) VALUES(?,?,?,?,?,?,?)");
    if (!statement) {
        rollback();
        return std::unexpected(std::move(statement.error()));
    }
    for (std::size_t position = 0U; position < profiles.size(); ++position) {
        const auto& profile = profiles[position];
        const auto id = profile.id.to_string();
        auto* raw = statement->get();
        const auto root_bound = profile.local_music_root
                                    ? bind_blob(raw, 5, *profile.local_music_root)
                                    : sqlite3_bind_null(raw, 5) == SQLITE_OK;
        if (!bind_text(raw, 1, id) || !bind_blob(raw, 2, profile.name) ||
            !bind_blob(raw, 3, profile.host) ||
            sqlite3_bind_int(raw, 4, static_cast<int>(profile.port)) != SQLITE_OK || !root_bound ||
            sqlite3_bind_int(raw, 6, profile.auto_connect ? 1 : 0) != SQLITE_OK ||
            sqlite3_bind_int64(raw, 7, static_cast<sqlite3_int64>(position)) != SQLITE_OK) {
            auto error = database_error(database, "Could not bind connection profile");
            rollback();
            return std::unexpected(std::move(error));
        }
        if (auto result = step_done(database, raw, "Could not store connection profile"); !result) {
            rollback();
            return result;
        }
    }
    if (auto result = execute(database, "COMMIT"); !result) {
        rollback();
        return result;
    }
    return {};
}

core::Result<std::vector<TrackViewPreset>> ListRepository::load_view_presets() const {
    auto statement =
        prepare(implementation_->database, "SELECT binding, header_state FROM track_view_presets "
                                           "ORDER BY position");
    if (!statement) {
        return std::unexpected(std::move(statement.error()));
    }
    std::vector<TrackViewPreset> presets;
    int result = SQLITE_ROW;
    while ((result = sqlite3_step(statement->get())) == SQLITE_ROW) {
        presets.push_back(TrackViewPreset{.binding = column_blob(statement->get(), 0),
                                          .header_state = column_blob(statement->get(), 1)});
        if (presets.back().binding.empty()) {
            return std::unexpected(core::Error{.code = core::ErrorCode::database,
                                               .message = "Invalid persisted track view preset",
                                               .context = {}});
        }
    }
    if (result != SQLITE_DONE) {
        return std::unexpected(
            database_error(implementation_->database, "Could not load track view presets"));
    }
    return presets;
}

core::Result<void>
ListRepository::replace_view_presets(const std::span<const TrackViewPreset> presets) {
    constexpr std::size_t maximum_presets = 4'096U;
    constexpr std::size_t maximum_state_size = 1024U * 1024U;
    if (presets.size() > maximum_presets) {
        return std::unexpected(core::Error{.code = core::ErrorCode::limit_exceeded,
                                           .message = "At most 4096 view presets can be stored",
                                           .context = {}});
    }
    std::unordered_set<std::string> bindings;
    for (const auto& preset : presets) {
        if (preset.binding.empty() || preset.header_state.size() > maximum_state_size ||
            !bindings.insert(preset.binding).second) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::invalid_argument,
                .message = "View preset bindings must be non-empty, unique, and bounded",
                .context = {},
            });
        }
    }
    auto* database = implementation_->database;
    if (auto result = execute(database, "BEGIN IMMEDIATE"); !result) {
        return result;
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    if (auto result = execute(database, "DELETE FROM track_view_presets"); !result) {
        rollback();
        return result;
    }
    auto statement = prepare(
        database, "INSERT INTO track_view_presets(binding, header_state, position) VALUES(?,?,?)");
    if (!statement) {
        rollback();
        return std::unexpected(std::move(statement.error()));
    }
    for (std::size_t position = 0U; position < presets.size(); ++position) {
        const auto& preset = presets[position];
        auto* raw = statement->get();
        if (!bind_blob(raw, 1, preset.binding) || !bind_blob(raw, 2, preset.header_state) ||
            sqlite3_bind_int64(raw, 3, static_cast<sqlite3_int64>(position)) != SQLITE_OK) {
            auto error = database_error(database, "Could not bind track view preset");
            rollback();
            return std::unexpected(std::move(error));
        }
        if (auto result = step_done(database, raw, "Could not store track view preset"); !result) {
            rollback();
            return result;
        }
    }
    if (auto result = execute(database, "COMMIT"); !result) {
        rollback();
        return result;
    }
    return {};
}

} // namespace trackknife::persistence
