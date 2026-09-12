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
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <functional>
#include <map>
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
    void reset() {
        check(sqlite3_reset(value_));
        check(sqlite3_clear_bindings(value_));
    }
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
        if (name == "tracknumber" && entry_.track_number > 0) {
            return std::to_string(entry_.track_number);
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
    static const auto track =
        titleformat::compile("$if(%tracknumber%,$num(%tracknumber%,2). ,)%title%", options);
    static const auto found_track = titleformat::compile(
        "%artist% — $if(%tracknumber%,$num(%tracknumber%,2). ,)%title%", options);
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
    // The "2:" prefix versions the indexed representation itself: rows
    // written before migration 30 (ADR-0150) mismatch once and reindex on
    // the next Refresh, backfilling field rows and technical columns.
    return "2:" + std::to_string(revision.device) + ':' + std::to_string(revision.inode) + ':' +
           std::to_string(revision.size) + ':' +
           std::to_string(revision.modification_time_seconds) + ':' +
           std::to_string(revision.modification_time_nanoseconds);
}

// A successful walk alone is insufficient: an unmounted volume can leave an
// accessible empty mountpoint. Require an absent path and a surviving directory
// on the file's previously observed device; uncertainty retains the cache.
bool confirmed_missing(const std::string& raw_path, const std::string& revision,
                       const std::string& root) {
    std::uint64_t device = 0;
    // Rows written since ADR-0150 carry a leading representation-version
    // component; the device follows it. Pre-migration rows start with the
    // device directly.
    std::string_view text{revision};
    if (text.starts_with("2:")) {
        text.remove_prefix(2U);
    }
    const auto separator = text.find(':');
    if (separator == std::string_view::npos) {
        return false;
    }
    const auto parsed = std::from_chars(text.data(), text.data() + separator, device);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + separator) {
        return false;
    }
    struct stat state{};
    if (::lstat(raw_path.c_str(), &state) == 0 || errno != ENOENT) {
        return false;
    }
    auto parent = std::filesystem::path{raw_path}.parent_path();
    while (!parent.empty()) {
        if (::lstat(parent.c_str(), &state) == 0) {
            return S_ISDIR(state.st_mode) && static_cast<std::uint64_t>(state.st_dev) == device;
        }
        if (errno != ENOENT || parent == std::filesystem::path{root} ||
            parent == parent.parent_path()) {
            return false;
        }
        parent = parent.parent_path();
    }
    return false;
}

