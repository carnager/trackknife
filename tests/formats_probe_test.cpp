// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/formats/probe.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <ranges>
#include <span>
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

void append_u16(std::array<unsigned char, 44>& header, const std::size_t offset,
                const std::uint16_t value) {
    header.at(offset) = static_cast<unsigned char>(value & 0xFFU);
    header.at(offset + 1U) = static_cast<unsigned char>((value >> 8U) & 0xFFU);
}

void append_u32(std::array<unsigned char, 44>& header, const std::size_t offset,
                const std::uint32_t value) {
    for (std::size_t byte = 0U; byte < 4U; ++byte) {
        header.at(offset + byte) =
            static_cast<unsigned char>((value >> static_cast<unsigned>(byte * 8U)) & 0xFFU);
    }
}

void write_wave(const std::filesystem::path& path, const std::span<const std::int16_t> samples) {
    constexpr std::uint32_t sample_rate = 8'000U;
    constexpr std::uint16_t channels = 1U;
    constexpr std::uint16_t bits_per_sample = 16U;
    const auto data_bytes =
        static_cast<std::uint32_t>(samples.size()) * channels * (bits_per_sample / 8U);
    std::array<unsigned char, 44> header{};
    constexpr std::array riff{'R', 'I', 'F', 'F'};
    constexpr std::array wave{'W', 'A', 'V', 'E'};
    constexpr std::array format{'f', 'm', 't', ' '};
    constexpr std::array data{'d', 'a', 't', 'a'};
    std::ranges::copy(riff, header.begin());
    append_u32(header, 4U, 36U + data_bytes);
    std::ranges::copy(wave, header.begin() + 8);
    std::ranges::copy(format, header.begin() + 12);
    append_u32(header, 16U, 16U);
    append_u16(header, 20U, 1U);
    append_u16(header, 22U, channels);
    append_u32(header, 24U, sample_rate);
    append_u32(header, 28U, sample_rate * channels * (bits_per_sample / 8U));
    append_u16(header, 32U, channels * (bits_per_sample / 8U));
    append_u16(header, 34U, bits_per_sample);
    std::ranges::copy(data, header.begin() + 36);
    append_u32(header, 40U, data_bytes);

    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(header.data()),
                 static_cast<std::streamsize>(header.size()));
    for (const auto sample : samples) {
        const auto bits = static_cast<std::uint16_t>(sample);
        const std::array bytes{static_cast<char>(bits & 0xFFU),
                               static_cast<char>((bits >> 8U) & 0xFFU)};
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
}

[[nodiscard]] std::int16_t pattern_sample(const std::int64_t sample) {
    return static_cast<std::int16_t>(((sample * 37) % 20'001) - 10'000);
}

[[nodiscard]] float expected_float_sample(const std::int64_t sample) {
    return static_cast<float>(pattern_sample(sample)) / 32'768.0F;
}

[[nodiscard]] std::optional<std::vector<unsigned char>>
decode_base64_file(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return std::nullopt;
    }
    const std::string encoded{std::istreambuf_iterator<char>{input},
                              std::istreambuf_iterator<char>{}};
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> decoded;
    decoded.reserve((encoded.size() * 3U) / 4U);
    unsigned accumulator = 0U;
    int bits = -8;
    for (const auto character : encoded) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isspace(byte) != 0) {
            continue;
        }
        if (character == '=') {
            break;
        }
        const auto value = alphabet.find(character);
        if (value == std::string_view::npos) {
            return std::nullopt;
        }
        accumulator = (accumulator << 6U) | static_cast<unsigned>(value);
        bits += 6;
        if (bits >= 0) {
            decoded.push_back(static_cast<unsigned char>((accumulator >> bits) & 0xFFU));
            bits -= 8;
        }
    }
    return decoded;
}

[[nodiscard]] std::vector<float> decode_all(trackknife::formats::AudioDecoder& decoder) {
    std::vector<float> samples;
    std::int64_t expected_start = decoder.sample_range().start_sample;
    while (true) {
        auto chunk = decoder.next_chunk();
        CHECK(chunk.has_value());
        if (!chunk || !*chunk) {
            break;
        }
        if ((*chunk)->start_sample != expected_start) {
            std::cerr << "non-contiguous chunk: expected " << expected_start << ", got "
                      << (*chunk)->start_sample << ", frames "
                      << (*chunk)->frame_count(decoder.output_format().channels) << '\n';
        }
        CHECK((*chunk)->start_sample == expected_start);
        expected_start +=
            static_cast<std::int64_t>((*chunk)->frame_count(decoder.output_format().channels));
        samples.insert(samples.end(), (*chunk)->interleaved_samples.begin(),
                       (*chunk)->interleaved_samples.end());
    }
    return samples;
}

