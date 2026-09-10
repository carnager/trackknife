// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/core/stable_id.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/metadata/mp3_writer.hpp"
#include "trackknife/metadata/mp4_writer.hpp"
#include "trackknife/metadata/staged_patch.hpp"
#include "trackknife/metadata/staged_selection.hpp"
#include "trackknife/metadata/write_plan.hpp"

#include <taglib/mp4coverart.h>
#include <taglib/mp4file.h>
#include <taglib/mp4item.h>
#include <taglib/tpropertymap.h>

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
                ("trackknife-mp4-writer-" + trackknife::core::StableId::random().to_string());
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

// An independent top-level box walk: returns the payload of the first box
// with the requested type, empty when absent.
[[nodiscard]] std::vector<unsigned char> box_bytes(const std::vector<unsigned char>& bytes,
                                                   const std::string_view type) {
    std::size_t offset = 0U;
    while (offset + 8U <= bytes.size()) {
        auto size = (static_cast<std::size_t>(bytes[offset]) << 24U) |
                    (static_cast<std::size_t>(bytes[offset + 1U]) << 16U) |
                    (static_cast<std::size_t>(bytes[offset + 2U]) << 8U) |
                    static_cast<std::size_t>(bytes[offset + 3U]);
        const std::string_view name{reinterpret_cast<const char*>(bytes.data()) + offset + 4U, 4U};
        if (size == 0U) {
            size = bytes.size() - offset;
        }
        if (size < 8U || size > bytes.size() - offset) {
            return {};
        }
        if (name == type) {
            return {bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                    bytes.begin() + static_cast<std::ptrdiff_t>(offset + size)};
        }
        offset += size;
    }
    return {};
}

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

// ADR-0136: standard atoms, an exact freeform addition, the combined trkn
// total, and a pre-existing covr atom must all survive a prepared-copy tag
// write while ftyp/mdat stay byte-identical and the PCM is unchanged.
void roundTripsAtomEditsPreservingBoxes(const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto source =
        materialize(fixture_directory, "tagged-tone-m4a.b64", directory.path() / "source.m4a");

    // Give the source a cover atom and the combined track total up front —
    // the writer must carry both through untouched picture-wise.
    const std::vector<unsigned char> cover_pixels{0xFFU, 0xD8U, 0xFFU, 0xDBU, 0x00U,
                                                  0x04U, 0x01U, 0x02U, 0xFFU, 0xD9U};
    {
        TagLib::MP4::File file{source.c_str()};
        CHECK(file.isValid());
        TagLib::MP4::CoverArtList covers;
        covers.append(TagLib::MP4::CoverArt{
            TagLib::MP4::CoverArt::JPEG,
            TagLib::ByteVector{reinterpret_cast<const char*>(cover_pixels.data()),
                               static_cast<unsigned int>(cover_pixels.size())}});
        file.tag()->setItem("covr", TagLib::MP4::Item{covers});
        auto properties = file.properties();
        properties.replace("TRACKNUMBER", TagLib::String{"3/12", TagLib::String::UTF8});
        CHECK(file.setProperties(properties).isEmpty());
        CHECK(file.save());
    }

    const auto read = trackknife::metadata::read_local_metadata(source.native());
    CHECK(read.has_value());
    if (!read) {
        return;
    }
    CHECK(read->adapter_name == "taglib-mp4-v1");
    CHECK(read->capabilities.fields_writable);
    CHECK(read->capabilities.unknown_data_preserved_on_write);
    CHECK(!read->capabilities.pictures_writable);
    CHECK(read->document.first_effective_value("title") ==
          std::optional<std::string>{"Fixture Tone"});
    CHECK(read->document.first_effective_value("tracknumber") ==
          std::optional<std::string>{"3/12"});

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
    const auto album = selection->field_index("album");
    const auto tracknumber = selection->field_index("tracknumber");
    const auto genre = selection->ensure_missing_field("genre", "GENRE");
    const auto freeform = selection->ensure_missing_field("customfield", "CUSTOMFIELD");
    CHECK(title.has_value() && album.has_value() && tracknumber.has_value() && genre.has_value() &&
          freeform.has_value());
    if (!title || !album || !tracknumber || !genre || !freeform) {
        return;
    }
    trackknife::metadata::StagedMetadataPatchSet patches;
    CHECK(patches.replace_values(*selection, 0U, *title, {"Prepared Tone"}).has_value());
    CHECK(patches.remove_field(*selection, 0U, *album).has_value());
    CHECK(patches.replace_values(*selection, 0U, *tracknumber, {"7/12"}).has_value());
    CHECK(patches.replace_values(*selection, 0U, *genre, {"Test Tone"}).has_value());
    CHECK(patches.replace_values(*selection, 0U, *freeform, {"Freeform Value"}).has_value());

    auto plan = trackknife::metadata::revalidate_metadata_write_plan(*selection, patches);
    CHECK(plan.has_value() && plan->ready());
    if (!plan || !plan->ready()) {
        return;
    }
    const auto prepared_path = directory.path() / "prepared.m4a";
    const auto prepared = trackknife::metadata::prepare_mp4_metadata_write_copy(
        plan->sources.front(), prepared_path.native());
    if (!prepared) {
        std::cerr << prepared.error().message << '\n';
    }
    CHECK(prepared.has_value());
    if (!prepared) {
        return;
    }
    CHECK(prepared->field_change_count == 5U);

    const auto reread = trackknife::metadata::read_local_metadata(prepared_path.native());
    CHECK(reread.has_value());
    if (!reread) {
        return;
    }
    CHECK(reread->document.first_effective_value("title") ==
          std::optional<std::string>{"Prepared Tone"});
    CHECK(!reread->document.first_effective_value("album"));
    CHECK(reread->document.first_effective_value("tracknumber") ==
          std::optional<std::string>{"7/12"});
    CHECK(reread->document.first_effective_value("genre") ==
          std::optional<std::string>{"Test Tone"});
    CHECK(reread->document.first_effective_value("customfield") ==
          std::optional<std::string>{"Freeform Value"});

    // The freeform addition and the untouched cover keep their exact native
    // atom identity in the prepared copy.
    {
        TagLib::MP4::File file{prepared_path.c_str()};
        CHECK(file.isValid());
        CHECK(file.tag()->contains("----:com.apple.iTunes:CUSTOMFIELD"));
        CHECK(file.tag()->contains("covr"));
        const auto covers = file.tag()->item("covr").toCoverArtList();
        CHECK(covers.size() == 1U);
        const auto& data = covers.front().data();
        CHECK(std::vector<unsigned char>(data.begin(), data.end()) == cover_pixels);
    }

    const auto source_bytes = read_bytes(source);
    const auto prepared_bytes = read_bytes(prepared_path);
    CHECK(!box_bytes(source_bytes, "ftyp").empty());
    CHECK(box_bytes(source_bytes, "ftyp") == box_bytes(prepared_bytes, "ftyp"));
    CHECK(!box_bytes(source_bytes, "mdat").empty());
    CHECK(box_bytes(source_bytes, "mdat") == box_bytes(prepared_bytes, "mdat"));

    CHECK(decoded_pcm_matches(source, prepared_path));

    CHECK(trackknife::metadata::is_qualified_text_adapter("taglib-mp4-v1"));
    CHECK(!trackknife::metadata::is_qualified_text_adapter("taglib-properties-v1"));
}

} // namespace

int main(const int argc, char** argv) {
    CHECK(argc == 2);
    if (argc == 2) {
        roundTripsAtomEditsPreservingBoxes(std::filesystem::path{argv[1]});
    }
    return failures == 0 ? 0 : 1;
}
