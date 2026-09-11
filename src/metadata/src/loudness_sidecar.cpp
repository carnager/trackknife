// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/loudness_sidecar.hpp"

#include "trackknife/core/error.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace trackknife::metadata {
namespace {

[[nodiscard]] core::Error sidecar_error(const core::ErrorCode code, std::string message) {
    return core::Error{.code = code, .message = std::move(message), .context = {}};
}

// Minimal, bounded JSON subset for the fixed tkmeta schema: objects,
// arrays, plain-ASCII strings, and numbers. Anything else fails closed.
struct JsonValue;
using JsonObject = std::vector<std::pair<std::string, JsonValue>>;
using JsonArray = std::vector<JsonValue>;

struct JsonValue {
    // The raw token is retained for numbers so integers can be
    // revalidated with exact round trips.
    std::variant<double, std::string, JsonObject, JsonArray> value;
    std::string number_token;
};

class JsonParser {
  public:
    explicit JsonParser(const std::string_view source) : source_(source) {}

    [[nodiscard]] core::Result<JsonValue> parse() {
        auto result = parse_value(0U);
        if (!result) {
            return result;
        }
        skip_whitespace();
        if (offset_ != source_.size()) {
            return std::unexpected(
                sidecar_error(core::ErrorCode::invalid_argument,
                              "sidecar JSON has trailing content after the document"));
        }
        return result;
    }

  private:
    static constexpr std::size_t maximum_depth = 8U;
    static constexpr std::size_t maximum_members = 32'768U;

    void skip_whitespace() {
        while (offset_ < source_.size() && (source_[offset_] == ' ' || source_[offset_] == '\t' ||
                                            source_[offset_] == '\n' || source_[offset_] == '\r')) {
            ++offset_;
        }
    }

    [[nodiscard]] core::Result<JsonValue> parse_value(const std::size_t depth) {
        if (depth > maximum_depth) {
            return std::unexpected(
                sidecar_error(core::ErrorCode::limit_exceeded, "sidecar JSON nests too deeply"));
        }
        skip_whitespace();
        if (offset_ >= source_.size()) {
            return std::unexpected(
                sidecar_error(core::ErrorCode::invalid_argument, "sidecar JSON ends unexpectedly"));
        }
        const auto head = source_[offset_];
        if (head == '{') {
            return parse_object(depth);
        }
        if (head == '[') {
            return parse_array(depth);
        }
        if (head == '"') {
            auto text = parse_string();
            if (!text) {
                return std::unexpected(std::move(text.error()));
            }
            return JsonValue{.value = std::move(*text), .number_token = {}};
        }
        return parse_number();
    }

    [[nodiscard]] core::Result<JsonValue> parse_object(const std::size_t depth) {
        ++offset_; // consume '{'
        JsonObject members;
        skip_whitespace();
        if (offset_ < source_.size() && source_[offset_] == '}') {
            ++offset_;
            return JsonValue{.value = std::move(members), .number_token = {}};
        }
        while (true) {
            if (members.size() >= maximum_members) {
                return std::unexpected(sidecar_error(core::ErrorCode::limit_exceeded,
                                                     "sidecar JSON object is too large"));
            }
            skip_whitespace();
            auto key = parse_string();
            if (!key) {
                return std::unexpected(std::move(key.error()));
            }
            skip_whitespace();
            if (offset_ >= source_.size() || source_[offset_] != ':') {
                return std::unexpected(sidecar_error(core::ErrorCode::invalid_argument,
                                                     "sidecar JSON object member lacks a colon"));
            }
            ++offset_;
            auto member = parse_value(depth + 1U);
            if (!member) {
                return member;
            }
            members.emplace_back(std::move(*key), std::move(*member));
            skip_whitespace();
            if (offset_ < source_.size() && source_[offset_] == ',') {
                ++offset_;
                continue;
            }
            if (offset_ < source_.size() && source_[offset_] == '}') {
                ++offset_;
                return JsonValue{.value = std::move(members), .number_token = {}};
            }
            return std::unexpected(sidecar_error(core::ErrorCode::invalid_argument,
                                                 "sidecar JSON object is not terminated"));
        }
    }

