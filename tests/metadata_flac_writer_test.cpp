// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/formats/artwork.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/metadata/flac_writer.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/metadata/staged_patch.hpp"
#include "trackknife/metadata/staged_selection.hpp"
#include "trackknife/metadata/write_plan.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
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
    TemporaryDirectory()
        : path_{std::filesystem::temp_directory_path() /
                ("trackknife-flac-writer-" + trackknife::core::StableId::random().to_string())} {
        std::error_code error;
        CHECK(std::filesystem::create_directory(path_, error));
        CHECK(!error);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        CHECK(!error);
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
        const auto byte = static_cast<unsigned char>(character);
        const auto value = values[byte];
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
    const auto bytes = decode_base64_file(fixture_directory / fixture);
    CHECK(bytes.has_value());
    if (bytes) {
        std::ofstream output{destination, std::ios::binary};
        output.write(reinterpret_cast<const char*>(bytes->data()),
                     static_cast<std::streamsize>(bytes->size()));
        CHECK(output.good());
    }
    return destination;
}

[[nodiscard]] std::vector<unsigned char> read_bytes(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void add_unknown_application_block(const std::filesystem::path& path) {
    auto bytes = read_bytes(path);
    CHECK(bytes.size() >= 42U);
    CHECK(std::ranges::equal(std::span{bytes}.first<4>(),
                             std::array<unsigned char, 4>{'f', 'L', 'a', 'C'}));
    if (bytes.size() < 42U) {
        return;
    }
    const auto first_length = (static_cast<std::size_t>(bytes[5]) << 16U) |
                              (static_cast<std::size_t>(bytes[6]) << 8U) |
                              static_cast<std::size_t>(bytes[7]);
    const auto insertion = 8U + first_length;
    CHECK(insertion <= bytes.size());
    constexpr std::array<unsigned char, 16> application_block{
        0x02U, 0x00U, 0x00U, 0x0CU, 't', 'k', 'U', 'T',
        0x00U, 0xFFU, 0x7EU, 0x81U, 'p', 'r', 'o', 'b',
    };
    bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(insertion), application_block.begin(),
                 application_block.end());
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    CHECK(output.good());
}

[[nodiscard]] std::vector<std::vector<unsigned char>>
application_payloads(const std::filesystem::path& path) {
    const auto bytes = read_bytes(path);
    std::vector<std::vector<unsigned char>> payloads;
    if (bytes.size() < 8U) {
        return payloads;
    }
    std::size_t offset = 4U;
    bool last = false;
    while (!last && offset + 4U <= bytes.size()) {
        last = (bytes[offset] & 0x80U) != 0U;
        const auto type = bytes[offset] & 0x7FU;
        const auto length = (static_cast<std::size_t>(bytes[offset + 1U]) << 16U) |
                            (static_cast<std::size_t>(bytes[offset + 2U]) << 8U) |
                            static_cast<std::size_t>(bytes[offset + 3U]);
        offset += 4U;
        if (length > bytes.size() - offset) {
            return {};
        }
        if (type == 2U) {
            payloads.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                  bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
        }
        offset += length;
    }
    return payloads;
}

