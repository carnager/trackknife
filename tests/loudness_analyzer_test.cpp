// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/core/stable_id.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/loudness/analyzer.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
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

constexpr int sample_rate = 44'100;
constexpr double tone_hertz = 997.0;

[[nodiscard]] std::vector<float> sine_frames(const double amplitude, const int channels,
                                             const double seconds) {
    const auto frames = static_cast<std::size_t>(seconds * sample_rate);
    std::vector<float> samples;
    samples.reserve(frames * static_cast<std::size_t>(channels));
    for (std::size_t frame = 0U; frame < frames; ++frame) {
        const auto value =
            static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * tone_hertz *
                                                    static_cast<double>(frame) / sample_rate));
        for (int channel = 0; channel < channels; ++channel) {
            samples.push_back(value);
        }
    }
    return samples;
}

[[nodiscard]] std::optional<trackknife::loudness::TrackLoudness>
analyze_sine(const double amplitude, const int channels, const bool true_peak,
             trackknife::loudness::LoudnessAnalyzer* keep_alive = nullptr) {
    auto analyzer =
        trackknife::loudness::LoudnessAnalyzer::create(sample_rate, channels, true_peak);
    if (!analyzer) {
        std::cerr << analyzer.error().message << '\n';
        return std::nullopt;
    }
    const auto samples = sine_frames(amplitude, channels, 5.0);
    if (!analyzer->add_frames(samples)) {
        return std::nullopt;
    }
    auto result = analyzer->finish();
    if (!result) {
        std::cerr << result.error().message << '\n';
        return std::nullopt;
    }
    if (keep_alive != nullptr) {
        *keep_alive = std::move(*analyzer);
    }
    return *result;
}

// ITU-R BS.1770 conformance points: a 997 Hz full-scale sine reads
// -3.01 LKFS in a single channel and ~0.00 LKFS when both stereo channels
// carry it; halving the amplitude is exactly -6.02 dB.
void sineLoudnessMatchesTheAnalyticReference() {
    const auto full = analyze_sine(1.0, 2, false);
    CHECK(full.has_value());
    if (!full) {
        return;
    }
    CHECK(std::abs(full->integrated_lufs - 0.0) < 0.1);
    CHECK(std::abs(full->sample_peak - 1.0) < 0.002);
    CHECK(!full->true_peak.has_value());
    CHECK(full->measurable());
    // ReplayGain 2.0: gain aims the track at -18 LUFS.
    CHECK(std::abs(full->track_gain_db() - (-18.0 - full->integrated_lufs)) < 1e-9);

    const auto half = analyze_sine(0.5, 2, false);
    CHECK(half.has_value());
    if (!half) {
        return;
    }
    // Halving the amplitude is exactly -6.02 dB of loudness.
    CHECK(std::abs((full->integrated_lufs - half->integrated_lufs) - 6.0206) < 0.05);
    CHECK(std::abs(half->sample_peak - 0.5) < 0.002);

    const auto mono = analyze_sine(1.0, 1, false);
    CHECK(mono.has_value());
    if (!mono) {
        return;
    }
    CHECK(std::abs(mono->integrated_lufs - (-3.01)) < 0.1);
}

void truePeakIsAtLeastTheSamplePeak() {
    const auto measured = analyze_sine(0.9, 2, true);
    CHECK(measured.has_value());
    if (!measured) {
        return;
    }
    CHECK(measured->true_peak.has_value());
    CHECK(measured->true_peak && *measured->true_peak >= measured->sample_peak);
    CHECK(measured->sample_peak > 0.89);
}

// Album loudness is the gated loudness of the whole programme: with a loud
// and a very quiet track the relative gate keeps the album near the loud
// track instead of averaging the two gains.
void albumReductionIsProgrammeLoudnessNotAnAverage() {
    trackknife::loudness::LoudnessAnalyzer loud{
        *trackknife::loudness::LoudnessAnalyzer::create(sample_rate, 2, false)};
    trackknife::loudness::LoudnessAnalyzer quiet{
        *trackknife::loudness::LoudnessAnalyzer::create(sample_rate, 2, false)};
    const auto loud_result = analyze_sine(1.0, 2, false, &loud);
    const auto quiet_result = analyze_sine(0.05, 2, false, &quiet);
    CHECK(loud_result.has_value() && quiet_result.has_value());
    if (!loud_result || !quiet_result) {
        return;
    }
    const std::array<const trackknife::loudness::LoudnessAnalyzer*, 2> analyzers{&loud, &quiet};
    const auto album = trackknife::loudness::album_integrated_lufs(analyzers);
    CHECK(album.has_value());
    if (!album) {
        std::cerr << album.error().message << '\n';
        return;
    }
    CHECK(*album <= loud_result->integrated_lufs + 0.01);
    CHECK(*album > quiet_result->integrated_lufs + 10.0);
    CHECK(std::abs(*album - loud_result->integrated_lufs) < 3.5);
}

void rejectsInvalidInput() {
    CHECK(!trackknife::loudness::LoudnessAnalyzer::create(0, 2, false).has_value());
    CHECK(!trackknife::loudness::LoudnessAnalyzer::create(sample_rate, 0, false).has_value());
    auto analyzer = trackknife::loudness::LoudnessAnalyzer::create(sample_rate, 2, false);
    CHECK(analyzer.has_value());
    if (analyzer) {
        const std::array<float, 3> uneven{0.0F, 0.0F, 0.0F};
        CHECK(!analyzer->add_frames(uneven).has_value());
    }
    CHECK(!trackknife::loudness::album_integrated_lufs({}).has_value());
}

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

// The real decode integration: the 100 ms fixture tone analyzes cleanly,
// reports its peak, and is honestly unmeasurable for gated loudness — a
// sub-400 ms programme has no integrated value to derive gains from.
void decodedFixtureAnalyzesAndShortMaterialIsUnmeasurable(
    const std::filesystem::path& fixture_directory) {
    const auto decoded = decode_base64_file(fixture_directory / "tagged-tone-wavpack.b64");
    CHECK(decoded.has_value());
    if (!decoded) {
        return;
    }
    const auto path =
        std::filesystem::temp_directory_path() /
        ("trackknife-loudness-" + trackknife::core::StableId::random().to_string() + ".wv");
    {
        std::ofstream output{path, std::ios::binary};
        output.write(reinterpret_cast<const char*>(decoded->data()),
                     static_cast<std::streamsize>(decoded->size()));
    }
    auto decoder = trackknife::formats::AudioDecoder::open(path.native());
    CHECK(decoder.has_value());
    if (decoder) {
        const auto analyzed = trackknife::loudness::analyze_decoded_source(*decoder, true);
        CHECK(analyzed.has_value());
        if (analyzed) {
            CHECK(analyzed->sample_peak > 0.01);
            CHECK(analyzed->true_peak.has_value());
            CHECK(!analyzed->measurable());
        }
    }
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);
}

} // namespace

int main(const int argc, char** argv) {
    CHECK(argc == 2);
    sineLoudnessMatchesTheAnalyticReference();
    truePeakIsAtLeastTheSamplePeak();
    albumReductionIsProgrammeLoudnessNotAnAverage();
    rejectsInvalidInput();
    if (argc == 2) {
        decodedFixtureAnalyzesAndShortMaterialIsUnmeasurable(std::filesystem::path{argv[1]});
    }
    return failures == 0 ? 0 : 1;
}