    [[nodiscard]] core::Result<JsonValue> parse_array(const std::size_t depth) {
        ++offset_; // consume '['
        JsonArray elements;
        skip_whitespace();
        if (offset_ < source_.size() && source_[offset_] == ']') {
            ++offset_;
            return JsonValue{.value = std::move(elements), .number_token = {}};
        }
        while (true) {
            if (elements.size() >= maximum_members) {
                return std::unexpected(sidecar_error(core::ErrorCode::limit_exceeded,
                                                     "sidecar JSON array is too large"));
            }
            auto element = parse_value(depth + 1U);
            if (!element) {
                return element;
            }
            elements.push_back(std::move(*element));
            skip_whitespace();
            if (offset_ < source_.size() && source_[offset_] == ',') {
                ++offset_;
                continue;
            }
            if (offset_ < source_.size() && source_[offset_] == ']') {
                ++offset_;
                return JsonValue{.value = std::move(elements), .number_token = {}};
            }
            return std::unexpected(sidecar_error(core::ErrorCode::invalid_argument,
                                                 "sidecar JSON array is not terminated"));
        }
    }

    // Keys and the schema's few strings are plain printable ASCII;
    // escapes beyond \" and \\ fail closed.
    [[nodiscard]] core::Result<std::string> parse_string() {
        if (offset_ >= source_.size() || source_[offset_] != '"') {
            return std::unexpected(
                sidecar_error(core::ErrorCode::invalid_argument, "sidecar JSON expected a string"));
        }
        ++offset_;
        std::string result;
        while (offset_ < source_.size()) {
            const auto character = source_[offset_];
            if (character == '"') {
                ++offset_;
                return result;
            }
            if (character == '\\') {
                if (offset_ + 1U >= source_.size() ||
                    (source_[offset_ + 1U] != '"' && source_[offset_ + 1U] != '\\')) {
                    return std::unexpected(
                        sidecar_error(core::ErrorCode::unsupported,
                                      "sidecar JSON string uses an unsupported escape"));
                }
                result.push_back(source_[offset_ + 1U]);
                offset_ += 2U;
                continue;
            }
            if (static_cast<unsigned char>(character) < 0x20U) {
                return std::unexpected(sidecar_error(core::ErrorCode::invalid_argument,
                                                     "sidecar JSON string has a control byte"));
            }
            result.push_back(character);
            ++offset_;
        }
        return std::unexpected(sidecar_error(core::ErrorCode::invalid_argument,
                                             "sidecar JSON string is not terminated"));
    }

    [[nodiscard]] core::Result<JsonValue> parse_number() {
        const auto begin = offset_;
        if (offset_ < source_.size() && source_[offset_] == '-') {
            ++offset_;
        }
        while (offset_ < source_.size() &&
               ((source_[offset_] >= '0' && source_[offset_] <= '9') || source_[offset_] == '.' ||
                source_[offset_] == 'e' || source_[offset_] == 'E' || source_[offset_] == '+' ||
                source_[offset_] == '-')) {
            ++offset_;
        }
        const auto token = source_.substr(begin, offset_ - begin);
        double parsed = 0.0;
        const auto ends = std::from_chars(token.data(), token.data() + token.size(), parsed);
        if (token.empty() || ends.ec != std::errc{} || ends.ptr != token.data() + token.size() ||
            !std::isfinite(parsed)) {
            return std::unexpected(sidecar_error(core::ErrorCode::invalid_argument,
                                                 "sidecar JSON contains an invalid number"));
        }
        return JsonValue{.value = parsed, .number_token = std::string{token}};
    }

