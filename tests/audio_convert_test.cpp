// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/convert/convert.hpp"
#include "trackknife/convert/preset.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/stable_id.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
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
constexpr double tone_seconds = 2.0;

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() /
                ("trackknife-convert-" + trackknife::core::StableId::random().to_string());
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

void write_sine_wav(const std::filesystem::path& path, const double amplitude,
                    const double seconds) {
    const auto frames = static_cast<std::uint32_t>(seconds * sample_rate);
    const std::uint32_t data_bytes = frames * 2U * 2U;
    std::ofstream output{path, std::ios::binary};
    const auto write_u32 = [&output](const std::uint32_t value) {
        const std::array<char, 4> bytes{
            static_cast<char>(value & 0xFFU), static_cast<char>((value >> 8U) & 0xFFU),
            static_cast<char>((value >> 16U) & 0xFFU), static_cast<char>((value >> 24U) & 0xFFU)};
        output.write(bytes.data(), 4);
    };
    const auto write_u16 = [&output](const std::uint16_t value) {
        const std::array<char, 2> bytes{static_cast<char>(value & 0xFFU),
                                        static_cast<char>((value >> 8U) & 0xFFU)};
        output.write(bytes.data(), 2);
    };
    output.write("RIFF", 4);
    write_u32(36U + data_bytes);
    output.write("WAVE", 4);
    output.write("fmt ", 4);
    write_u32(16U);
    write_u16(1U);
    write_u16(2U);
    write_u32(static_cast<std::uint32_t>(sample_rate));
    write_u32(static_cast<std::uint32_t>(sample_rate) * 4U);
    write_u16(4U);
    write_u16(16U);
    output.write("data", 4);
    write_u32(data_bytes);
    for (std::uint32_t frame = 0U; frame < frames; ++frame) {
        const auto value = amplitude * std::sin(2.0 * std::numbers::pi * tone_hertz *
                                                static_cast<double>(frame) / sample_rate);
        const auto sample = static_cast<std::int16_t>(std::clamp(value, -1.0, 1.0) * 32'767.0);
        write_u16(static_cast<std::uint16_t>(sample));
        write_u16(static_cast<std::uint16_t>(sample));
    }
}

// Anything besides the named survivors — hidden temporaries above all —
// counts as leftover.
[[nodiscard]] std::size_t entries_besides(const std::filesystem::path& directory,
                                          const std::vector<std::string>& survivors) {
    std::size_t extras = 0U;
    for (const auto& entry : std::filesystem::directory_iterator{directory}) {
        const auto name = entry.path().filename().string();
        if (std::ranges::find(survivors, name) == survivors.end()) {
            std::cerr << "unexpected entry: " << name << '\n';
            ++extras;
        }
    }
    return extras;
}

void builtinPresetsProbeAvailable() {
    const auto& presets = trackknife::convert::builtin_encoder_presets();
    CHECK(presets.size() == 4U);
    for (const auto& preset : presets) {
        const auto availability = trackknife::convert::probe_encoder_preset(preset);
        if (!availability.available) {
            std::cerr << preset.id << ": " << availability.detail << '\n';
        }
        CHECK(availability.available);
    }
    const auto found = trackknife::convert::find_encoder_preset("opus-192");
    CHECK(found.has_value() && found->codec_name == "libopus");
    CHECK(!trackknife::convert::find_encoder_preset("wax-cylinder").has_value());
    trackknife::convert::EncoderPreset unbuildable;
    unbuildable.id = "bogus";
    unbuildable.codec_name = "no-such-encoder";
    unbuildable.container_name = "flac";
    const auto bogus = trackknife::convert::probe_encoder_preset(unbuildable);
    CHECK(!bogus.available && !bogus.detail.empty());
}

void convertsToEveryPresetAtomically() {
    TemporaryDirectory directory;
    const auto source = directory.path() / "tone.wav";
    write_sine_wav(source, 0.6, tone_seconds);
    std::vector<std::string> survivors{"tone.wav"};

    for (const auto& preset : trackknife::convert::builtin_encoder_presets()) {
        const auto destination =
            directory.path() / ("converted-" + preset.id + "." + preset.file_extension);
        std::uint64_t last_frames = 0U;
        bool total_present = false;
        const auto converted = trackknife::convert::convert_audio_file(
            {.source_raw_path = source.native(),
             .source_selection = {},
             .source_range = {},
             .destination_raw_path = destination.native(),
             .preset = preset},
            [&last_frames, &total_present](const std::uint64_t frames_done,
                                           const std::optional<std::uint64_t> frames_total) {
                CHECK(frames_done >= last_frames);
                last_frames = frames_done;
                total_present = total_present || frames_total.has_value();
            });
        if (!converted) {
            std::cerr << preset.id << ": " << converted.error().message << '\n';
        }
        CHECK(converted.has_value());
        if (!converted) {
            continue;
        }
        survivors.push_back(destination.filename().string());
        CHECK(std::filesystem::exists(destination));
        CHECK(converted->channels == 2);
        // Opus only speaks 48 kHz-family rates, so 44.1 kHz material is
        // resampled up; every other preset keeps the source rate.
        const auto expected_rate = preset.id == "opus-192" ? 48'000 : sample_rate;
        CHECK(converted->sample_rate == expected_rate);
        const auto expected_frames = static_cast<std::int64_t>(tone_seconds * expected_rate);
        CHECK(std::abs(converted->duration_samples - expected_frames) <= expected_rate / 5);
        CHECK(last_frames == static_cast<std::uint64_t>(tone_seconds * sample_rate));
        CHECK(total_present);
    }
    CHECK(entries_besides(directory.path(), survivors) == 0U);
}

void refusesExistingDestinationAndMissingDirectory() {
    TemporaryDirectory directory;
    const auto source = directory.path() / "tone.wav";
    write_sine_wav(source, 0.6, 0.5);
    const auto preset = *trackknife::convert::find_encoder_preset("flac");

    const auto occupied = directory.path() / "occupied.flac";
    std::ofstream{occupied} << "already here";
    const auto conflicting =
        trackknife::convert::convert_audio_file({.source_raw_path = source.native(),
                                                 .source_selection = {},
                                                 .source_range = {},
                                                 .destination_raw_path = occupied.native(),
                                                 .preset = preset});
    CHECK(!conflicting.has_value());
    CHECK(!conflicting && conflicting.error().code == trackknife::core::ErrorCode::conflict);
    {
        std::ifstream input{occupied};
        std::string content;
        std::getline(input, content);
        CHECK(content == "already here");
    }

    const auto orphan = directory.path() / "missing" / "out.flac";
    const auto orphaned =
        trackknife::convert::convert_audio_file({.source_raw_path = source.native(),
                                                 .source_selection = {},
                                                 .source_range = {},
                                                 .destination_raw_path = orphan.native(),
                                                 .preset = preset});
    CHECK(!orphaned.has_value());
    CHECK(!orphaned && orphaned.error().code == trackknife::core::ErrorCode::invalid_argument);

    CHECK(entries_besides(directory.path(), {"tone.wav", "occupied.flac"}) == 0U);
}

void cancellationLeavesNoPartialOutput() {
    TemporaryDirectory directory;
    const auto source = directory.path() / "tone.wav";
    write_sine_wav(source, 0.6, 1.0);
    trackknife::core::CancellationSource cancellation;
    cancellation.request_cancellation();
    const auto cancelled = trackknife::convert::convert_audio_file(
        {.source_raw_path = source.native(),
         .source_selection = {},
         .source_range = {},
         .destination_raw_path = (directory.path() / "cancelled.opus").native(),
         .preset = *trackknife::convert::find_encoder_preset("opus-192")},
        {}, cancellation.token());
    CHECK(!cancelled.has_value());
    CHECK(!cancelled && cancelled.error().code == trackknife::core::ErrorCode::cancelled);
    CHECK(entries_besides(directory.path(), {"tone.wav"}) == 0U);
}

} // namespace

int main() {
    builtinPresetsProbeAvailable();
    convertsToEveryPresetAtomically();
    refusesExistingDestinationAndMissingDirectory();
    cancellationLeavesNoPartialOutput();
    return failures == 0 ? 0 : 1;
}
