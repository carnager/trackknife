// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/convert/convert.hpp"
#include "trackknife/convert/preset.hpp"
#include "trackknife/convert/scan.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/metadata/document.hpp"
#include "trackknife/metadata/local_reader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <optional>
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
             .preset = preset,
             .target_sample_rate = {},
             .metadata = {}},
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

void carriesMetadataIntoEveryPreset() {
    TemporaryDirectory directory;
    const auto source = directory.path() / "tone.wav";
    write_sine_wav(source, 0.6, 0.5);

    trackknife::metadata::MetadataDocument document;
    const auto field = [](const std::string& canonical, const std::string& native,
                          std::vector<std::string> values) {
        trackknife::metadata::MetadataField result;
        result.canonical_name = canonical;
        result.native_name = native;
        result.values = std::move(values);
        return result;
    };
    document.fields.push_back(field("title", "TITLE", {"Converted Tone"}));
    document.fields.push_back(field("artist", "ARTIST", {"Fixture Band"}));
    document.fields.push_back(field("tracknumber", "TRACKNUMBER", {"7"}));
    document.fields.push_back(field("replaygaintrackgain", "REPLAYGAIN_TRACK_GAIN", {"-6.50 dB"}));

    for (const auto& preset : trackknife::convert::builtin_encoder_presets()) {
        const auto destination =
            directory.path() / ("tagged-" + preset.id + "." + preset.file_extension);
        const auto converted =
            trackknife::convert::convert_audio_file({.source_raw_path = source.native(),
                                                     .source_selection = {},
                                                     .source_range = {},
                                                     .destination_raw_path = destination.native(),
                                                     .preset = preset,
                                                     .target_sample_rate = {},
                                                     .metadata = document});
        if (!converted) {
            std::cerr << preset.id << ": " << converted.error().message << '\n';
        }
        CHECK(converted.has_value());
        if (!converted) {
            continue;
        }
        const auto reread = trackknife::metadata::read_local_metadata(destination.native());
        CHECK(reread.has_value());
        if (!reread) {
            continue;
        }
        CHECK(reread->document.first_effective_value("title") ==
              std::optional<std::string>{"Converted Tone"});
        CHECK(reread->document.first_effective_value("artist") ==
              std::optional<std::string>{"Fixture Band"});
        CHECK(reread->document.first_effective_value("tracknumber") ==
              std::optional<std::string>{"7"});
        CHECK(reread->document.first_effective_value("replaygaintrackgain") ==
              std::optional<std::string>{"-6.50 dB"});
    }
}

void resamplesOnRequestWithinEncoderConstraints() {
    TemporaryDirectory directory;
    const auto source = directory.path() / "tone.wav";
    write_sine_wav(source, 0.6, 0.5);

    // FLAC accepts any rate, so the requested 96 kHz sticks and the
    // duration scales with it.
    const auto upsampled = trackknife::convert::convert_audio_file(
        {.source_raw_path = source.native(),
         .source_selection = {},
         .source_range = {},
         .destination_raw_path = (directory.path() / "up.flac").native(),
         .preset = *trackknife::convert::find_encoder_preset("flac"),
         .target_sample_rate = 96'000,
         .metadata = {}});
    CHECK(upsampled.has_value());
    CHECK(upsampled && upsampled->sample_rate == 96'000);
    CHECK(upsampled && std::abs(upsampled->duration_samples - 48'000) <= 96'000 / 5);

    // Opus only speaks the 48 kHz family: the same request lands on 48 kHz
    // instead of failing.
    const auto constrained = trackknife::convert::convert_audio_file(
        {.source_raw_path = source.native(),
         .source_selection = {},
         .source_range = {},
         .destination_raw_path = (directory.path() / "constrained.opus").native(),
         .preset = *trackknife::convert::find_encoder_preset("opus-192"),
         .target_sample_rate = 96'000,
         .metadata = {}});
    CHECK(constrained.has_value());
    CHECK(constrained && constrained->sample_rate == 48'000);

    // Absurd rates fail typed before any decoding starts.
    const auto absurd = trackknife::convert::convert_audio_file(
        {.source_raw_path = source.native(),
         .source_selection = {},
         .source_range = {},
         .destination_raw_path = (directory.path() / "absurd.flac").native(),
         .preset = *trackknife::convert::find_encoder_preset("flac"),
         .target_sample_rate = 4'000,
         .metadata = {}});
    CHECK(!absurd.has_value());
    CHECK(!absurd && absurd.error().code == trackknife::core::ErrorCode::invalid_argument);
    CHECK(entries_besides(directory.path(), {"tone.wav", "up.flac", "constrained.opus"}) == 0U);
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
                                                 .preset = preset,
                                                 .target_sample_rate = {},
                                                 .metadata = {}});
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
                                                 .preset = preset,
                                                 .target_sample_rate = {},
                                                 .metadata = {}});
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
         .preset = *trackknife::convert::find_encoder_preset("opus-192"),
         .target_sample_rate = {},
         .metadata = {}},
        {}, cancellation.token());
    CHECK(!cancelled.has_value());
    CHECK(!cancelled && cancelled.error().code == trackknife::core::ErrorCode::cancelled);
    CHECK(entries_besides(directory.path(), {"tone.wav"}) == 0U);
}