[[nodiscard]] std::optional<std::vector<float>> decode_all(const std::filesystem::path& path) {
    auto decoder = trackknife::formats::AudioDecoder::open(path.native());
    CHECK(decoder.has_value());
    if (!decoder) {
        return std::nullopt;
    }
    std::vector<float> samples;
    while (true) {
        auto chunk = decoder->next_chunk();
        CHECK(chunk.has_value());
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
}

[[nodiscard]] std::optional<trackknife::metadata::StagedMetadataSelection>
selection_for(const trackknife::metadata::LocalMetadataRead& read) {
    auto selection = trackknife::metadata::StagedMetadataSelection::create({
        trackknife::metadata::StagedMetadataSource{
            .raw_path = read.raw_path,
            .source_revision = read.source_revision,
            .baseline = read.document,
        },
    });
    CHECK(selection.has_value());
    if (!selection) {
        return std::nullopt;
    }
    return std::move(*selection);
}

[[nodiscard]] std::optional<trackknife::metadata::MetadataWritePlan>
make_plan(const trackknife::metadata::StagedMetadataSelection& selection,
          const trackknife::metadata::StagedMetadataPatchSet& patches) {
    auto plan = trackknife::metadata::revalidate_metadata_write_plan(selection, patches);
    CHECK(plan.has_value());
    if (!plan) {
        return std::nullopt;
    }
    return std::move(*plan);
}

[[nodiscard]] bool has_issue(const trackknife::metadata::MetadataWritePlanSource& source,
                             const trackknife::metadata::MetadataWritePlanIssueKind kind) {
    return std::ranges::any_of(source.issues,
                               [kind](const auto& issue) { return issue.kind == kind; });
}

[[nodiscard]] std::optional<std::filesystem::path>
prepare_replaygain_source(const std::filesystem::path& fixture_directory,
                          const TemporaryDirectory& directory) {
    const auto source = materialize(fixture_directory, "rich-metadata-flac.b64",
                                    directory.path() / "baseline.flac");
    const auto read = trackknife::metadata::read_local_metadata(source.native());
    CHECK(read.has_value());
    if (!read) {
        return std::nullopt;
    }
    auto selection = selection_for(*read);
    if (!selection) {
        return std::nullopt;
    }
    const auto gain =
        selection->ensure_missing_field("REPLAYGAIN_TRACK_GAIN", "REPLAYGAIN_TRACK_GAIN");
    const auto peak =
        selection->ensure_missing_field("REPLAYGAIN_TRACK_PEAK", "REPLAYGAIN_TRACK_PEAK");
    CHECK(gain.has_value());
    CHECK(peak.has_value());
    if (!gain || !peak) {
        return std::nullopt;
    }
    trackknife::metadata::StagedMetadataPatchSet patches;
    CHECK(patches.replace_values(*selection, 0U, *gain, {"-7.25 dB"}).has_value());
    CHECK(patches.replace_values(*selection, 0U, *peak, {"0.987654"}).has_value());
    auto plan = make_plan(*selection, patches);
    CHECK(plan && plan->ready());
    if (!plan || !plan->ready()) {
        return std::nullopt;
    }
    const auto enriched = directory.path() / "preservation-source.flac";
    const auto prepared = trackknife::metadata::prepare_flac_metadata_write_copy(
        plan->sources.front(), enriched.native());
    CHECK(prepared.has_value());
    if (!prepared) {
        return std::nullopt;
    }
    add_unknown_application_block(enriched);
    return enriched;
}

void preservesNativeFlacWhileApplyingExactTextChanges(
    const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto source = prepare_replaygain_source(fixture_directory, directory);
    CHECK(source.has_value());
    if (!source) {
        return;
    }
    const auto source_bytes = read_bytes(*source);
    const auto source_applications = application_payloads(*source);
    CHECK(source_applications ==
          (std::vector<std::vector<unsigned char>>{
              {'t', 'k', 'U', 'T', 0x00U, 0xFFU, 0x7EU, 0x81U, 'p', 'r', 'o', 'b'}}));
    const auto source_pcm = decode_all(*source);
    CHECK(source_pcm.has_value());

    const auto read = trackknife::metadata::read_local_metadata(source->native());
    CHECK(read.has_value());
    if (!read) {
        return;
    }
    auto selection = selection_for(*read);
    if (!selection) {
        return;
    }
    const auto artist = selection->field_index("ARTIST");
    const auto album = selection->field_index("ALBUM");
    const auto recording_id = selection->field_index("MUSICBRAINZ_TRACKID");
    const auto custom = selection->ensure_missing_field("EXPERIMENTAL_FIELD", "EXPERIMENTAL_FIELD");
    CHECK(artist.has_value());
    CHECK(album.has_value());
    CHECK(recording_id.has_value());
    CHECK(custom.has_value());
    if (!artist || !album || !recording_id || !custom) {
        return;
    }
    trackknife::metadata::StagedMetadataPatchSet patches;
    CHECK(patches
              .replace_values(*selection, 0U, *artist,
                              {"Second Artist", "First Artist", "Second Artist"})
              .has_value());
    CHECK(patches.remove_field(*selection, 0U, *album).has_value());
    CHECK(
        patches
            .replace_values(*selection, 0U, *recording_id, {"99999999-9999-9999-9999-999999999999"})
            .has_value());
    CHECK(patches.replace_values(*selection, 0U, *custom, {"first", "second"}).has_value());

    auto plan = make_plan(*selection, patches);
    CHECK(plan.has_value());
    CHECK(plan && plan->ready());
    CHECK(plan && plan->blocking_issue_count() == 0U);
    CHECK(plan && plan->sources.front().adapter_name == "taglib-flac-v1");
    if (!plan || !plan->ready()) {
        return;
    }
    const auto prepared_path = directory.path() / "prepared.flac";
    const auto prepared = trackknife::metadata::prepare_flac_metadata_write_copy(
        plan->sources.front(), prepared_path.native());
    CHECK(prepared.has_value());
    if (!prepared) {
        std::cerr << prepared.error().message << '\n';
        return;
    }
    CHECK(prepared->source_revision == read->source_revision);
    CHECK(prepared->field_change_count == 4U);
    CHECK(read_bytes(*source) == source_bytes);
    CHECK(application_payloads(prepared_path) == source_applications);
    CHECK(prepared->document.effective_values("artist") ==
          (std::vector<std::string>{"Second Artist", "First Artist", "Second Artist"}));
    CHECK(!prepared->document.first_effective_value("album").has_value());
    CHECK(prepared->document.effective_values("experimental_field") ==
          (std::vector<std::string>{"first", "second"}));
    CHECK(prepared->document.effective_values("musicbrainz_trackid") ==
          (std::vector<std::string>{"99999999-9999-9999-9999-999999999999"}));
    CHECK(prepared->document.effective_values("replaygain_track_gain") ==
          (std::vector<std::string>{"-7.25 dB"}));
    CHECK(prepared->document.effective_values("replaygain_track_peak") ==
          (std::vector<std::string>{"0.987654"}));
    const auto prepared_pcm = decode_all(prepared_path);
    CHECK(prepared_pcm == source_pcm);

    const auto prepared_bytes = read_bytes(prepared_path);
    const auto existing = trackknife::metadata::prepare_flac_metadata_write_copy(
        plan->sources.front(), prepared_path.native());
    CHECK(!existing.has_value());
    CHECK(!existing && existing.error().code == trackknife::core::ErrorCode::conflict);
    CHECK(read_bytes(prepared_path) == prepared_bytes);
}

void preservesEmbeddedArtworkAndDecodedAudio(const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto source =
        materialize(fixture_directory, "art-tone-flac.b64", directory.path() / "art-source.flac");
    const auto artwork = trackknife::formats::load_embedded_artwork(source.native());
    const auto pcm = decode_all(source);
    const auto read = trackknife::metadata::read_local_metadata(source.native());
    CHECK(artwork.has_value());
    CHECK(pcm.has_value());
    CHECK(read.has_value());
    if (!artwork || !pcm || !read) {
        return;
    }
    auto selection = selection_for(*read);
    if (!selection) {
        return;
    }
    const auto title = selection->field_index("TITLE");
    CHECK(title.has_value());
    if (!title) {
        return;
    }
    trackknife::metadata::StagedMetadataPatchSet patches;
    CHECK(patches.replace_values(*selection, 0U, *title, {"Artwork preserved"}).has_value());
    auto plan = make_plan(*selection, patches);
    CHECK(plan && plan->ready());
    if (!plan || !plan->ready()) {
        return;
    }
    const auto destination = directory.path() / "art-prepared.flac";
    const auto prepared = trackknife::metadata::prepare_flac_metadata_write_copy(
        plan->sources.front(), destination.native());
    CHECK(prepared.has_value());
    const auto prepared_artwork = trackknife::formats::load_embedded_artwork(destination.native());
    CHECK(prepared_artwork == artwork);
    CHECK(decode_all(destination) == pcm);
}

void blocksUnrepresentablePlansAndStaleOrCancelledWrites(
    const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto source =
        materialize(fixture_directory, "rich-metadata-flac.b64", directory.path() / "source.flac");
    const auto read = trackknife::metadata::read_local_metadata(source.native());
    CHECK(read.has_value());
    if (!read) {
        return;
    }

    auto empty_selection = selection_for(*read);
    const auto artist = empty_selection ? empty_selection->field_index("ARTIST") : std::nullopt;
    CHECK(empty_selection.has_value());
    CHECK(artist.has_value());
    if (!empty_selection || !artist) {
        return;
    }
    trackknife::metadata::StagedMetadataPatchSet empty_patches;
    CHECK(empty_patches.replace_values(*empty_selection, 0U, *artist, {""}).has_value());
    auto empty_plan = make_plan(*empty_selection, empty_patches);
    CHECK(empty_plan.has_value());
    CHECK(empty_plan && !empty_plan->ready());
    CHECK(empty_plan &&
          has_issue(empty_plan->sources.front(),
                    trackknife::metadata::MetadataWritePlanIssueKind::unsupported_field_mapping));

    auto invalid_key_selection = selection_for(*read);
    CHECK(invalid_key_selection.has_value());
    if (!invalid_key_selection) {
        return;
    }
    const auto invalid_key = invalid_key_selection->ensure_missing_field("BAD~FIELD", "BAD~FIELD");
    CHECK(invalid_key.has_value());
    if (!invalid_key) {
        return;
    }
    trackknife::metadata::StagedMetadataPatchSet invalid_key_patches;
    CHECK(invalid_key_patches.replace_values(*invalid_key_selection, 0U, *invalid_key, {"value"})
              .has_value());
    auto invalid_key_plan = make_plan(*invalid_key_selection, invalid_key_patches);
    CHECK(invalid_key_plan && !invalid_key_plan->ready());
    CHECK(invalid_key_plan &&
          has_issue(invalid_key_plan->sources.front(),
                    trackknife::metadata::MetadataWritePlanIssueKind::unsupported_field_mapping));

    auto invalid_utf8_selection = selection_for(*read);
    CHECK(invalid_utf8_selection.has_value());
    const auto invalid_utf8_title =
        invalid_utf8_selection ? invalid_utf8_selection->field_index("TITLE") : std::nullopt;
    CHECK(invalid_utf8_title.has_value());
    if (!invalid_utf8_selection || !invalid_utf8_title) {
        return;
    }
    trackknife::metadata::StagedMetadataPatchSet invalid_utf8_patches;
    CHECK(invalid_utf8_patches
              .replace_values(*invalid_utf8_selection, 0U, *invalid_utf8_title,
                              {std::string{"\xC3\x28", 2U}})
              .has_value());
    auto invalid_utf8_plan = make_plan(*invalid_utf8_selection, invalid_utf8_patches);
    CHECK(invalid_utf8_plan && !invalid_utf8_plan->ready());
    CHECK(invalid_utf8_plan &&
          has_issue(invalid_utf8_plan->sources.front(),
                    trackknife::metadata::MetadataWritePlanIssueKind::unsupported_field_mapping));

    auto selection = selection_for(*read);
    CHECK(selection.has_value());
    if (!selection) {
        return;
    }
    const auto title = selection->field_index("TITLE");
    CHECK(title.has_value());
    if (!title) {
        return;
    }
    trackknife::metadata::StagedMetadataPatchSet patches;
    CHECK(patches.replace_values(*selection, 0U, *title, {"Changed"}).has_value());
    auto plan = make_plan(*selection, patches);
    CHECK(plan && plan->ready());
    if (!plan || !plan->ready()) {
        return;
    }

    trackknife::core::CancellationSource cancellation;
    cancellation.request_cancellation();
    const auto cancelled_path = directory.path() / "cancelled.flac";
    const auto cancelled = trackknife::metadata::prepare_flac_metadata_write_copy(
        plan->sources.front(), cancelled_path.native(), cancellation.token());
    CHECK(!cancelled.has_value());
    CHECK(!cancelled && cancelled.error().code == trackknife::core::ErrorCode::cancelled);
    CHECK(!std::filesystem::exists(cancelled_path));

    auto mismatched_plan = plan->sources.front();
    mismatched_plan.changes.front().original_values = {"Not the fresh original"};
    const auto mismatch_path = directory.path() / "mismatch.flac";
    const auto mismatch = trackknife::metadata::prepare_flac_metadata_write_copy(
        mismatched_plan, mismatch_path.native());
    CHECK(!mismatch.has_value());
    CHECK(!mismatch && mismatch.error().code == trackknife::core::ErrorCode::conflict);
    CHECK(!std::filesystem::exists(mismatch_path));

    const auto old_time = std::filesystem::last_write_time(source);
    std::filesystem::last_write_time(source, old_time + std::chrono::seconds{1});
    const auto stale_path = directory.path() / "stale.flac";
    const auto stale = trackknife::metadata::prepare_flac_metadata_write_copy(plan->sources.front(),
                                                                              stale_path.native());
    CHECK(!stale.has_value());
    CHECK(!stale && stale.error().code == trackknife::core::ErrorCode::conflict);
    CHECK(!std::filesystem::exists(stale_path));
}

} // namespace

int main(const int argc, char** argv) {
    CHECK(argc == 2);
    if (argc == 2) {
        const std::filesystem::path fixture_directory{argv[1]};
        preservesNativeFlacWhileApplyingExactTextChanges(fixture_directory);
        preservesEmbeddedArtworkAndDecodedAudio(fixture_directory);
        blocksUnrepresentablePlansAndStaleOrCancelledWrites(fixture_directory);
    }
    return failures == 0 ? 0 : 1;
}