void probesRealWaveAndPreservesRawPath() {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("trackknife-format-probe-" + trackknife::core::StableId::random().to_string());
    std::error_code error;
    CHECK(std::filesystem::create_directories(root, error));
    const std::string invalid_name{"probe-\xFF.wav", 11U};
    const auto path = root / std::filesystem::path{invalid_name};
    const std::array<std::int16_t, 800> silence{};
    write_wave(path, silence);

    const auto probe = trackknife::formats::probe_local_media(path.native());
    CHECK(probe.has_value());
    CHECK(probe && probe->raw_path == path.native());
    CHECK(probe && probe->container_names.find("wav") != std::string::npos);
    CHECK(probe && probe->duration_ms && *probe->duration_ms == 100);
    CHECK(probe && probe->audio_streams.size() == 1U);
    CHECK(probe && probe->best_audio_stream == 0);
    CHECK(probe && probe->audio_streams.front().codec_name == "pcm_s16le");
    CHECK(probe && probe->audio_streams.front().sample_rate == 8'000);
    CHECK(probe && probe->audio_streams.front().channels == 1);

    auto decoder = trackknife::formats::AudioDecoder::open(path.native());
    CHECK(decoder.has_value());
    CHECK(decoder && decoder->output_format().sample_rate == 8'000);
    CHECK(decoder && decoder->output_format().channels == 1);
    CHECK(decoder && decoder->duration_samples() == 800);
    std::size_t decoded_frames = 0U;
    bool decoded_only_silence = true;
    while (decoder) {
        auto chunk = decoder->next_chunk();
        CHECK(chunk.has_value());
        if (!chunk || !*chunk) {
            break;
        }
        decoded_frames += (*chunk)->frame_count(decoder->output_format().channels);
        decoded_only_silence =
            decoded_only_silence &&
            std::ranges::all_of((*chunk)->interleaved_samples,
                                [](const float sample) { return sample == 0.0F; });
    }
    CHECK(decoded_frames == 800U);
    CHECK(decoded_only_silence);

    trackknife::core::CancellationSource decode_cancellation;
    auto cancelled_decoder =
        trackknife::formats::AudioDecoder::open(path.native(), decode_cancellation.token());
    CHECK(cancelled_decoder.has_value());
    if (cancelled_decoder) {
        decode_cancellation.request_cancellation();
        auto cancelled_chunk = cancelled_decoder->next_chunk();
        CHECK(!cancelled_chunk);
        CHECK(cancelled_chunk.error().code == trackknife::core::ErrorCode::cancelled);
    }

    trackknife::core::CancellationSource cancellation;
    cancellation.request_cancellation();
    const auto cancelled =
        trackknife::formats::probe_local_media(path.native(), cancellation.token());
    CHECK(!cancelled);
    CHECK(cancelled.error().code == trackknife::core::ErrorCode::cancelled);
    std::filesystem::remove_all(root, error);
    CHECK(!error);
}

void seeksAndTrimsAdjacentSampleRanges() {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("trackknife-format-seek-" + trackknife::core::StableId::random().to_string());
    std::error_code error;
    CHECK(std::filesystem::create_directories(root, error));
    const auto path = root / "pattern.wav";
    std::vector<std::int16_t> samples(4'096U);
    for (std::size_t index = 0U; index < samples.size(); ++index) {
        samples[index] = pattern_sample(static_cast<std::int64_t>(index));
    }
    write_wave(path, samples);

    auto decoder = trackknife::formats::AudioDecoder::open(path.native());
    CHECK(decoder.has_value());
    if (decoder) {
        const auto seek = decoder->seek_to_sample(1'234);
        CHECK(seek.has_value());
        auto chunk = decoder->next_chunk();
        CHECK(chunk.has_value());
        CHECK(chunk && *chunk);
        CHECK(chunk && *chunk && (*chunk)->start_sample == 1'234);
        CHECK(chunk && *chunk && !(*chunk)->interleaved_samples.empty());
        CHECK(chunk && *chunk &&
              (*chunk)->interleaved_samples.front() == expected_float_sample(1'234));

        const auto seek_to_end = decoder->seek_to_sample(4'096);
        CHECK(seek_to_end.has_value());
        auto end = decoder->next_chunk();
        CHECK(end.has_value());
        CHECK(end && !*end);
        const auto seek_back = decoder->seek_to_sample(321);
        CHECK(seek_back.has_value());
        auto earlier = decoder->next_chunk();
        CHECK(earlier.has_value());
        CHECK(earlier && *earlier && (*earlier)->start_sample == 321);
        CHECK(earlier && *earlier &&
              (*earlier)->interleaved_samples.front() == expected_float_sample(321));
    }

    const auto decode_range = [&path](const trackknife::formats::SampleRange range) {
        std::vector<float> decoded;
        auto ranged = trackknife::formats::AudioDecoder::open_segment(path.native(), range);
        CHECK(ranged.has_value());
        if (!ranged) {
            return decoded;
        }
        CHECK(ranged->sample_range() == range);
        auto expected_position = range.start_sample;
        while (true) {
            auto chunk = ranged->next_chunk();
            CHECK(chunk.has_value());
            if (!chunk || !*chunk) {
                break;
            }
            CHECK((*chunk)->start_sample == expected_position);
            expected_position +=
                static_cast<std::int64_t>((*chunk)->frame_count(ranged->output_format().channels));
            decoded.insert(decoded.end(), (*chunk)->interleaved_samples.begin(),
                           (*chunk)->interleaved_samples.end());
        }
        CHECK(!range.end_sample || expected_position == *range.end_sample);
        return decoded;
    };

    const auto first = decode_range({.start_sample = 0, .end_sample = 2'048});
    const auto second = decode_range({.start_sample = 2'048, .end_sample = 4'096});
    CHECK(first.size() == 2'048U);
    CHECK(second.size() == 2'048U);
    std::vector<float> joined = first;
    joined.insert(joined.end(), second.begin(), second.end());
    CHECK(joined.size() == samples.size());
    for (std::size_t index = 0U; index < joined.size(); ++index) {
        CHECK(joined[index] == expected_float_sample(static_cast<std::int64_t>(index)));
    }

    const auto invalid = trackknife::formats::AudioDecoder::open_segment(
        path.native(), {.start_sample = 200, .end_sample = 100});
    CHECK(!invalid);
    CHECK(invalid.error().code == trackknife::core::ErrorCode::invalid_argument);
    auto bounded = trackknife::formats::AudioDecoder::open_segment(
        path.native(), {.start_sample = 100, .end_sample = 200});
    CHECK(bounded.has_value());
    if (bounded) {
        const auto outside = bounded->seek_to_sample(99);
        CHECK(!outside);
        CHECK(outside.error().code == trackknife::core::ErrorCode::invalid_argument);
    }

    std::filesystem::remove_all(root, error);
    CHECK(!error);
}

void decodesCompressedGaplessFixtures(const std::filesystem::path& fixture_directory) {
    struct Fixture {
        std::string_view encoded_name;
        std::string_view materialized_name;
        std::string_view codec_name;
    };
    constexpr std::array fixtures{
        Fixture{"gapless-tone-aac-m4a.b64", "gapless-tone.m4a", "aac"},
        Fixture{"gapless-tone-mp3.b64", "gapless-tone.mp3", "mp3"},
        Fixture{"gapless-tone-opus.b64", "gapless-tone.opus", "opus"},
    };
    const auto root =
        std::filesystem::temp_directory_path() /
        ("trackknife-gapless-fixtures-" + trackknife::core::StableId::random().to_string());
    std::error_code error;
    CHECK(std::filesystem::create_directories(root, error));

    for (const auto& fixture : fixtures) {
        const auto binary = decode_base64_file(fixture_directory / fixture.encoded_name);
        CHECK(binary.has_value());
        if (!binary) {
            continue;
        }
        const auto path = root / fixture.materialized_name;
        std::ofstream output{path, std::ios::binary};
        output.write(reinterpret_cast<const char*>(binary->data()),
                     static_cast<std::streamsize>(binary->size()));
        output.close();
        CHECK(output.good());

        const auto probe = trackknife::formats::probe_local_media(path.native());
        CHECK(probe.has_value());
        CHECK(probe && probe->audio_streams.size() == 1U);
        CHECK(probe && probe->audio_streams.front().codec_name == fixture.codec_name);
        CHECK(probe && probe->audio_streams.front().sample_rate == 48'000);
        CHECK(probe && probe->audio_streams.front().channels == 1);

        auto decoder = trackknife::formats::AudioDecoder::open(path.native());
        CHECK(decoder.has_value());
        if (!decoder) {
            continue;
        }
        auto decoded = decode_all(*decoder);
        if (decoded.size() != 4'800U) {
            std::cerr << fixture.materialized_name << " decoded " << decoded.size() << " frames\n";
        }
        CHECK(decoded.size() == 4'800U);
        if (decoded.size() == 4'800U) {
            const auto head_energy = std::ranges::fold_left(
                decoded | std::views::take(128U), 0.0F,
                [](const float sum, const float sample) { return sum + std::abs(sample); });
            const auto tail_energy = std::ranges::fold_left(
                decoded | std::views::drop(decoded.size() - 128U), 0.0F,
                [](const float sum, const float sample) { return sum + std::abs(sample); });
            CHECK(head_energy > 1.0F);
            CHECK(tail_energy > 1.0F);
        }

        auto segment = trackknife::formats::AudioDecoder::open_segment(
            path.native(), {.start_sample = 1'200, .end_sample = 3'600});
        CHECK(segment.has_value());
        if (segment) {
            const auto segment_samples = decode_all(*segment);
            if (segment_samples.size() != 2'400U) {
                std::cerr << fixture.materialized_name << " segment decoded "
                          << segment_samples.size() << " frames\n";
            }
            CHECK(segment_samples.size() == 2'400U);
            if (decoded.size() == 4'800U && segment_samples.size() == 2'400U) {
                const auto matches_full_decode = std::ranges::equal(
                    segment_samples, decoded | std::views::drop(1'200U) | std::views::take(2'400U));
                if (!matches_full_decode) {
                    std::cerr << fixture.materialized_name
                              << " seek output differs from its full-decode slice\n";
                }
                CHECK(matches_full_decode);
            }
        }
    }

    std::filesystem::remove_all(root, error);
    CHECK(!error);
}

void normalizesVorbisFrameTimeline(const std::filesystem::path& fixture_directory) {
    const auto binary = decode_base64_file(fixture_directory / "vorbis-positive-start.b64");
    CHECK(binary.has_value());
    if (!binary) {
        return;
    }
    const auto path =
        std::filesystem::temp_directory_path() /
        ("trackknife-vorbis-timeline-" + trackknife::core::StableId::random().to_string() + ".ogg");
    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(binary->data()),
                 static_cast<std::streamsize>(binary->size()));
    output.close();
    CHECK(output.good());

    auto decoder = trackknife::formats::AudioDecoder::open(path.native());
    CHECK(decoder.has_value());
    if (decoder) {
        CHECK(decoder->output_format().sample_rate == 44'100);
        CHECK(decoder->output_format().channels == 1);
        const auto decoded = decode_all(*decoder);
        CHECK(decoded.size() == 4'282U);

        auto segment = trackknife::formats::AudioDecoder::open_segment(
            path.native(), {.start_sample = 1'000, .end_sample = 3'000});
        CHECK(segment.has_value());
        if (segment) {
            const auto segment_samples = decode_all(*segment);
            CHECK(segment_samples.size() == 2'000U);
            CHECK(std::ranges::equal(segment_samples, decoded | std::views::drop(1'000U) |
                                                          std::views::take(2'000U)));
        }
    }

    std::error_code error;
    std::filesystem::remove(path, error);
    CHECK(!error);
}

void probesTaggedFlacFixture(const std::filesystem::path& fixture_directory) {
    const auto binary = decode_base64_file(fixture_directory / "tagged-tone-flac.b64");
    CHECK(binary.has_value());
    if (!binary) {
        return;
    }
    const auto path =
        std::filesystem::temp_directory_path() /
        ("trackknife-tagged-flac-" + trackknife::core::StableId::random().to_string() + ".flac");
    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(binary->data()),
                 static_cast<std::streamsize>(binary->size()));
    output.close();
    CHECK(output.good());

    const auto probe = trackknife::formats::probe_local_media(path.native());
    CHECK(probe.has_value());
    if (probe) {
        CHECK(probe->audio_streams.size() == 1U);
        CHECK(probe->audio_streams.front().codec_name == "flac");
        CHECK(probe->audio_streams.front().sample_rate == 44'100);
        CHECK(probe->audio_streams.front().channels == 1);
        const auto tag_value = [&probe](const std::string_view name) -> std::string {
            for (const auto& tag : probe->tags) {
                if (tag.name == name) {
                    return tag.value;
                }
            }
            return {};
        };
        CHECK(probe->tags.size() == 5U);
        CHECK(tag_value("title") == "Fixture Tone");
        CHECK(tag_value("artist") == "Trackknife Project");
        CHECK(tag_value("album") == "Trackbench Fixtures");
        CHECK(tag_value("date") == "2026");
        CHECK(tag_value("track") == "3");
    }

    auto decoder = trackknife::formats::AudioDecoder::open(path.native());
    CHECK(decoder.has_value());
    if (decoder) {
        CHECK(decoder->output_format().sample_rate == 44'100);
        CHECK(decoder->output_format().channels == 1);
        const auto decoded = decode_all(*decoder);
        CHECK(decoded.size() == 4'410U);
        const auto loud = [](const float sample) { return std::fabs(sample) > 0.01F; };
        CHECK(std::ranges::any_of(decoded | std::views::take(64U), loud));
        CHECK(std::ranges::any_of(decoded | std::views::drop(decoded.size() - 64U), loud));
    }

    std::error_code error;
    std::filesystem::remove(path, error);
    CHECK(!error);
}

void probesTaggedWavPackFixture(const std::filesystem::path& fixture_directory) {
    const auto binary = decode_base64_file(fixture_directory / "tagged-tone-wavpack.b64");
    CHECK(binary.has_value());
    if (!binary) {
        return;
    }
    const auto path =
        std::filesystem::temp_directory_path() /
        ("trackknife-tagged-wavpack-" + trackknife::core::StableId::random().to_string() + ".wv");
    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(binary->data()),
                 static_cast<std::streamsize>(binary->size()));
    output.close();
    CHECK(output.good());

    const auto probe = trackknife::formats::probe_local_media(path.native());
    CHECK(probe.has_value());
    if (probe) {
        CHECK(probe->audio_streams.size() == 1U);
        CHECK(probe->audio_streams.front().codec_name == "wavpack");
        CHECK(probe->audio_streams.front().sample_rate == 44'100);
        CHECK(probe->audio_streams.front().channels == 1);
        const auto tag_value = [&probe](const std::string_view name) -> std::string {
            for (const auto& tag : probe->tags) {
                if (tag.name == name) {
                    return tag.value;
                }
            }
            return {};
        };
        CHECK(probe->tags.size() == 2U);
        CHECK(tag_value("title") == "Fixture Tone");
        CHECK(tag_value("artist") == "Trackknife Project");
    }

    auto decoder = trackknife::formats::AudioDecoder::open(path.native());
    CHECK(decoder.has_value());
    if (decoder) {
        CHECK(decoder->output_format().sample_rate == 44'100);
        CHECK(decoder->output_format().channels == 1);
        const auto decoded = decode_all(*decoder);
        CHECK(decoded.size() == 4'410U);
        const auto loud = [](const float sample) { return std::fabs(sample) > 0.01F; };
        CHECK(std::ranges::any_of(decoded | std::views::take(64U), loud));
        CHECK(std::ranges::any_of(decoded | std::views::drop(decoded.size() - 64U), loud));
    }

    std::error_code error;
    std::filesystem::remove(path, error);
    CHECK(!error);
}

} // namespace

int main(const int argc, char** argv) {
    CHECK(argc == 2);
    probesRealWaveAndPreservesRawPath();
    seeksAndTrimsAdjacentSampleRanges();
    if (argc == 2) {
        decodesCompressedGaplessFixtures(argv[1]);
        normalizesVorbisFrameTimeline(argv[1]);
        probesTaggedFlacFixture(argv[1]);
        probesTaggedWavPackFixture(argv[1]);
    }
    return failures == 0 ? 0 : 1;
}