void prune_missing(sqlite3* db, const std::string& root, const std::string& generation,
                   const core::CancellationToken& cancellation) {
    std::string after;
    while (!cancellation.is_cancellation_requested()) {
        std::vector<std::pair<std::string, std::string>> candidates;
        {
            // Use the schema-28 raw-path primary-key index for keyset paging;
            // the root/seen index would repeatedly sort all missing entries.
            Statement page{db, "SELECT raw_path,revision FROM local_library_tracks "
                               "INDEXED BY sqlite_autoindex_local_library_tracks_1 WHERE "
                               "root=? AND seen<>? AND raw_path>? ORDER BY raw_path LIMIT 200"};
            page.blob(1, root);
            page.text(2, generation);
            page.blob(3, after);
            while (page.next()) {
                candidates.emplace_back(page.bytes(0), page.bytes(1));
            }
        }
        if (candidates.empty()) {
            return;
        }
        after = candidates.back().first;
        // Filesystem checks run outside the write transaction and in bounded pages.
        std::erase_if(candidates, [&](const auto& entry) {
            return cancellation.is_cancellation_requested() ||
                   !confirmed_missing(entry.first, entry.second, root);
        });
        if (cancellation.is_cancellation_requested()) {
            return;
        }
        Transaction transaction{db};
        for (const auto& [path, revision] : candidates) {
            Statement remove{db, "DELETE FROM local_library_tracks WHERE raw_path=? AND root=? "
                                 "AND revision=? AND seen<>? AND EXISTS(SELECT 1 FROM "
                                 "local_library_roots WHERE raw_path=root AND scan_token=?)"};
            remove.blob(1, path);
            remove.blob(2, root);
            remove.text(3, revision);
            remove.text(4, generation);
            remove.text(5, generation);
            remove.next();
        }
        transaction.commit();
    }
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

// ADR-0150: technical properties retained from the probe the scan
// already runs, as typed queryable columns.
struct Technicals {
    std::string codec;
    int sample_rate{0};
    int bits{0};
    int channels{0};
    std::int64_t duration_ms{-1};
};

int bits_from_sample_format(const std::string& format) {
    // FFmpeg spells float formats without a width; everything else
    // carries its bit depth as digits ("s16", "s32p", "u8").
    if (format.starts_with("dbl")) {
        return 64;
    }
    if (format.starts_with("flt")) {
        return 32;
    }
    int result = 0;
    for (const char character : format) {
        if (character >= '0' && character <= '9') {
            result = result * 10 + (character - '0');
        }
    }
    return result;
}

Technicals technicals_from(const formats::MediaProbe& probe) {
    Technicals result;
    result.duration_ms = probe.duration_ms.value_or(-1);
    if (!probe.best_audio_stream) {
        return result;
    }
    const auto found = std::ranges::find(probe.audio_streams, *probe.best_audio_stream,
                                         &formats::AudioStreamInfo::stream_index);
    if (found == probe.audio_streams.end()) {
        return result;
    }
    result.codec = found->codec_name;
    result.sample_rate = found->sample_rate;
    result.bits = bits_from_sample_format(found->sample_format);
    result.channels = found->channels;
    return result;
}

// ADR-0150: one bounded row per tag value, original bytes beside the
// normalized form, replaced wholesale inside the caller's transaction.
constexpr std::size_t maximum_field_names = 64U;
constexpr std::size_t maximum_field_values = 32U;
constexpr std::size_t maximum_field_value_bytes = 2'048U;

void write_field_rows(sqlite3* db, const std::string& raw_path,
                      const metadata::MetadataDocument& document) {
    {
        Statement remove{db, "DELETE FROM local_library_fields WHERE raw_path=?"};
        remove.blob(1, raw_path);
        remove.next();
    }
    Statement insert{db, "INSERT OR IGNORE INTO local_library_fields"
                         "(raw_path,canonical_name,position,value,value_lower) "
                         "VALUES(?,?,?,?,?)"};
    std::map<std::string, int> positions;
    for (const auto& field : document.fields) {
        if (field.canonical_name.empty()) {
            continue;
        }
        auto position = positions.find(field.canonical_name);
        if (position == positions.end()) {
            if (positions.size() >= maximum_field_names) {
                continue;
            }
            position = positions.emplace(field.canonical_name, 0).first;
        }
        for (const auto& value : field.values) {
            if (position->second >= static_cast<int>(maximum_field_values)) {
                break;
            }
            if (value.empty() || value.size() > maximum_field_value_bytes) {
                continue;
            }
            insert.reset();
            insert.blob(1, raw_path);
            insert.text(2, field.canonical_name);
            insert.number(3, position->second);
            insert.blob(4, value);
            insert.blob(5, lower(value));
            insert.next();
            ++position->second;
        }
    }
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

// --- ADR-0150: tkq filter planner and candidate evaluation ---

// One SQL parameter with its binding affinity: value_lower and raw_path
// are BLOB columns, canonical names are TEXT.
struct FilterBinding {
    enum class Kind : std::uint8_t { text, blob } kind{Kind::text};
    std::string value;
};

struct FilterClause {
    std::string sql;
    std::vector<FilterBinding> bindings;
};

// Technical pseudo-fields resolve to typed columns; everything else goes
// through the generic field table.
[[nodiscard]] const char* technical_column(const std::string& canonical) {
    if (canonical == "codec") {
        return "codec_name";
    }
    if (canonical == "samplerate") {
        return "sample_rate";
    }
    if (canonical == "bitspersample") {
        return "bits";
    }
    if (canonical == "channels") {
        return "channels";
    }
    if (canonical == "lengthms") {
        return "duration_ms";
    }
    return nullptr;
}

[[nodiscard]] std::string canonical_query_field(const std::string& field) {
    return metadata::canonicalize_field_name(field);
}

// Translates one predicate to SQL against alias t, or nullopt when the
// predicate needs per-row evaluation (tkfmt expressions).
[[nodiscard]] std::optional<FilterClause>
translate_predicate(const query::TkqPredicate& predicate) {
    using query::TkqComparison;
    using query::TkqOperandKind;
    FilterClause clause;
    if (predicate.operand == TkqOperandKind::expression) {
        return std::nullopt;
    }
    if (predicate.operand == TkqOperandKind::any_field) {
        // Every word occurs in the denormalized search text or any field value.
        std::string sql;
        for (const auto& word : predicate.words) {
            if (!sql.empty()) {
                sql += " AND ";
            }
            sql += "(instr(t.search_track,?)>0 OR EXISTS(SELECT 1 FROM local_library_fields f "
                   "WHERE f.raw_path=t.raw_path AND instr(f.value_lower,?)>0))";
            clause.bindings.push_back({FilterBinding::Kind::text, word});
            clause.bindings.push_back({FilterBinding::Kind::blob, word});
        }
        clause.sql = "(" + sql + ")";
        return clause;
    }
    const auto canonical = canonical_query_field(predicate.field);
    if (const auto* column = technical_column(canonical)) {
        const auto qualified = std::string{"t."} + column;
        switch (predicate.comparison) {
        case TkqComparison::is:
            clause.sql = "(" + qualified + "=?)";
            clause.bindings.push_back({FilterBinding::Kind::text, predicate.normalized});
            return clause;
        case TkqComparison::has: {
            std::string sql;
            for (const auto& word : predicate.words) {
                if (!sql.empty()) {
                    sql += " AND ";
                }
                sql += "instr(" + qualified + ",?)>0";
                clause.bindings.push_back({FilterBinding::Kind::text, word});
            }
            clause.sql = "(" + sql + ")";
            return clause;
        }
        case TkqComparison::greater:
        case TkqComparison::less:
        case TkqComparison::equal: {
            const auto* comparator = predicate.comparison == TkqComparison::greater ? ">"
                                     : predicate.comparison == TkqComparison::less  ? "<"
                                                                                    : "=";
            clause.sql = "(" + qualified + comparator + std::to_string(predicate.number) + ")";
            return clause;
        }
        case TkqComparison::present:
            clause.sql = canonical == "codec"      ? "(" + qualified + "<>'')"
                         : canonical == "lengthms" ? "(" + qualified + ">=0)"
                                                   : "(" + qualified + ">0)";
            return clause;
        case TkqComparison::missing:
            clause.sql = canonical == "codec"      ? "(" + qualified + "='')"
                         : canonical == "lengthms" ? "(" + qualified + "<0)"
                                                   : "(" + qualified + "<=0)";
            return clause;
        }
        return std::nullopt;
    }
    if (canonical == "date" && (predicate.comparison == TkqComparison::greater ||
                                predicate.comparison == TkqComparison::less ||
                                predicate.comparison == TkqComparison::equal)) {
        const auto* comparator = predicate.comparison == TkqComparison::greater ? ">"
                                 : predicate.comparison == TkqComparison::less  ? "<"
                                                                                : "=";
        clause.sql = "(t.date<>'' AND CAST(substr(t.date,1,4) AS INTEGER)" +
                     std::string{comparator} + std::to_string(predicate.number) + ")";
        return clause;
    }
    constexpr auto exists_head = "EXISTS(SELECT 1 FROM local_library_fields f WHERE "
                                 "f.raw_path=t.raw_path AND f.canonical_name=?";
    switch (predicate.comparison) {
    case TkqComparison::is:
        clause.sql = std::string{"("} + exists_head + " AND f.value_lower=?))";
        clause.bindings.push_back({FilterBinding::Kind::text, canonical});
        clause.bindings.push_back({FilterBinding::Kind::blob, predicate.normalized});
        break;
    case TkqComparison::has: {
        std::string sql;
        for (const auto& word : predicate.words) {
            if (!sql.empty()) {
                sql += " AND ";
            }
            sql += std::string{exists_head} + " AND instr(f.value_lower,?)>0)";
            clause.bindings.push_back({FilterBinding::Kind::text, canonical});
            clause.bindings.push_back({FilterBinding::Kind::blob, word});
        }
        clause.sql = "(" + sql + ")";
        break;
    }
    case TkqComparison::greater:
    case TkqComparison::less:
    case TkqComparison::equal: {
        const auto* comparator = predicate.comparison == TkqComparison::greater ? ">"
                                 : predicate.comparison == TkqComparison::less  ? "<"
                                                                                : "=";
        clause.sql = std::string{"("} + exists_head + " AND CAST(f.value_lower AS INTEGER)" +
                     comparator + std::to_string(predicate.number) + "))";
        clause.bindings.push_back({FilterBinding::Kind::text, canonical});
        break;
    }
    case TkqComparison::present:
        clause.sql = std::string{"("} + exists_head + "))";
        clause.bindings.push_back({FilterBinding::Kind::text, canonical});
        break;
    case TkqComparison::missing:
        clause.sql = std::string{"(NOT "} + exists_head + "))";
        clause.bindings.push_back({FilterBinding::Kind::text, canonical});
        break;
    }
    return clause;
}

[[nodiscard]] std::optional<FilterClause> translate_node(const query::CompiledTkq& compiled,
                                                         const std::size_t index) {
    const auto& node = compiled.nodes[index];
    switch (node.kind) {
    case query::TkqNodeKind::predicate:
        return translate_predicate(compiled.predicates[node.predicate_index]);
    case query::TkqNodeKind::not_node: {
        auto inner = translate_node(compiled, node.children.front());
        if (!inner) {
            return std::nullopt;
        }
        inner->sql = "(NOT " + inner->sql + ")";
        return inner;
    }
    case query::TkqNodeKind::and_node:
    case query::TkqNodeKind::or_node: {
        FilterClause clause;
        const auto* joiner = node.kind == query::TkqNodeKind::and_node ? " AND " : " OR ";
        std::string sql;
        for (const auto child : node.children) {
            auto translated = translate_node(compiled, child);
            if (!translated) {
                return std::nullopt;
            }
            if (!sql.empty()) {
                sql += joiner;
            }
            sql += translated->sql;
            clause.bindings.insert(clause.bindings.end(),
                                   std::make_move_iterator(translated->bindings.begin()),
                                   std::make_move_iterator(translated->bindings.end()));
        }
        clause.sql = "(" + sql + ")";
        return clause;
    }
    }
    return std::nullopt;
}

// A candidate row with everything per-row evaluation and sorting need.
struct FilterRow {
    std::string raw_path;
    std::string title;
    std::string artist;
    std::string album;
    std::string album_key;
    std::string date;
    std::string search_track;
    int disc{0};
    int track{0};
    std::string codec_name;
    std::int64_t sample_rate{0};
    std::int64_t bits{0};
    std::int64_t channels{0};
    std::int64_t duration_ms{-1};
    // canonical name -> values in order, original beside normalized.
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> fields;
};

class FilterRowContext final : public titleformat::EvaluationContext {
  public:
    FilterRowContext(const FilterRow& row, const titleformat::FormatContextKind kind)
        : row_(row), kind_(kind) {}
    titleformat::FormatContextKind kind() const noexcept override { return kind_; }
    std::optional<std::string> resolveField(std::string_view name) const override {
        const auto canonical = metadata::canonicalize_field_name(name);
        const auto found = row_.fields.find(canonical);
        if (found != row_.fields.end() && !found->second.empty()) {
            return found->second.front().first;
        }
        // The track row keeps display fallbacks even when the tag is absent.
        if (canonical == "title") {
            return row_.title;
        }
        if (canonical == "artist" || canonical == "albumartist") {
            return row_.artist;
        }
        if (canonical == "album") {
            return row_.album;
        }
        return std::nullopt;
    }
    std::optional<MetadataValues> resolveMetadata(std::string_view name) const override {
        const auto canonical = metadata::canonicalize_field_name(name);
        const auto found = row_.fields.find(canonical);
        if (found == row_.fields.end()) {
            return std::nullopt;
        }
        MetadataValues values;
        values.reserve(found->second.size());
        for (const auto& [original, normalized] : found->second) {
            static_cast<void>(normalized);
            values.push_back(original);
        }
        return values;
    }
    std::optional<std::string> resolveTechnicalInfo(std::string_view name) const override {
        const auto canonical = metadata::canonicalize_field_name(name);
        if (canonical == "codec") {
            return row_.codec_name.empty() ? std::nullopt : std::optional{row_.codec_name};
        }
        if (canonical == "samplerate" && row_.sample_rate > 0) {
            return std::to_string(row_.sample_rate);
        }
        if (canonical == "bitspersample" && row_.bits > 0) {
            return std::to_string(row_.bits);
        }
        if (canonical == "channels" && row_.channels > 0) {
            return std::to_string(row_.channels);
        }
        if (canonical == "lengthms" && row_.duration_ms >= 0) {
            return std::to_string(row_.duration_ms);
        }
        return std::nullopt;
    }

  private:
    const FilterRow& row_;
    titleformat::FormatContextKind kind_;
};

[[nodiscard]] std::optional<std::int64_t> leading_integer(const std::string& text) {
    std::int64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr == text.data()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] bool compare_number(const std::int64_t value, const query::TkqComparison comparison,
                                  const std::int64_t operand) {
    switch (comparison) {
    case query::TkqComparison::greater:
        return value > operand;
    case query::TkqComparison::less:
        return value < operand;
    default:
        return value == operand;
    }
}

[[nodiscard]] bool evaluate_text_comparison(const std::string& normalized_text,
                                            const query::TkqPredicate& predicate) {
    using query::TkqComparison;
    switch (predicate.comparison) {
    case TkqComparison::is:
        return normalized_text == predicate.normalized;
    case TkqComparison::has:
        return std::ranges::all_of(predicate.words, [&](const std::string& word) {
            return normalized_text.find(word) != std::string::npos;
        });
    case TkqComparison::present:
        return !normalized_text.empty();
    case TkqComparison::missing:
        return normalized_text.empty();
    case TkqComparison::greater:
    case TkqComparison::less:
    case TkqComparison::equal: {
        const auto value = leading_integer(normalized_text);
        return value && compare_number(*value, predicate.comparison, predicate.number);
    }
    }
    return false;
}

[[nodiscard]] bool evaluate_predicate(const query::CompiledTkq& compiled,
                                      const query::TkqPredicate& predicate, const FilterRow& row,
                                      const core::CancellationToken& cancellation) {
    using query::TkqComparison;
    using query::TkqOperandKind;
    if (predicate.operand == TkqOperandKind::expression) {
        const FilterRowContext context{row, titleformat::FormatContextKind::grouping};
        titleformat::EvaluationOptions options;
        options.cancellation = cancellation;
        const auto value =
            titleformat::evaluate(compiled.programs[predicate.program_index], context, options);
        if (!value) {
            return false;
        }
        return evaluate_text_comparison(lower(value->text), predicate);
    }
    if (predicate.operand == TkqOperandKind::any_field) {
        return std::ranges::all_of(predicate.words, [&](const std::string& word) {
            if (row.search_track.find(word) != std::string::npos) {
                return true;
            }
            for (const auto& [name, values] : row.fields) {
                static_cast<void>(name);
                for (const auto& [original, normalized] : values) {
                    static_cast<void>(original);
                    if (normalized.find(word) != std::string::npos) {
                        return true;
                    }
                }
            }
            return false;
        });
    }
    const auto canonical = canonical_query_field(predicate.field);
    if (technical_column(canonical) != nullptr) {
        if (canonical == "codec") {
            return evaluate_text_comparison(row.codec_name, predicate);
        }
        const auto value = canonical == "samplerate"      ? row.sample_rate
                           : canonical == "bitspersample" ? row.bits
                           : canonical == "channels"      ? row.channels
                                                          : row.duration_ms;
        const auto present = canonical == "lengthms" ? value >= 0 : value > 0;
        switch (predicate.comparison) {
        case TkqComparison::present:
            return present;
        case TkqComparison::missing:
            return !present;
        case TkqComparison::is:
        case TkqComparison::has:
            return present && evaluate_text_comparison(std::to_string(value), predicate);
        default:
            return present && compare_number(value, predicate.comparison, predicate.number);
        }
    }
    if (canonical == "date" && (predicate.comparison == TkqComparison::greater ||
                                predicate.comparison == TkqComparison::less ||
                                predicate.comparison == TkqComparison::equal)) {
        const auto year = leading_integer(row.date);
        return year && compare_number(*year, predicate.comparison, predicate.number);
    }
    const auto found = row.fields.find(canonical);
    switch (predicate.comparison) {
    case TkqComparison::present:
        return found != row.fields.end();
    case TkqComparison::missing:
        return found == row.fields.end();
    case TkqComparison::is:
        return found != row.fields.end() &&
               std::ranges::any_of(found->second, [&](const auto& value) {
                   return value.second == predicate.normalized;
               });
    case TkqComparison::has:
        return found != row.fields.end() &&
               std::ranges::all_of(predicate.words, [&](const std::string& word) {
                   return std::ranges::any_of(found->second, [&](const auto& value) {
                       return value.second.find(word) != std::string::npos;
                   });
               });
    case TkqComparison::greater:
    case TkqComparison::less:
    case TkqComparison::equal:
        return found != row.fields.end() &&
               std::ranges::any_of(found->second, [&](const auto& value) {
                   const auto number = leading_integer(value.second);
                   return number && compare_number(*number, predicate.comparison, predicate.number);
               });
    }
    return false;
}

[[nodiscard]] bool evaluate_node(const query::CompiledTkq& compiled, const std::size_t index,
                                 const FilterRow& row,
                                 const core::CancellationToken& cancellation) {
    const auto& node = compiled.nodes[index];
    switch (node.kind) {
    case query::TkqNodeKind::predicate:
        return evaluate_predicate(compiled, compiled.predicates[node.predicate_index], row,
                                  cancellation);
    case query::TkqNodeKind::not_node:
        return !evaluate_node(compiled, node.children.front(), row, cancellation);
    case query::TkqNodeKind::and_node:
        return std::ranges::all_of(node.children, [&](const std::size_t child) {
            return evaluate_node(compiled, child, row, cancellation);
        });
    case query::TkqNodeKind::or_node:
        return std::ranges::any_of(node.children, [&](const std::size_t child) {
            return evaluate_node(compiled, child, row, cancellation);
        });
    }
    return false;
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
            columns = "artist,artist,artist,'',count(*),sum(available),0";
            order = " GROUP BY artist ORDER BY artist COLLATE NOCASE";
            break;
        case LibraryEntryKind::album:
            columns = "album_key,min(album),min(artist),min(album),count(*),sum(available),0";
            order = " GROUP BY album_key ORDER BY min(artist) COLLATE NOCASE,min(date),min(album) "
                    "COLLATE NOCASE,album_key";
            break;
        case LibraryEntryKind::track:
            columns = "raw_path,title,artist,album,1,available,track";
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
                                    static_cast<std::size_t>(statement.number(5)),
                                    static_cast<int>(statement.number(6))});
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

namespace {

constexpr auto filter_columns =
    "t.raw_path,t.title,t.artist,t.album,t.album_key,t.date,t.search_track,t.disc,t.track,"
    "t.codec_name,t.sample_rate,t.bits,t.channels,t.duration_ms";
constexpr auto filter_order = " ORDER BY t.artist COLLATE NOCASE,t.album_key,t.disc,t.track,"
                              "t.title COLLATE NOCASE,t.raw_path";
constexpr std::size_t filter_match_cap = 100'000U;

struct FilterPlan {
    std::optional<FilterClause> pushed;
    bool residual{false};
};

[[nodiscard]] FilterPlan plan_filter(const query::CompiledTkq& compiled) {
    FilterPlan plan;
    if (compiled.match_all) {
        return plan;
    }
    if (auto whole = translate_node(compiled, compiled.root)) {
        plan.pushed = std::move(whole);
        return plan;
    }
    plan.residual = true;
    // Pushable AND-conjuncts still pre-filter the candidate stream; the
    // full tree re-evaluates per row, so pushing is purely an optimization.
    const auto& root = compiled.nodes[compiled.root];
    if (root.kind == query::TkqNodeKind::and_node) {
        FilterClause partial;
        std::string sql;
        for (const auto child : root.children) {
            auto translated = translate_node(compiled, child);
            if (!translated) {
                continue;
            }
            if (!sql.empty()) {
                sql += " AND ";
            }
            sql += translated->sql;
            partial.bindings.insert(partial.bindings.end(),
                                    std::make_move_iterator(translated->bindings.begin()),
                                    std::make_move_iterator(translated->bindings.end()));
        }
        if (!sql.empty()) {
            partial.sql = "(" + sql + ")";
            plan.pushed = std::move(partial);
        }
    }
    return plan;
}

void bind_clause(Statement& statement, const FilterClause& clause) {
    int index = 1;
    for (const auto& binding : clause.bindings) {
        if (binding.kind == FilterBinding::Kind::blob) {
            statement.blob(index++, binding.value);
        } else {
            statement.text(index++, binding.value);
        }
    }
}

[[nodiscard]] std::string filter_where(const FilterPlan& plan) {
    std::string where = " WHERE t.available=1";
    if (plan.pushed) {
        where += " AND " + plan.pushed->sql;
    }
    return where;
}

void load_field_rows(Statement& statement, const std::string& raw_path, FilterRow& row) {
    statement.reset();
    statement.blob(1, raw_path);
    while (statement.next()) {
        row.fields[statement.bytes(0)].emplace_back(statement.bytes(1), statement.bytes(2));
    }
}

// Materializes the bounded match set in the default library order,
// evaluating residual predicates per row; sorting happens afterwards.
[[nodiscard]] std::vector<FilterRow>
collect_filter_matches(sqlite3* db, const query::CompiledTkq& compiled, const FilterPlan& plan,
                       const core::CancellationToken& cancellation) {
    const auto need_rows = plan.residual || compiled.sort.has_value();
    Statement select{db, std::string{"SELECT "} + filter_columns + " FROM local_library_tracks t" +
                             filter_where(plan) + filter_order};
    if (plan.pushed) {
        bind_clause(select, *plan.pushed);
    }
    Statement fields{db, "SELECT canonical_name,value,value_lower FROM local_library_fields "
                         "WHERE raw_path=? ORDER BY canonical_name,position"};
    std::vector<FilterRow> matches;
    while (select.next()) {
        if (cancellation.is_cancellation_requested()) {
            fail("Library query cancelled", core::ErrorCode::cancelled);
        }
        FilterRow row;
        row.raw_path = select.bytes(0);
        row.title = select.bytes(1);
        row.artist = select.bytes(2);
        row.album = select.bytes(3);
        row.album_key = select.bytes(4);
        row.date = select.bytes(5);
        row.search_track = select.bytes(6);
        row.disc = static_cast<int>(select.number(7));
        row.track = static_cast<int>(select.number(8));
        row.codec_name = select.bytes(9);
        row.sample_rate = select.number(10);
        row.bits = select.number(11);
        row.channels = select.number(12);
        row.duration_ms = select.number(13);
        if (need_rows) {
            load_field_rows(fields, row.raw_path, row);
        }
        if (plan.residual && !evaluate_node(compiled, compiled.root, row, cancellation)) {
            continue;
        }
        if (matches.size() == filter_match_cap) {
            fail("The query matches more than 100000 files; narrow it",
                 core::ErrorCode::limit_exceeded);
        }
        matches.push_back(std::move(row));
    }
    if (compiled.sort) {
        struct Keyed {
            std::string key;
            std::size_t position;
        };
        std::vector<Keyed> keyed;
        keyed.reserve(matches.size());
        for (std::size_t position = 0U; position < matches.size(); ++position) {
            const FilterRowContext context{matches[position], titleformat::FormatContextKind::sort};
            titleformat::EvaluationOptions options;
            options.cancellation = cancellation;
            auto value = titleformat::evaluate(compiled.sort->program, context, options);
            if (!value) {
                throw value.error();
            }
            keyed.push_back({lower(value->text), position});
        }
        const auto descending = compiled.sort->direction == query::TkqSortDirection::descending;
        // Stable over the default library order, so equal keys keep the
        // deterministic raw_path-terminated ordering as their tiebreaker.
        std::ranges::stable_sort(keyed, [descending](const Keyed& left, const Keyed& right) {
            return descending ? right.key < left.key : left.key < right.key;
        });
        std::vector<FilterRow> sorted;
        sorted.reserve(matches.size());
        for (const auto& entry : keyed) {
            sorted.push_back(std::move(matches[entry.position]));
        }
        matches = std::move(sorted);
    }
    return matches;
}

[[nodiscard]] LibraryEntry filter_entry(const FilterRow& row) {
    LibraryEntry entry{
        LibraryEntryKind::track, row.raw_path, row.title, row.artist, row.album, 1U, 1U, row.track};
    entry.label = format_label(entry, true);
    return entry;
}

} // namespace

core::Result<LibraryPage> LocalLibrary::filter(const query::CompiledTkq& compiled,
                                               const std::size_t offset, const std::size_t limit,
                                               const core::CancellationToken& cancellation) const {
    return checked([&] {
        auto* db = implementation_->db;
        QueryCancellation guard{db, cancellation};
        const auto plan = plan_filter(compiled);
        const auto page_limit = std::clamp<std::size_t>(limit, 1U, 200U);
        LibraryPage page;
        if (!plan.residual && !compiled.sort) {
            // Fully indexable and unsorted: page in SQL like ordinary queries.
            Statement statement{db, std::string{"SELECT "} + filter_columns +
                                        " FROM local_library_tracks t" + filter_where(plan) +
                                        filter_order + " LIMIT " + std::to_string(page_limit + 1U) +
                                        " OFFSET " +
                                        std::to_string(std::min<std::size_t>(offset, 1'000'000U))};
            if (plan.pushed) {
                bind_clause(statement, *plan.pushed);
            }
            while (statement.next()) {
                if (page.entries.size() == page_limit) {
                    page.more = true;
                    break;
                }
                FilterRow row;
                row.raw_path = statement.bytes(0);
                row.title = statement.bytes(1);
                row.artist = statement.bytes(2);
                row.album = statement.bytes(3);
                row.track = static_cast<int>(statement.number(8));
                page.entries.push_back(filter_entry(row));
            }
            return page;
        }
        const auto matches = collect_filter_matches(db, compiled, plan, cancellation);
        for (auto position = offset; position < matches.size(); ++position) {
            if (page.entries.size() == page_limit) {
                page.more = true;
                break;
            }
            page.entries.push_back(filter_entry(matches[position]));
        }
        return page;
    });
}

core::Result<std::vector<std::string>>
LocalLibrary::filter_paths(const query::CompiledTkq& compiled,
                           const core::CancellationToken& cancellation) const {
    return checked([&] {
        auto* db = implementation_->db;
        QueryCancellation guard{db, cancellation};
        const auto plan = plan_filter(compiled);
        if (!plan.residual && !compiled.sort) {
            Statement statement{db, std::string{"SELECT t.raw_path FROM local_library_tracks t"} +
                                        filter_where(plan) + filter_order + " LIMIT 100001"};
            if (plan.pushed) {
                bind_clause(statement, *plan.pushed);
            }
            std::vector<std::string> result;
            while (statement.next()) {
                if (result.size() == filter_match_cap) {
                    fail("The query matches more than 100000 files; narrow it",
                         core::ErrorCode::limit_exceeded);
                }
                result.push_back(statement.bytes(0));
            }
            return result;
        }
        const auto matches = collect_filter_matches(db, compiled, plan, cancellation);
        std::vector<std::string> result;
        result.reserve(matches.size());
        for (const auto& row : matches) {
            result.push_back(row.raw_path);
        }
        return result;
    });
}

core::Result<std::optional<std::string>>
LocalLibrary::artwork_source(const std::string& album_key,
                             const core::CancellationToken& cancellation) const {
    return checked([&]() -> std::optional<std::string> {
        auto* db = implementation_->db;
        QueryCancellation guard{db, cancellation};
        Statement source{db, "SELECT raw_path FROM local_library_tracks WHERE album_key=? "
                             "AND available=1 ORDER BY disc,track,raw_path LIMIT 1"};
        source.text(1, album_key);
        if (!source.next()) {
            return std::nullopt;
        }
        return source.bytes(0);
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
                metadata::MetadataDocument document;
                Technicals technicals;
                if (!unchanged) {
                    auto probe = formats::probe_local_media(raw, cancellation);
                    if (!probe || !probe->best_audio_stream) {
                        ++progress.failed;
                        complete = false;
                        continue;
                    }
                    technicals = technicals_from(*probe);
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
                        "release_id,date,disc,track,search_track,search_album,available,seen,"
                        "codec_name,sample_rate,bits,channels,duration_ms) "
                        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,1,?,?,?,?,?,?) "
                        "ON CONFLICT(raw_path) DO UPDATE SET "
                        "root=excluded.root,revision=excluded.revision,title=excluded.title,artist="
                        "excluded.artist,album=excluded.album,album_key=excluded.album_key,"
                        "release_id=excluded.release_id,date=excluded.date,disc=excluded.disc,"
                        "track=excluded.track,search_track=excluded.search_track,search_album="
                        "excluded.search_album,available=1,seen=excluded.seen,"
                        "codec_name=excluded.codec_name,sample_rate=excluded.sample_rate,"
                        "bits=excluded.bits,channels=excluded.channels,"
                        "duration_ms=excluded.duration_ms"};
                    upsert.blob(1, raw);
                    upsert.blob(2, root.raw_path);
                    upsert.text(3, revision);
                    bind_tags(upsert, 4, *tags, raw);
                    upsert.text(14, generation);
                    upsert.text(15, technicals.codec);
                    upsert.number(16, technicals.sample_rate);
                    upsert.number(17, technicals.bits);
                    upsert.number(18, technicals.channels);
                    upsert.number(19, technicals.duration_ms);
                    upsert.next();
                    write_field_rows(db, raw, document);
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
            if (error || cancellation.is_cancellation_requested()) {
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
            if (complete) {
                prune_missing(db, root.raw_path, generation, cancellation);
            }
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
        // ADR-0150: field rows reference the track row, and the rename below
        // would strand them (enforced foreign keys reject an updated parent
        // key with surviving children). Carry a move's rows across in memory;
        // a tag commit rebuilds them from the fresh document afterwards.
        struct FieldRow {
            std::string canonical_name;
            int position;
            std::string value;
            std::string value_lower;
        };
        std::vector<FieldRow> retained;
        if (document == nullptr) {
            Statement select{db, "SELECT canonical_name,position,value,value_lower FROM "
                                 "local_library_fields WHERE raw_path=?"};
            select.blob(1, source);
            while (select.next()) {
                retained.push_back({select.bytes(0), static_cast<int>(select.number(1)),
                                    select.bytes(2), select.bytes(3)});
            }
        }
        {
            Statement remove{db, "DELETE FROM local_library_fields WHERE raw_path=?"};
            remove.blob(1, source);
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
        if (document != nullptr) {
            write_field_rows(db, target, *document);
        } else if (!retained.empty()) {
            Statement insert{db, "INSERT OR IGNORE INTO local_library_fields"
                                 "(raw_path,canonical_name,position,value,value_lower) "
                                 "VALUES(?,?,?,?,?)"};
            for (const auto& row : retained) {
                insert.reset();
                insert.blob(1, target);
                insert.text(2, row.canonical_name);
                insert.number(3, row.position);
                insert.blob(4, row.value);
                insert.blob(5, row.value_lower);
                insert.next();
            }
        }
        return true;
    });
    if (!result) {
        return std::unexpected(result.error());
    }
    return {};
}

} // namespace trackknife::persistence