    std::string_view source_;
    std::size_t offset_{0U};
};

[[nodiscard]] core::Result<const JsonObject*> require_object(const JsonValue& value,
                                                             const std::string_view label) {
    const auto* object = std::get_if<JsonObject>(&value.value);
    if (object == nullptr) {
        return std::unexpected(
            sidecar_error(core::ErrorCode::invalid_argument,
                          "sidecar " + std::string(label) + " must be a JSON object"));
    }
    return object;
}

template <typename Integer>
[[nodiscard]] core::Result<Integer> require_integer(const JsonValue& value,
                                                    const std::string_view label) {
    if (!std::holds_alternative<double>(value.value)) {
        return std::unexpected(
            sidecar_error(core::ErrorCode::invalid_argument,
                          "sidecar " + std::string(label) + " must be a number"));
    }
    Integer result{};
    const auto& token = value.number_token;
    const auto ends = std::from_chars(token.data(), token.data() + token.size(), result);
    if (ends.ec != std::errc{} || ends.ptr != token.data() + token.size()) {
        return std::unexpected(
            sidecar_error(core::ErrorCode::invalid_argument,
                          "sidecar " + std::string(label) + " must be an exact integer"));
    }
    return result;
}

[[nodiscard]] core::Result<double> require_double(const JsonValue& value,
                                                  const std::string_view label) {
    const auto* number = std::get_if<double>(&value.value);
    if (number == nullptr) {
        return std::unexpected(
            sidecar_error(core::ErrorCode::invalid_argument,
                          "sidecar " + std::string(label) + " must be a number"));
    }
    return *number;
}

[[nodiscard]] core::Result<LoudnessSidecarEntry> parse_entry(const JsonValue& value) {
    auto members = require_object(value, "loudness entry");
    if (!members) {
        return std::unexpected(std::move(members.error()));
    }
    LoudnessSidecarEntry entry;
    for (const auto& [key, member] : **members) {
        if (key == "stream_index" || key == "subsong_index") {
            auto parsed = require_integer<int>(member, key);
            if (!parsed || *parsed < 0) {
                return std::unexpected(
                    parsed ? sidecar_error(core::ErrorCode::invalid_argument,
                                           "sidecar " + key + " must be non-negative")
                           : std::move(parsed.error()));
            }
            (key == "stream_index" ? entry.stream_index : entry.subsong_index) = *parsed;
        } else if (key == "start_sample" || key == "end_sample") {
            auto parsed = require_integer<std::int64_t>(member, key);
            if (!parsed || *parsed < 0) {
                return std::unexpected(
                    parsed ? sidecar_error(core::ErrorCode::invalid_argument,
                                           "sidecar " + key + " must be non-negative")
                           : std::move(parsed.error()));
            }
            (key == "start_sample" ? entry.start_sample : entry.end_sample) = *parsed;
        } else if (key == "track_gain_db" || key == "album_gain_db") {
            auto parsed = require_double(member, key);
            if (!parsed || std::abs(*parsed) > 60.0) {
                return std::unexpected(parsed ? sidecar_error(core::ErrorCode::invalid_argument,
                                                              "sidecar " + key + " is out of range")
                                              : std::move(parsed.error()));
            }
            (key == "track_gain_db" ? entry.track_gain_db : entry.album_gain_db) = *parsed;
        } else if (key == "track_peak" || key == "album_peak") {
            auto parsed = require_double(member, key);
            if (!parsed || *parsed < 0.0) {
                return std::unexpected(
                    parsed ? sidecar_error(core::ErrorCode::invalid_argument,
                                           "sidecar " + key + " must be non-negative")
                           : std::move(parsed.error()));
            }
            (key == "track_peak" ? entry.track_peak : entry.album_peak) = *parsed;
        } else {
            return std::unexpected(
                sidecar_error(core::ErrorCode::unsupported,
                              "sidecar loudness entry has unknown key \"" + key + "\""));
        }
    }
    if (entry.start_sample && entry.end_sample && *entry.end_sample <= *entry.start_sample) {
        return std::unexpected(sidecar_error(core::ErrorCode::invalid_argument,
                                             "sidecar entry range must end after it starts"));
    }
    if (entry.empty()) {
        return std::unexpected(sidecar_error(core::ErrorCode::invalid_argument,
                                             "sidecar entry carries no loudness values"));
    }
    return entry;
}

void append_number(std::string& output, const double value) {
    std::array<char, 64> buffer{};
    const auto ends = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    output.append(buffer.data(), ends.ptr);
}

template <typename Integer> void append_integer(std::string& output, const Integer value) {
    std::array<char, 32> buffer{};
    const auto ends = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    output.append(buffer.data(), ends.ptr);
}

} // namespace

std::string loudness_sidecar_path(const std::string& raw_audio_path) {
    return raw_audio_path + ".tkmeta";
}

