// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/core/stable_id.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/metadata/mp3_writer.hpp"
#include "trackknife/metadata/ogg_writer.hpp"
#include "trackknife/metadata/staged_patch.hpp"
#include "trackknife/metadata/staged_selection.hpp"
#include "trackknife/metadata/write_plan.hpp"

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
                ("trackknife-ogg-writer-" + trackknife::core::StableId::random().to_string());
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

// Bit-exact decoded PCM: the packet proof from the writer plus this decode
// comparison together pin that a tag rewrite cannot touch the audio.
[[nodiscard]] bool decoded_pcm_matches(const std::filesystem::path& first,
                                       const std::filesystem::path& second) {
    auto decode_all = [](const std::filesystem::path& path) -> std::optional<std::vector<float>> {
        auto decoder = trackknife::formats::AudioDecoder::open(path.native());
        if (!decoder) {
            return std::nullopt;
        }
        std::vector<float> samples;
        while (true) {
            auto chunk = decoder->next_chunk();
            if (!chunk) {
                return std::nullopt;
            }
            if (!*chunk) {
                break;
            }
            samples.insert(samples.end(), (*chunk)->interleaved_samples.begin(),
                           (*chunk)->interleaved_samples.end());
        }
        return samples;
    };
    const auto first_samples = decode_all(first);
    const auto second_samples = decode_all(second);
    return first_samples && second_samples && *first_samples == *second_samples;
}

void roundTripsCommentEditsPreservingPackets(const std::filesystem::path& fixture_directory,
                                             const std::string_view fixture,
                                             const std::string_view expected_adapter) {
    TemporaryDirectory directory;
    const auto source = materialize(fixture_directory, fixture, directory.path() / "source.ogg");

    const auto read = trackknife::metadata::read_local_metadata(source.native());
    CHECK(read.has_value());
    if (!read) {
        return;
    }
    CHECK(read->adapter_name == expected_adapter);
    CHECK(read->capabilities.fields_writable);
    CHECK(read->capabilities.unknown_data_preserved_on_write);
    CHECK(!read->capabilities.pictures_writable);
    CHECK(read->document.first_effective_value("title") ==
          std::optional<std::string>{"Fixture Tone"});

    auto selection = trackknife::metadata::StagedMetadataSelection::create(
        {trackknife::metadata::StagedMetadataSource{
            .raw_path = read->raw_path,
            .source_revision = read->source_revision,
            .baseline = read->document,
        }});
    CHECK(selection.has_value());
    if (!selection) {
        return;
    }
    const auto title = selection->field_index("title");
    const auto artist = selection->field_index("artist");
    const auto track_id =
        selection->ensure_missing_field("musicbrainztrackid", "MUSICBRAINZ_TRACKID");
    const auto totals = selection->ensure_missing_field("totaltracks", "TOTALTRACKS");
    CHECK(title.has_value() && artist.has_value() && track_id.has_value() && totals.has_value());
    if (!title || !artist || !track_id || !totals) {
        return;
    }
    trackknife::metadata::StagedMetadataPatchSet patches;
    CHECK(patches.replace_values(*selection, 0U, *title, {"Prepared Tone"}).has_value());
    CHECK(patches.remove_field(*selection, 0U, *artist).has_value());
    CHECK(
        patches.replace_values(*selection, 0U, *track_id, {"01b43c31-1b23-42f0-a5d5-275288538230"})
            .has_value());
    CHECK(patches.replace_values(*selection, 0U, *totals, {"9"}).has_value());

    auto plan = trackknife::metadata::revalidate_metadata_write_plan(*selection, patches);
    CHECK(plan.has_value() && plan->ready());
    if (!plan || !plan->ready()) {
        return;
    }
    const auto prepared_path = directory.path() / "prepared.ogg";
    const auto prepared = trackknife::metadata::prepare_ogg_metadata_write_copy(
        plan->sources.front(), prepared_path.native());
    if (!prepared) {
        std::cerr << fixture << ": " << prepared.error().message << '\n';
    }
    CHECK(prepared.has_value());
    if (!prepared) {
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
    CHECK(reread->document.first_effective_value("totaltracks") == std::optional<std::string>{"9"});

    CHECK(decoded_pcm_matches(source, prepared_path));

    CHECK(trackknife::metadata::is_qualified_text_adapter(expected_adapter));
}

} // namespace

int main(const int argc, char** argv) {
    CHECK(argc == 2);
    if (argc == 2) {
        const std::filesystem::path fixture_directory{argv[1]};
        roundTripsCommentEditsPreservingPackets(fixture_directory, "tagged-tone-vorbis.b64",
                                                "taglib-vorbis-v1");
        roundTripsCommentEditsPreservingPackets(fixture_directory, "tagged-tone-opus.b64",
                                                "taglib-opus-v1");
    }
    return failures == 0 ? 0 : 1;
}
