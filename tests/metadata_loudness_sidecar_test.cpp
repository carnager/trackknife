// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/core/error.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/metadata/loudness_sidecar.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void check(const bool condition, const std::string_view expression, const int line) {
    if (!condition) {
        std::cerr << "line " << line << ": check failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

using trackknife::metadata::LoudnessSidecar;
using trackknife::metadata::LoudnessSidecarEntry;

[[nodiscard]] LoudnessSidecar sample_sidecar() {
    LoudnessSidecar sidecar;
    sidecar.source_size = 123'456U;
    sidecar.source_modified_seconds = 1'757'500'000;
    sidecar.source_modified_nanoseconds = 42;
    sidecar.entries = {
        LoudnessSidecarEntry{
            .stream_index = std::nullopt,
            .subsong_index = std::nullopt,
            .start_sample = 0,
            .end_sample = 8'820'000,
            .track_gain_db = -3.46,
            .track_peak = 0.994629,
            .album_gain_db = -5.53,
            .album_peak = 0.994629,
        },
        LoudnessSidecarEntry{
            .stream_index = 0,
            .subsong_index = 2,
            .start_sample = std::nullopt,
            .end_sample = std::nullopt,
            .track_gain_db = 1.25,
            .track_peak = std::nullopt,
            .album_gain_db = std::nullopt,
            .album_peak = std::nullopt,
        },
    };
    return sidecar;
}

void roundTripsEveryFieldExactly() {
    const auto original = sample_sidecar();
    const auto serialized = trackknife::metadata::serialize_loudness_sidecar(original);
    CHECK(serialized.has_value());
    if (!serialized) {
        return;
    }
    const auto parsed = trackknife::metadata::parse_loudness_sidecar(*serialized);
    CHECK(parsed.has_value());
    CHECK(parsed && *parsed == original);

    trackknife::core::LocalSourceRevision matching{
        .device = 1U,
        .inode = 2U,
        .size = 123'456U,
        .modification_time_seconds = 1'757'500'000,
        .modification_time_nanoseconds = 42,
    };
    CHECK(original.matches(matching));
    // Device/inode differences (copies) do not invalidate the sidecar,
    // size or mtime differences do.
    matching.device = 9U;
    matching.inode = 9U;
    CHECK(original.matches(matching));
    matching.size = 1U;
    CHECK(!original.matches(matching));
}

void failsClosedOnUnknownContent() {
    using trackknife::core::ErrorCode;
    const auto unknown_key = trackknife::metadata::parse_loudness_sidecar(
        R"({"tkmeta": 1, "source": {"size": 1, "modified_seconds": 1,)"
        R"( "modified_nanoseconds": 0}, "loudness": {"entries": []}, "extra": 1})");
    CHECK(!unknown_key);
    CHECK(!unknown_key && unknown_key.error().code == ErrorCode::unsupported);

    const auto future_version = trackknife::metadata::parse_loudness_sidecar(
        R"({"tkmeta": 2, "source": {"size": 1, "modified_seconds": 1,)"
        R"( "modified_nanoseconds": 0}, "loudness": {"entries": []}})");
    CHECK(!future_version);
    CHECK(!future_version && future_version.error().code == ErrorCode::unsupported);

    const auto missing_section = trackknife::metadata::parse_loudness_sidecar(R"({"tkmeta": 1})");
    CHECK(!missing_section);

    const auto duplicate_identity = trackknife::metadata::parse_loudness_sidecar(
        R"({"tkmeta": 1, "source": {"size": 1, "modified_seconds": 1,)"
        R"( "modified_nanoseconds": 0}, "loudness": {"entries": [)"
        R"({"track_gain_db": -1.0}, {"track_gain_db": -2.0}]}})");
    CHECK(!duplicate_identity);

    const auto out_of_range = trackknife::metadata::parse_loudness_sidecar(
        R"({"tkmeta": 1, "source": {"size": 1, "modified_seconds": 1,)"
        R"( "modified_nanoseconds": 0}, "loudness": {"entries": [)"
        R"({"track_gain_db": 100.0}]}})");
    CHECK(!out_of_range);

    const auto fractional_integer = trackknife::metadata::parse_loudness_sidecar(
        R"({"tkmeta": 1, "source": {"size": 1.5, "modified_seconds": 1,)"
        R"( "modified_nanoseconds": 0}, "loudness": {"entries": []}})");
    CHECK(!fractional_integer);

    const auto not_json = trackknife::metadata::parse_loudness_sidecar("REM GENRE Ambient");
    CHECK(!not_json);
}

void readsFilesAndTreatsAbsenceAsEmpty() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("trackknife-sidecar-" + trackknife::core::StableId::random().to_string());
    std::error_code fs_error;
    CHECK(std::filesystem::create_directories(root, fs_error));
    const auto audio = (root / "album.flac").native();

    const auto absent = trackknife::metadata::read_loudness_sidecar(audio);
    CHECK(absent.has_value());
    CHECK(absent && !absent->has_value());

    const auto sidecar = sample_sidecar();
    const auto serialized = trackknife::metadata::serialize_loudness_sidecar(sidecar);
    CHECK(serialized.has_value());
    if (serialized) {
        std::ofstream output{trackknife::metadata::loudness_sidecar_path(audio), std::ios::binary};
        output << *serialized;
    }
    const auto read_back = trackknife::metadata::read_loudness_sidecar(audio);
    CHECK(read_back.has_value());
    CHECK(read_back && read_back->has_value() && **read_back == sidecar);

    {
        std::ofstream output{trackknife::metadata::loudness_sidecar_path(audio), std::ios::binary};
        output << "not json";
    }
    const auto corrupt = trackknife::metadata::read_loudness_sidecar(audio);
    CHECK(!corrupt);

    std::filesystem::remove_all(root, fs_error);
}

} // namespace

int main() {
    roundTripsEveryFieldExactly();
    failsClosedOnUnknownContent();
    readsFilesAndTreatsAbsenceAsEmpty();
    return failures == 0 ? 0 : 1;
}