core::Result<LoudnessSidecar> parse_loudness_sidecar(const std::string_view source,
                                                     const LoudnessSidecarLimits& limits) {
    if (source.size() > limits.source_bytes) {
        return std::unexpected(
            sidecar_error(core::ErrorCode::limit_exceeded, "sidecar exceeds the byte limit"));
    }
    auto document = JsonParser{source}.parse();
    if (!document) {
        return std::unexpected(std::move(document.error()));
    }
    auto root = require_object(*document, "document");
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }

    LoudnessSidecar sidecar;
    bool saw_version = false;
    bool saw_source = false;
    bool saw_loudness = false;
    for (const auto& [key, member] : **root) {
        if (key == "tkmeta") {
            auto version = require_integer<int>(member, key);
            if (!version) {
                return std::unexpected(std::move(version.error()));
            }
            if (*version != 1) {
                return std::unexpected(sidecar_error(core::ErrorCode::unsupported,
                                                     "sidecar version is not supported"));
            }
            saw_version = true;
        } else if (key == "source") {
            auto source_members = require_object(member, key);
            if (!source_members) {
                return std::unexpected(std::move(source_members.error()));
            }
            for (const auto& [source_key, source_member] : **source_members) {
                if (source_key == "size") {
                    auto parsed = require_integer<std::uint64_t>(source_member, source_key);
                    if (!parsed) {
                        return std::unexpected(std::move(parsed.error()));
                    }
                    sidecar.source_size = *parsed;
                } else if (source_key == "modified_seconds" ||
                           source_key == "modified_nanoseconds") {
                    auto parsed = require_integer<std::int64_t>(source_member, source_key);
                    if (!parsed) {
                        return std::unexpected(std::move(parsed.error()));
                    }
                    (source_key == "modified_seconds" ? sidecar.source_modified_seconds
                                                      : sidecar.source_modified_nanoseconds) =
                        *parsed;
                } else {
                    return std::unexpected(
                        sidecar_error(core::ErrorCode::unsupported,
                                      "sidecar source has unknown key \"" + source_key + "\""));
                }
            }
            saw_source = true;
        } else if (key == "loudness") {
            auto loudness = require_object(member, key);
            if (!loudness) {
                return std::unexpected(std::move(loudness.error()));
            }
            for (const auto& [loudness_key, loudness_member] : **loudness) {
                if (loudness_key == "reference_lufs") {
                    auto parsed = require_double(loudness_member, loudness_key);
                    if (!parsed) {
                        return std::unexpected(std::move(parsed.error()));
                    }
                    sidecar.reference_lufs = *parsed;
                } else if (loudness_key == "entries") {
                    const auto* elements = std::get_if<JsonArray>(&loudness_member.value);
                    if (elements == nullptr) {
                        return std::unexpected(
                            sidecar_error(core::ErrorCode::invalid_argument,
                                          "sidecar loudness entries must be an array"));
                    }
                    if (elements->size() > limits.entries) {
                        return std::unexpected(sidecar_error(core::ErrorCode::limit_exceeded,
                                                             "sidecar exceeds the entry limit"));
                    }
                    for (const auto& element : *elements) {
                        auto entry = parse_entry(element);
                        if (!entry) {
                            return std::unexpected(std::move(entry.error()));
                        }
                        for (const auto& existing : sidecar.entries) {
                            if (existing.same_identity(*entry)) {
                                return std::unexpected(
                                    sidecar_error(core::ErrorCode::invalid_argument,
                                                  "sidecar entries duplicate one identity"));
                            }
                        }
                        sidecar.entries.push_back(*entry);
                    }
                } else {
                    return std::unexpected(
                        sidecar_error(core::ErrorCode::unsupported,
                                      "sidecar loudness has unknown key \"" + loudness_key + "\""));
                }
            }
            saw_loudness = true;
        } else {
            return std::unexpected(sidecar_error(core::ErrorCode::unsupported,
                                                 "sidecar has unknown key \"" + key + "\""));
        }
    }
    if (!saw_version || !saw_source || !saw_loudness) {
        return std::unexpected(sidecar_error(core::ErrorCode::invalid_argument,
                                             "sidecar is missing a required section"));
    }
    return sidecar;
}