void scansItemsInParallelIsolatingFailures() {
    TemporaryDirectory directory;
    const auto preset = *trackknife::convert::find_encoder_preset("opus-192");
    std::vector<trackknife::convert::ConversionScanItem> items;
    for (std::size_t index = 0U; index < 5U; ++index) {
        const auto source = directory.path() / ("tone-" + std::to_string(index) + ".wav");
        write_sine_wav(source, 0.5, 0.5);
        items.push_back(
            {.item_index = index * 10U,
             .source_raw_path = source.native(),
             .selection = {},
             .range = {},
             .destination_raw_path =
                 (directory.path() / ("out-" + std::to_string(index) + ".opus")).native(),
             .metadata = {}});
    }
    // A missing source and an in-scan destination collision must fail alone.
    items.push_back({.item_index = 60U,
                     .source_raw_path = (directory.path() / "missing.wav").native(),
                     .selection = {},
                     .range = {},
                     .destination_raw_path = (directory.path() / "out-missing.opus").native(),
                     .metadata = {}});
    items.push_back({.item_index = 70U,
                     .source_raw_path = items[0].source_raw_path,
                     .selection = {},
                     .range = {},
                     .destination_raw_path = items[0].destination_raw_path,
                     .metadata = {}});

    std::size_t final_completed = 0U;
    const auto result = trackknife::convert::scan_conversion(
        items, {.preset = preset, .maximum_parallelism = 3U, .target_sample_rate = {}},
        [&final_completed](const trackknife::convert::ConversionScanProgress& update) {
            final_completed = std::max(final_completed, update.completed_items);
            CHECK(update.total_items == 7U);
        });
    CHECK(result.has_value());
    if (!result) {
        std::cerr << result.error().message << '\n';
        return;
    }
    CHECK(result->items.size() == 7U);
    CHECK(result->converted_count() == 5U);
    CHECK(result->failed_count() == 2U);
    CHECK(final_completed == 7U);
    CHECK(!result->cancellation_requested);
    for (std::size_t index = 0U; index < 5U; ++index) {
        const auto& entry = result->items[index];
        CHECK(entry.state == trackknife::convert::ConversionScanState::converted);
        CHECK(entry.source_revision.has_value());
        CHECK(entry.converted.has_value() && entry.converted->sample_rate == 48'000);
        CHECK(std::filesystem::exists(entry.destination_raw_path));
    }
    CHECK(result->items[5].state == trackknife::convert::ConversionScanState::failed);
    CHECK(result->items[5].issue.has_value());
    const auto& collision = result->items[6];
    CHECK(collision.state == trackknife::convert::ConversionScanState::failed);
    CHECK(collision.issue.has_value() &&
          collision.issue->code == trackknife::core::ErrorCode::conflict);

    CHECK(trackknife::convert::scan_conversion(
              items, {.preset = preset, .maximum_parallelism = 0U, .target_sample_rate = {}})
              .has_value() == false);

    trackknife::core::CancellationSource cancellation;
    cancellation.request_cancellation();
    const auto cancelled = trackknife::convert::scan_conversion(
        std::span{items}.subspan(0U, 1U),
        {.preset = preset, .maximum_parallelism = 1U, .target_sample_rate = {}}, {},
        cancellation.token());
    CHECK(cancelled.has_value());
    CHECK(cancelled && cancelled->cancellation_requested);
    CHECK(cancelled &&
          cancelled->items[0].state == trackknife::convert::ConversionScanState::cancelled);
}

} // namespace

int main() {
    builtinPresetsProbeAvailable();
    convertsToEveryPresetAtomically();
    carriesMetadataIntoEveryPreset();
    resamplesOnRequestWithinEncoderConstraints();
    refusesExistingDestinationAndMissingDirectory();
    cancellationLeavesNoPartialOutput();
    scansItemsInParallelIsolatingFailures();
    return failures == 0 ? 0 : 1;
}
