// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/persistence/local_library.hpp"

#include "trackknife/core/stable_id.hpp"
#include "trackknife/core/unicode.hpp"
#include "trackknife/formats/probe.hpp"
#include "trackknife/metadata/flac_mapping.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/persistence/list_repository.hpp"
#include "trackknife/titleformat/evaluator.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace trackknife::persistence {
namespace {

[[noreturn]] void fail(const std::string& message,
                       const core::ErrorCode code = core::ErrorCode::database) {
    throw core::Error{.code = code, .message = message, .context = {}};
}

template <typename F> auto checked(F&& operation) -> core::Result<std::invoke_result_t<F>> {
    try {
        return operation();
    } catch (const core::Error& error) {
        return std::unexpected(error);
    } catch (const std::exception& error) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::io, .message = error.what(), .context = {}});
    }
}

void execute(sqlite3* db, const char* sql) {
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        fail(sqlite3_errmsg(db));
    }
}

class Statement {
  public:
    Statement(sqlite3* db, const std::string& sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &value_, nullptr) != SQLITE_OK) {
            fail(sqlite3_errmsg(db));
        }
    }
    ~Statement() { sqlite3_finalize(value_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    void text(int index, const std::string& value) {
        check(sqlite3_bind_text64(value_, index, value.data(), value.size(), SQLITE_TRANSIENT,
                                  SQLITE_UTF8));
    }
    void blob(int index, const std::string& value) {
        check(sqlite3_bind_blob64(value_, index, value.data(), value.size(), SQLITE_TRANSIENT));
    }
    void number(int index, sqlite3_int64 value) { check(sqlite3_bind_int64(value_, index, value)); }
    bool next() {
        const auto result = sqlite3_step(value_);
        if (result == SQLITE_ROW) {
            return true;
        }
        if (result != SQLITE_DONE) {
            fail(sqlite3_errmsg(db_));
        }
        return false;
    }
    std::string bytes(int column) const {
        const auto* data = static_cast<const char*>(sqlite3_column_blob(value_, column));
        return data == nullptr
                   ? std::string{}
                   : std::string{data,
                                 static_cast<std::size_t>(sqlite3_column_bytes(value_, column))};
    }
    sqlite3_int64 number(int column) const { return sqlite3_column_int64(value_, column); }

  private:
    void check(int result) {
        if (result != SQLITE_OK) {
            fail(sqlite3_errmsg(db_));
        }
    }
    sqlite3* db_;
    sqlite3_stmt* value_{nullptr};
};

class Transaction {
  public:
    explicit Transaction(sqlite3* db) : db_(db) { execute(db_, "BEGIN IMMEDIATE"); }
    ~Transaction() {
        if (!done_) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    void commit() {
        execute(db_, "COMMIT");
        done_ = true;
    }

  private:
    sqlite3* db_;
    bool done_{false};
};

class QueryCancellation {
  public:
    QueryCancellation(sqlite3* db, const core::CancellationToken& token) : db_(db) {
        if (token.is_cancellation_requested()) {
            fail("Library query cancelled", core::ErrorCode::cancelled);
        }
        sqlite3_progress_handler(
            db_, 1000,
            [](void* pointer) {
                return static_cast<const core::CancellationToken*>(pointer)
                               ->is_cancellation_requested()
                           ? 1
                           : 0;
            },
            const_cast<core::CancellationToken*>(&token));
    }
    ~QueryCancellation() { sqlite3_progress_handler(db_, 0, nullptr, nullptr); }
    QueryCancellation(const QueryCancellation&) = delete;
    QueryCancellation& operator=(const QueryCancellation&) = delete;

  private:
    sqlite3* db_;
};

std::string lower(const std::string& value) {
    const auto result = core::unicodeSimpleLower(value);
    return result ? *result : core::escape_raw_path(value);
}

class LibraryLabelContext final : public titleformat::EvaluationContext {
  public:
    explicit LibraryLabelContext(const LibraryEntry& entry) : entry_(entry) {}
    titleformat::FormatContextKind kind() const noexcept override {
        return titleformat::FormatContextKind::tree_level;
    }
    std::optional<std::string> resolveField(std::string_view name) const override {
        if (name == "artist" || name == "albumartist") {
            return entry_.artist;
        }
        if (name == "album") {
            return entry_.album;
        }
        if (name == "title") {
            return entry_.label;
        }
        return std::nullopt;
    }

  private:
    const LibraryEntry& entry_;
};

std::string format_label(const LibraryEntry& entry, bool search) {
    // The shipped tree uses the same versioned language as working-list views.
    const titleformat::CompileOptions options{
        .context = titleformat::FormatContextKind::tree_level, .dialect = {}, .parse_options = {}};
    static const auto artist = titleformat::compile("%albumartist%", options);
    static const auto album = titleformat::compile("%album%", options);
    static const auto track = titleformat::compile("%title%", options);
    static const auto found_track = titleformat::compile("%artist% — %title%", options);
    const auto& compiled = entry.kind == LibraryEntryKind::artist  ? artist
                           : entry.kind == LibraryEntryKind::album ? album
                           : search                                ? found_track
                                                                   : track;
    if (!compiled.program) {
        fail("Invalid default library format", core::ErrorCode::invariant);
    }
    const LibraryLabelContext context{entry};
    auto rendered = titleformat::evaluate(*compiled.program, context);
    if (!rendered) {
        throw rendered.error();
    }
    return rendered->text;
}

bool contained(const std::string& path, const std::string& root) {
    return path == root || path.starts_with(root == "/" ? root : root + '/');
}

std::string revision_key(const core::LocalSourceRevision& revision) {
    return std::to_string(revision.device) + ':' + std::to_string(revision.inode) + ':' +
           std::to_string(revision.size) + ':' +
           std::to_string(revision.modification_time_seconds) + ':' +
           std::to_string(revision.modification_time_nanoseconds);
}

struct Tags {
    std::string title;
    std::string artist;
    std::string album;
    std::string release;
    std::string date;
    std::string search_track;
    int disc{0};
    int track{0};
};

Tags tags_from(const metadata::MetadataDocument& document, const std::string& path) {
    const auto value = [&](std::string_view name) {
        auto text = document.first_effective_value(name).value_or("");
        if (text.size() > 16'384U) {
            text.resize(16'384U);
        }
        return text;
    };
    const auto number = [&](std::string_view name) {
        const auto text = value(name);
        int result = 0;
        std::from_chars(text.data(), text.data() + text.size(), result);
        return result;
    };
    Tags tags;
    tags.title = value("title");
    if (tags.title.empty()) {
        tags.title = core::escape_raw_path(std::filesystem::path{path}.stem().native());
    }
    tags.artist = value("albumartist");
    if (tags.artist.empty()) {
        tags.artist = value("artist");
    }
    if (tags.artist.empty()) {
        tags.artist = "Unknown artist";
    }
    tags.album = value("album");
    if (tags.album.empty()) {
        tags.album = "Unknown album";
    }
    tags.release = value("musicbrainzalbumid");
    tags.date = value("date");
    tags.disc = number("discnumber");
    tags.track = number("tracknumber");
    tags.search_track = lower(tags.title + ' ' + tags.artist + ' ' + tags.album);
    for (const auto& artist : document.effective_values("artist")) {
        tags.search_track += ' ' + lower(artist);
    }
    return tags;
}

std::string album_key(const Tags& tags, const std::string& path) {
    if (!tags.release.empty()) {
        return "mbid:" + tags.release;
    }
    return std::to_string(tags.artist.size()) + ':' + tags.artist +
           std::to_string(tags.album.size()) + ':' + tags.album + ':' +
           core::escape_raw_path(std::filesystem::path{path}.parent_path().native());
}

void bind_tags(Statement& statement, int start, const Tags& tags, const std::string& path) {
    statement.text(start++, tags.title);
    statement.text(start++, tags.artist);
    statement.text(start++, tags.album);
    statement.text(start++, album_key(tags, path));
    statement.text(start++, tags.release);
    statement.text(start++, tags.date);
    statement.number(start++, tags.disc);
    statement.number(start++, tags.track);
    statement.text(start++, tags.search_track);
    statement.text(start, lower(tags.artist + ' ' + tags.album));
}

std::vector<LibraryRoot> read_roots(sqlite3* db) {
    Statement query{db,
                    "SELECT raw_path,available,error FROM local_library_roots ORDER BY raw_path"};
    std::vector<LibraryRoot> result;
    while (query.next()) {
        result.push_back({query.bytes(0), query.number(1) != 0, query.bytes(2)});
    }
    return result;
}

struct Filter {
    std::string sql{" WHERE 1=1"};
    std::vector<std::pair<std::string, bool>> values;
    explicit Filter(const LibraryQuery& query) {
        if (query.text.size() > 4096U) {
            fail("Search is too long", core::ErrorCode::limit_exceeded);
        }
        if (query.artist) {
            sql += " AND artist=?";
            values.emplace_back(*query.artist, false);
        }
        if (query.album_key) {
            sql += " AND album_key=?";
            values.emplace_back(*query.album_key, false);
        }
        if (query.raw_path) {
            sql += " AND raw_path=?";
            values.emplace_back(*query.raw_path, true);
        }
        std::istringstream words{lower(query.text)};
        std::string word;
        std::size_t count = 0;
        while (words >> word) {
            if (++count > 16U) {
                fail("Search supports up to 16 words", core::ErrorCode::limit_exceeded);
            }
            sql += query.kind == LibraryEntryKind::track ? " AND instr(search_track,?)>0"
                                                         : " AND instr(search_album,?)>0";
            values.emplace_back(word, false);
        }
    }
    void bind(Statement& statement) const {
        int index = 1;
        for (const auto& [value, blob] : values) {
            if (blob) {
                statement.blob(index++, value);
            } else {
                statement.text(index++, value);
            }
        }
    }
};

bool audio_path(const std::filesystem::path& path) {
    static constexpr std::array extensions{
        ".flac", ".mp3",  ".ogg", ".oga",  ".opus", ".m4a",  ".mp4", ".aac", ".wv",
        ".wav",  ".rf64", ".w64", ".aiff", ".aif",  ".aifc", ".ape", ".mpc", ".tta",
        ".spx",  ".mka",  ".wma", ".dsf",  ".dff",  ".mod",  ".xm",  ".s3m", ".it"};
    const auto extension = lower(path.extension().native());
    return std::ranges::find(extensions, extension) != extensions.end();
}

} // namespace

struct LocalLibrary::Impl {
    sqlite3* db{nullptr};
    ~Impl() {
        if (db != nullptr) {
            sqlite3_close(db);
        }
    }
};

LocalLibrary::LocalLibrary(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}
LocalLibrary::LocalLibrary(LocalLibrary&&) noexcept = default;
LocalLibrary& LocalLibrary::operator=(LocalLibrary&&) noexcept = default;
LocalLibrary::~LocalLibrary() = default;

core::Result<LocalLibrary> LocalLibrary::open(const std::filesystem::path& path) {
    return checked([&] {
        auto migrated = ListRepository::open(path);
        if (!migrated) {
            throw migrated.error();
        }
        auto impl = std::make_unique<Impl>();
        if (sqlite3_open_v2(path.c_str(), &impl->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX,
                            nullptr) != SQLITE_OK) {
            fail("Could not open local library");
        }
        sqlite3_busy_timeout(impl->db, 5000);
        execute(impl->db, "PRAGMA foreign_keys=ON");
        return LocalLibrary{std::move(impl)};
    });
}

core::Result<std::vector<LibraryRoot>> LocalLibrary::roots() const {
    return checked([&] { return read_roots(implementation_->db); });
}

core::Result<void> LocalLibrary::add_root(const std::string& raw_path) {
    const auto result = checked([&] {
        if (raw_path.empty() || raw_path.find('\0') != std::string::npos ||
            !std::filesystem::path{raw_path}.is_absolute()) {
            fail("Choose an absolute folder path", core::ErrorCode::invalid_argument);
        }
        std::error_code error;
        const auto canonical = std::filesystem::canonical(std::filesystem::path{raw_path}, error);
        if (error || !std::filesystem::is_directory(canonical, error) || error) {
            fail("The library folder is unavailable", core::ErrorCode::io);
        }
        const auto& path = canonical.native();
        auto* db = implementation_->db;
        Transaction transaction{db};
        const auto existing = read_roots(db);
        if (existing.size() >= 64U) {
            fail("At most 64 library folders can be configured", core::ErrorCode::limit_exceeded);
        }
        for (const auto& root : existing) {
            if (contained(path, root.raw_path) || contained(root.raw_path, path)) {
                fail("This folder overlaps an existing library folder", core::ErrorCode::conflict);
            }
        }
        Statement insert{db, "INSERT INTO local_library_roots(raw_path) VALUES(?)"};
        insert.blob(1, path);
        insert.next();
        transaction.commit();
        return true;
    });
    if (!result) {
        return std::unexpected(result.error());
    }
    return {};
}

core::Result<void> LocalLibrary::remove_root(const std::string& path) {
    const auto result = checked([&] {
        Statement remove{implementation_->db, "DELETE FROM local_library_roots WHERE raw_path=?"};
        remove.blob(1, path);
        remove.next();
        return true;
    });
    if (!result) {
        return std::unexpected(result.error());
    }
    return {};
}

core::Result<LibraryPage> LocalLibrary::query(const LibraryQuery& query,
                                              const core::CancellationToken& cancellation) const {
    return checked([&] {
        auto* db = implementation_->db;
        QueryCancellation guard{db, cancellation};
        Filter filter{query};
        std::string columns;
        std::string order;
        switch (query.kind) {
        case LibraryEntryKind::artist:
            columns = "artist,artist,artist,'',count(*),sum(available)";
            order = " GROUP BY artist ORDER BY artist COLLATE NOCASE";
            break;
        case LibraryEntryKind::album:
            columns = "album_key,min(album),min(artist),min(album),count(*),sum(available)";
            order = " GROUP BY album_key ORDER BY min(artist) COLLATE NOCASE,min(date),min(album) "
                    "COLLATE NOCASE,album_key";
            break;
        case LibraryEntryKind::track:
            columns = "raw_path,title,artist,album,1,available";
            order = " ORDER BY artist COLLATE NOCASE,album_key,disc,track,title COLLATE "
                    "NOCASE,raw_path";
            break;
        }
        const auto limit = std::clamp<std::size_t>(query.limit, 1U, 200U);
        Statement statement{db,
                            "SELECT " + columns + " FROM local_library_tracks" + filter.sql +
                                order + " LIMIT " + std::to_string(limit + 1U) + " OFFSET " +
                                std::to_string(std::min<std::size_t>(query.offset, 1'000'000U))};
        filter.bind(statement);
        LibraryPage page;
        while (statement.next()) {
            if (page.entries.size() == limit) {
                page.more = true;
                break;
            }
            page.entries.push_back({query.kind, statement.bytes(0), statement.bytes(1),
                                    statement.bytes(2), statement.bytes(3),
                                    static_cast<std::size_t>(statement.number(4)),
                                    static_cast<std::size_t>(statement.number(5))});
            page.entries.back().label = format_label(page.entries.back(), !query.text.empty());
        }
        return page;
    });
}

core::Result<std::vector<std::string>>
LocalLibrary::paths(const LibraryQuery& query, const core::CancellationToken& cancellation) const {
    return checked([&] {
        auto* db = implementation_->db;
        QueryCancellation guard{db, cancellation};
        const Filter filter{query};
        Statement statement{db, "SELECT raw_path FROM local_library_tracks" + filter.sql +
                                    " AND available=1 ORDER BY artist COLLATE "
                                    "NOCASE,album_key,disc,track,raw_path LIMIT 100001"};
        filter.bind(statement);
        std::vector<std::string> result;
        while (statement.next()) {
            if (result.size() == 100'000U) {
                fail("Selection exceeds 100000 files; select an artist or album",
                     core::ErrorCode::limit_exceeded);
            }
            result.push_back(statement.bytes(0));
        }
        return result;
    });
}

core::Result<LibraryScanResult> LocalLibrary::scan(const core::CancellationToken& cancellation,
                                                   LibraryScanProgress& progress) {
    return checked([&] {
        auto* db = implementation_->db;
        const auto generation = core::StableId::random().to_string();
        LibraryScanResult result;
        for (const auto& root : read_roots(db)) {
            if (cancellation.is_cancellation_requested()) {
                result.cancelled = true;
                break;
            }
            std::error_code error;
            std::filesystem::recursive_directory_iterator iterator{
                std::filesystem::path{root.raw_path}, error};
            if (error) {
                Transaction transaction{db};
                Statement offline{db, "UPDATE local_library_roots SET "
                                      "available=0,error=?,scan_token=? WHERE raw_path=?"};
                offline.text(1, error.message());
                offline.text(2, generation);
                offline.blob(3, root.raw_path);
                offline.next();
                Statement missing{db, "UPDATE local_library_tracks SET available=0 WHERE root=?"};
                missing.blob(1, root.raw_path);
                missing.next();
                transaction.commit();
                continue;
            }
            {
                Statement begin{db, "UPDATE local_library_roots SET scan_token=? WHERE raw_path=?"};
                begin.text(1, generation);
                begin.blob(2, root.raw_path);
                begin.next();
            }
            bool complete = true;
            for (; iterator != std::filesystem::recursive_directory_iterator{};
                 iterator.increment(error)) {
                if (error) {
                    complete = false;
                    break;
                }
                if (cancellation.is_cancellation_requested()) {
                    result.cancelled = true;
                    complete = false;
                    break;
                }
                if (++progress.visited > 1'000'000U) {
                    complete = false;
                    break;
                }
                const auto path = iterator->path();
                const auto status = iterator->symlink_status(error);
                if (error) {
                    complete = false;
                    break;
                }
                if (!std::filesystem::is_regular_file(status) || !audio_path(path)) {
                    continue;
                }
                const auto& raw = path.native();
                auto before = core::observe_local_source_revision(raw);
                if (!before) {
                    ++progress.failed;
                    complete = false;
                    continue;
                }
                const auto revision = revision_key(*before);
                bool unchanged = false;
                {
                    Statement previous{
                        db, "SELECT revision FROM local_library_tracks WHERE raw_path=?"};
                    previous.blob(1, raw);
                    unchanged = previous.next() && previous.bytes(0) == revision;
                }
                std::optional<Tags> tags;
                if (!unchanged) {
                    auto probe = formats::probe_local_media(raw, cancellation);
                    if (!probe || !probe->best_audio_stream) {
                        ++progress.failed;
                        complete = false;
                        continue;
                    }
                    metadata::MetadataDocument document;
                    const auto read = metadata::read_local_metadata(raw, cancellation);
                    if (read) {
                        document = read->document;
                    }
                    for (const auto& tag : probe->tags) {
                        const auto identity = metadata::resolve_text_property_identity(tag.name);
                        if (!document.first_effective_value(identity.canonical_name)) {
                            document.fields.push_back(
                                {.canonical_name = identity.canonical_name,
                                 .native_name = tag.name,
                                 .values = {tag.value},
                                 .qualifier = {},
                                 .provenance = metadata::FieldProvenance::stream});
                        }
                    }
                    tags = tags_from(document, raw);
                }
                if (cancellation.is_cancellation_requested()) {
                    result.cancelled = true;
                    complete = false;
                    break;
                }
                Transaction transaction{db};
                // The commit lock serializes this fresh revision check against
                // metadata and relocation publication's dependent-state update.
                const auto after = core::observe_local_source_revision(raw);
                if (!after || *before != *after) {
                    ++progress.failed;
                    complete = false;
                    continue;
                }
                Statement exists{
                    db, "SELECT 1 FROM local_library_roots WHERE raw_path=? AND scan_token=?"};
                exists.blob(1, root.raw_path);
                exists.text(2, generation);
                if (!exists.next()) {
                    complete = false;
                    break;
                }
                if (tags) {
                    Statement upsert{
                        db,
                        "INSERT INTO "
                        "local_library_tracks(raw_path,root,revision,title,artist,album,album_key,"
                        "release_id,date,disc,track,search_track,search_album,available,seen) "
                        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,1,?) ON CONFLICT(raw_path) DO UPDATE SET "
                        "root=excluded.root,revision=excluded.revision,title=excluded.title,artist="
                        "excluded.artist,album=excluded.album,album_key=excluded.album_key,"
                        "release_id=excluded.release_id,date=excluded.date,disc=excluded.disc,"
                        "track=excluded.track,search_track=excluded.search_track,search_album="
                        "excluded.search_album,available=1,seen=excluded.seen"};
                    upsert.blob(1, raw);
                    upsert.blob(2, root.raw_path);
                    upsert.text(3, revision);
                    bind_tags(upsert, 4, *tags, raw);
                    upsert.text(14, generation);
                    upsert.next();
                    ++progress.indexed;
                } else {
                    Statement touch{db, "UPDATE local_library_tracks SET available=1,seen=? WHERE "
                                        "raw_path=? AND revision=?"};
                    touch.text(1, generation);
                    touch.blob(2, raw);
                    touch.text(3, revision);
                    touch.next();
                }
                transaction.commit();
            }
            if (error) {
                complete = false;
            }
            result.incomplete = result.incomplete || !complete;
            Transaction transaction{db};
            Statement state{db, "UPDATE local_library_roots SET available=1,error=? WHERE "
                                "raw_path=? AND scan_token=?"};
            state.text(1, complete ? "" : "Scan incomplete; previous entries retained");
            state.blob(2, root.raw_path);
            state.text(3, generation);
            state.next();
            if (complete) {
                Statement missing{
                    db, "UPDATE local_library_tracks SET available=0 WHERE root=? AND seen<>? "
                        "AND EXISTS(SELECT 1 FROM local_library_roots WHERE raw_path=root AND "
                        "scan_token=?)"};
                missing.blob(1, root.raw_path);
                missing.text(2, generation);
                missing.text(3, generation);
                missing.next();
            }
            transaction.commit();
        }
        result.cancelled = result.cancelled || cancellation.is_cancellation_requested();
        return result;
    });
}

core::Result<void> refresh_library_source(sqlite3* db, const std::string& source,
                                          const std::string& target,
                                          const metadata::MetadataDocument* document) {
    const auto result = checked([&] {
        Tags tags;
        {
            Statement existing{db,
                               "SELECT title,artist,album,release_id,date,disc,track,search_track "
                               "FROM local_library_tracks WHERE raw_path=?"};
            existing.blob(1, source);
            if (!existing.next()) {
                return true;
            }
            tags = {existing.bytes(0),
                    existing.bytes(1),
                    existing.bytes(2),
                    existing.bytes(3),
                    existing.bytes(4),
                    existing.bytes(7),
                    static_cast<int>(existing.number(5)),
                    static_cast<int>(existing.number(6))};
        }
        std::string root;
        for (const auto& candidate : read_roots(db)) {
            if (contained(target, candidate.raw_path)) {
                root = candidate.raw_path;
                break;
            }
        }
        if (root.empty()) {
            Statement remove{db, "DELETE FROM local_library_tracks WHERE raw_path=?"};
            remove.blob(1, source);
            remove.next();
            return true;
        }
        if (document != nullptr) {
            tags = tags_from(*document, target);
        }
        if (source != target) {
            Statement remove{db, "DELETE FROM local_library_tracks WHERE raw_path=?"};
            remove.blob(1, target);
            remove.next();
        }
        Statement update{
            db,
            "UPDATE local_library_tracks SET "
            "raw_path=?,root=?,revision='',title=?,artist=?,album=?,album_key=?,release_id=?,"
            "date=?,disc=?,track=?,search_track=?,search_album=?,available=1,"
            "seen=(SELECT scan_token FROM local_library_roots WHERE raw_path=?) WHERE raw_path=?"};
        update.blob(1, target);
        update.blob(2, root);
        bind_tags(update, 3, tags, target);
        update.blob(13, root);
        update.blob(14, source);
        update.next();
        return true;
    });
    if (!result) {
        return std::unexpected(result.error());
    }
    return {};
}

} // namespace trackknife::persistence