core::Result<std::string> serialize_loudness_sidecar(const LoudnessSidecar& sidecar) {
    for (const auto& entry : sidecar.entries) {
        if (entry.empty()) {
            return std::unexpected(sidecar_error(core::ErrorCode::invalid_argument,
                                                 "sidecar entry carries no loudness values"));
        }
        const auto gains_valid = (!entry.track_gain_db || std::abs(*entry.track_gain_db) <= 60.0) &&
                                 (!entry.album_gain_db || std::abs(*entry.album_gain_db) <= 60.0);
        const auto peaks_valid = (!entry.track_peak || *entry.track_peak >= 0.0) &&
                                 (!entry.album_peak || *entry.album_peak >= 0.0);
        if (!gains_valid || !peaks_valid) {
            return std::unexpected(sidecar_error(core::ErrorCode::invalid_argument,
                                                 "sidecar entry value is out of range"));
        }
    }

    std::string output;
    output.reserve(256U + sidecar.entries.size() * 160U);
    output += "{\n  \"tkmeta\": 1,\n  \"source\": {\"size\": ";
    append_integer(output, sidecar.source_size);
    output += ", \"modified_seconds\": ";
    append_integer(output, sidecar.source_modified_seconds);
    output += ", \"modified_nanoseconds\": ";
    append_integer(output, sidecar.source_modified_nanoseconds);
    output += "},\n  \"loudness\": {\n    \"reference_lufs\": ";
    append_number(output, sidecar.reference_lufs);
    output += ",\n    \"entries\": [";
    for (std::size_t index = 0U; index < sidecar.entries.size(); ++index) {
        const auto& entry = sidecar.entries[index];
        output += index == 0U ? "\n" : ",\n";
        output += "      {";
        bool first = true;
        const auto member = [&output, &first](const std::string_view key) {
            output += first ? "" : ", ";
            first = false;
            output += '"';
            output += key;
            output += "\": ";
        };
        if (entry.stream_index) {
            member("stream_index");
            append_integer(output, *entry.stream_index);
        }
        if (entry.subsong_index) {
            member("subsong_index");
            append_integer(output, *entry.subsong_index);
        }
        if (entry.start_sample) {
            member("start_sample");
            append_integer(output, *entry.start_sample);
        }
        if (entry.end_sample) {
            member("end_sample");
            append_integer(output, *entry.end_sample);
        }
        if (entry.track_gain_db) {
            member("track_gain_db");
            append_number(output, *entry.track_gain_db);
        }
        if (entry.track_peak) {
            member("track_peak");
            append_number(output, *entry.track_peak);
        }
        if (entry.album_gain_db) {
            member("album_gain_db");
            append_number(output, *entry.album_gain_db);
        }
        if (entry.album_peak) {
            member("album_peak");
            append_number(output, *entry.album_peak);
        }
        output += '}';
    }
    output += sidecar.entries.empty() ? "]\n  }\n}\n" : "\n    ]\n  }\n}\n";
    return output;
}

core::Result<std::optional<LoudnessSidecar>>
read_loudness_sidecar(const std::string& raw_audio_path, const LoudnessSidecarLimits& limits) {
    const auto path = loudness_sidecar_path(raw_audio_path);
    const std::filesystem::path sidecar_path{path};
    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(sidecar_path, status_error);
    if (status_error || !std::filesystem::exists(status)) {
        return std::optional<LoudnessSidecar>{};
    }
    if (!std::filesystem::is_regular_file(status)) {
        return std::unexpected(
            sidecar_error(core::ErrorCode::invalid_argument, "sidecar is not a regular file")
                .with_context("path", core::escape_raw_path(path)));
    }
    std::error_code size_error;
    const auto size = std::filesystem::file_size(sidecar_path, size_error);
    if (size_error) {
        return std::unexpected(sidecar_error(core::ErrorCode::io, "sidecar size could not be read")
                                   .with_context("path", core::escape_raw_path(path)));
    }
    if (size > limits.source_bytes) {
        return std::unexpected(
            sidecar_error(core::ErrorCode::limit_exceeded, "sidecar exceeds the byte limit")
                .with_context("path", core::escape_raw_path(path)));
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    std::ifstream input{sidecar_path, std::ios::binary};
    if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) && !bytes.empty()) {
        return std::unexpected(sidecar_error(core::ErrorCode::io, "sidecar could not be read")
                                   .with_context("path", core::escape_raw_path(path)));
    }
    auto parsed = parse_loudness_sidecar(bytes, limits);
    if (!parsed) {
        auto error = std::move(parsed.error());
        error.context.push_back({.key = "path", .value = core::escape_raw_path(path)});
        return std::unexpected(std::move(error));
    }
    return std::optional{std::move(*parsed)};
}

} // namespace trackknife::metadata
