// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/core/stable_id.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/metadata/mp3_writer.hpp"
#include "trackknife/metadata/staged_patch.hpp"
#include "trackknife/metadata/staged_selection.hpp"
#include "trackknife/metadata/write_plan.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const std::string_view expression, const int line) {
    if (!condition) {
        std::cerr << "line " << line << ": check failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() /
                ("trackknife-mp3-writer-" + trackknife::core::StableId::random().to_string());
        std::filesystem::create_directory(path_);
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

[[nodiscard]] std::optional<std::vector<unsigned char>>
decode_base64_file(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return std::nullopt;
    }
    const std::string encoded{std::istreambuf_iterator<char>{input},
                              std::istreambuf_iterator<char>{}};
    std::array<int, 256> values{};
    values.fill(-1);
    constexpr std::string_view alphabet{
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
    for (std::size_t index = 0U; index < alphabet.size(); ++index) {
        values[static_cast<unsigned char>(alphabet[index])] = static_cast<int>(index);
    }
    std::vector<unsigned char> decoded;
    unsigned accumulator = 0U;
    unsigned bits = 0U;
    for (const auto character : encoded) {
        if (character == '=') {
            break;
        }
        const auto value = values[static_cast<unsigned char>(character)];
        if (value < 0) {
            if (character == '\r' || character == '\n' || character == ' ' || character == '\t') {
                continue;
            }
            return std::nullopt;
        }
        accumulator = (accumulator << 6U) | static_cast<unsigned>(value);
        bits += 6U;
        if (bits >= 8U) {
            bits -= 8U;
            decoded.push_back(static_cast<unsigned char>((accumulator >> bits) & 0xFFU));
        }
    }
    return decoded;
}

[[nodiscard]] std::filesystem::path materialize(const std::filesystem::path& fixture_directory,
                                                const std::string_view fixture,
                                                const std::filesystem::path& destination) {
    const auto decoded = decode_base64_file(fixture_directory / fixture);
    std::ofstream output{destination, std::ios::binary};
    if (decoded) {
        output.write(reinterpret_cast<const char*>(decoded->data()),
                     static_cast<std::streamsize>(decoded->size()));
    }
    return destination;
}

[[nodiscard]] std::vector<unsigned char> read_bytes(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

// The MPEG audio bytes after the leading ID3v2 tag (syncsafe-sized).
[[nodiscard]] std::vector<unsigned char> audio_region(const std::vector<unsigned char>& bytes) {
    std::size_t begin = 0U;
    if (bytes.size() > 10U && bytes[0] == 'I' && bytes[1] == 'D' && bytes[2] == '3') {
        std::size_t size = 0U;
        for (std::size_t index = 6U; index < 10U; ++index) {
            size = (size << 7U) | (bytes[index] & 0x7FU);
        }
        begin = 10U + size + (((bytes[5] & 0x10U) != 0U) ? 10U : 0U);
    }
    std::size_t end = bytes.size();
    if (end >= 128U && bytes[end - 128U] == 'T' && bytes[end - 127U] == 'A' &&
        bytes[end - 126U] == 'G') {
        end -= 128U;
    }
    return {bytes.begin() + static_cast<std::ptrdiff_t>(begin),
            bytes.begin() + static_cast<std::ptrdiff_t>(end)};
}

[[nodiscard]] std::optional<trackknife::metadata::StagedMetadataSelection>
selection_for(const trackknife::metadata::LocalMetadataRead& read) {
    auto selection = trackknife::metadata::StagedMetadataSelection::create(
        {trackknife::metadata::StagedMetadataSource{
            .raw_path = read.raw_path,
            .source_revision = read.source_revision,
            .baseline = read.document,
        }});
    if (!selection) {
        std::cerr << selection.error().message << '\n';
        return std::nullopt;
    }
    return std::move(*selection);
}

[[nodiscard]] std::optional<trackknife::metadata::MetadataWritePlan>
make_plan(const trackknife::metadata::StagedMetadataSelection& selection,
          const trackknife::metadata::StagedMetadataPatchSet& patches) {
    auto plan = trackknife::metadata::revalidate_metadata_write_plan(selection, patches);
    if (!plan) {
        std::cerr << plan.error().message << '\n';
        return std::nullopt;
    }
    return std::move(*plan);
}

void roundTripsId3TextEditsPreservingAudio(const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto source =
        materialize(fixture_directory, "tagged-tone-mp3.b64", directory.path() / "roundtrip.mp3");
    const auto source_bytes = read_bytes(source);
    CHECK(!source_bytes.empty());

    const auto read = trackknife::metadata::read_local_metadata(source.native());
    CHECK(read.has_value());
    if (!read) {
        return;
    }
    CHECK(read->adapter_name == "taglib-mpeg-v1");
    CHECK(read->capabilities.fields_writable);
    CHECK(read->capabilities.unknown_data_preserved_on_write);
    CHECK(!read->capabilities.pictures_writable);
    CHECK(read->document.first_effective_value("title") ==
          std::optional<std::string>{"Fixture Tone"});

    auto selection = selection_for(*read);
    if (!selection) {
        return;
    }
    const auto title = selection->field_index("title");
    const auto artist = selection->field_index("artist");
    const auto track_id =
        selection->ensure_missing_field("musicbrainztrackid", "MUSICBRAINZ_TRACKID");
    const auto date = selection->ensure_missing_field("date", "Date");
    CHECK(title.has_value() && artist.has_value());
    CHECK(track_id.has_value() && date.has_value());
    if (!title || !artist || !track_id || !date) {
        return;
    }
    trackknife::metadata::StagedMetadataPatchSet patches;
    CHECK(patches.replace_values(*selection, 0U, *title, {"Prepared Tone"}).has_value());
    CHECK(patches.remove_field(*selection, 0U, *artist).has_value());
    CHECK(
        patches.replace_values(*selection, 0U, *track_id, {"01b43c31-1b23-42f0-a5d5-275288538230"})
            .has_value());
    CHECK(patches.replace_values(*selection, 0U, *date, {"1999-09-09"}).has_value());

    auto plan = make_plan(*selection, patches);
    CHECK(plan && plan->ready());
    if (!plan || !plan->ready()) {
        return;
    }
    const auto prepared_path = directory.path() / "roundtrip-prepared.mp3";
    const auto prepared = trackknife::metadata::prepare_mp3_metadata_write_copy(
        plan->sources.front(), prepared_path.native());
    CHECK(prepared.has_value());
    if (!prepared) {
        std::cerr << prepared.error().message << '\n';
        return;
    }
    CHECK(prepared->field_change_count == 4U);

    const auto reread = trackknife::metadata::read_local_metadata(prepared_path.native());
    CHECK(reread.has_value());
    if (!reread) {
        return;
    }
    CHECK(reread->document.first_effective_value("title") ==
          std::optional<std::string>{"Prepared Tone"});
    CHECK(!reread->document.first_effective_value("artist"));
    CHECK(reread->document.first_effective_value("musicbrainztrackid") ==
          std::optional<std::string>{"01b43c31-1b23-42f0-a5d5-275288538230"});
    CHECK(reread->document.first_effective_value("date") ==
          std::optional<std::string>{"1999-09-09"});

    // The MPEG audio frames are byte-identical even though the leading
    // ID3v2 tag resized.
    const auto prepared_bytes = read_bytes(prepared_path);
    CHECK(audio_region(source_bytes) == audio_region(prepared_bytes));

    // The dispatcher routes taglib-mpeg-v1 plans to this writer.
    const auto adapters_ok =
        trackknife::metadata::is_qualified_text_adapter("taglib-mpeg-v1") &&
        trackknife::metadata::is_qualified_text_adapter("taglib-flac-v1") &&
        trackknife::metadata::is_qualified_text_adapter("taglib-wavpack-v1") &&
        !trackknife::metadata::is_qualified_text_adapter("taglib-properties-v1");
    CHECK(adapters_ok);
}

void toleratesId3v1TrailersUnlikeWavPack(const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto source =
        materialize(fixture_directory, "tagged-tone-mp3.b64", directory.path() / "id3v1.mp3");
    {
        std::ofstream append{source, std::ios::binary | std::ios::app};
        std::array<char, 128> id3v1{};
        id3v1[0] = 'T';
        id3v1[1] = 'A';
        id3v1[2] = 'G';
        append.write(id3v1.data(), static_cast<std::streamsize>(id3v1.size()));
    }
    const auto source_bytes = read_bytes(source);
    const auto read = trackknife::metadata::read_local_metadata(source.native());
    CHECK(read.has_value());
    if (!read) {
        return;
    }
    auto selection = selection_for(*read);
    if (!selection) {
        return;
    }
    const auto title = selection->field_index("title");
    CHECK(title.has_value());
    if (!title) {
        return;
    }
    trackknife::metadata::StagedMetadataPatchSet patches;
    CHECK(patches.replace_values(*selection, 0U, *title, {"Trailered"}).has_value());
    auto plan = make_plan(*selection, patches);
    CHECK(plan && plan->ready());
    if (!plan || !plan->ready()) {
        return;
    }
    const auto prepared_path = directory.path() / "id3v1-prepared.mp3";
    const auto prepared = trackknife::metadata::prepare_mp3_metadata_write_copy(
        plan->sources.front(), prepared_path.native());
    CHECK(prepared.has_value());
    if (!prepared) {
        std::cerr << prepared.error().message << '\n';
        return;
    }
    const auto reread = trackknife::metadata::read_local_metadata(prepared_path.native());
    CHECK(reread.has_value());
    CHECK(reread && reread->document.first_effective_value("title") ==
                        std::optional<std::string>{"Trailered"});
    // ID3v1 is a rewritable trailer for MP3; the audio between the tags
    // stayed byte-identical.
    CHECK(audio_region(source_bytes) == audio_region(read_bytes(prepared_path)));
}

} // namespace

int main(const int argc, char** argv) {
    CHECK(argc == 2);
    if (argc == 2) {
        const std::filesystem::path fixture_directory{argv[1]};
        roundTripsId3TextEditsPreservingAudio(fixture_directory);
        toleratesId3v1TrailersUnlikeWavPack(fixture_directory);
    }
    return failures == 0 ? 0 : 1;
}
