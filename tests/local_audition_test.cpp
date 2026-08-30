// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/audio/local_audition.hpp"
#include "trackknife/core/error.hpp"
#include "trackknife/core/stable_id.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ranges>
#include <string_view>
#include <thread>

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

void write_silent_wave(const std::filesystem::path& path, const std::uint32_t frame_count) {
    constexpr std::uint32_t sample_rate = 48'000U;
    constexpr std::uint16_t channels = 1U;
    constexpr std::uint16_t bits_per_sample = 16U;
    const auto data_bytes = frame_count * (bits_per_sample / 8U);
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
    append_u32(header, 28U, sample_rate * (bits_per_sample / 8U));
    append_u16(header, 32U, bits_per_sample / 8U);
    append_u16(header, 34U, bits_per_sample);
    std::ranges::copy(data, header.begin() + 36);
    append_u32(header, 40U, data_bytes);

    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(header.data()),
                 static_cast<std::streamsize>(header.size()));
    constexpr std::array<char, 2> silence{};
    for (std::uint32_t frame = 0U; frame < frame_count; ++frame) {
        output.write(silence.data(), static_cast<std::streamsize>(silence.size()));
    }
}

template <typename Predicate>
trackknife::audio::LocalAuditionSnapshot wait_for(trackknife::audio::LocalAuditionService& service,
                                                  Predicate predicate) {
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    auto snapshot = service.snapshot();
    while (!predicate(snapshot) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(2ms);
        snapshot = service.snapshot();
    }
    return snapshot;
}

} // namespace

int main() {
    using namespace std::chrono_literals;
    using trackknife::audio::LocalAuditionState;
    using trackknife::audio::PlaybackBufferPreset;

    CHECK(trackknife::audio::playback_buffer_preset_id(PlaybackBufferPreset::responsive) ==
          "responsive");
    CHECK(trackknife::audio::playback_buffer_preset_from_id("balanced") ==
          PlaybackBufferPreset::balanced);
    CHECK(!trackknife::audio::playback_buffer_preset_from_id("unknown"));
    CHECK((trackknife::audio::playback_buffer_preset_config(PlaybackBufferPreset::responsive) ==
           trackknife::audio::PlaybackBufferDurationConfig{.capacity = 250ms,
                                                           .start_threshold = 50ms}));
    CHECK((trackknife::audio::playback_buffer_preset_config(PlaybackBufferPreset::balanced) ==
           trackknife::audio::PlaybackBufferDurationConfig{.capacity = 750ms,
                                                           .start_threshold = 100ms}));
    CHECK((trackknife::audio::playback_buffer_preset_config(PlaybackBufferPreset::resilient) ==
           trackknife::audio::PlaybackBufferDurationConfig{.capacity = 2'000ms,
                                                           .start_threshold = 250ms}));

    auto invalid_period = trackknife::audio::LocalAuditionService::create(
        {.buffer = {}, .output = {}, .producer_period = 0ms, .command_capacity = 4U});
    CHECK(!invalid_period);
    CHECK(invalid_period.error().code == trackknife::core::ErrorCode::invalid_argument);
    auto invalid_capacity = trackknife::audio::LocalAuditionService::create(
        {.buffer = {}, .output = {}, .producer_period = 1ms, .command_capacity = 0U});
    CHECK(!invalid_capacity);
    CHECK(invalid_capacity.error().code == trackknife::core::ErrorCode::invalid_argument);

    auto service = trackknife::audio::LocalAuditionService::create(
        {.buffer = {.capacity = 250ms, .start_threshold = 50ms},
         .output = {.stream_name = "Trackknife local audition integration test",
                    .target_object = std::nullopt,
                    .transition_timeout = 3s,
                    .exclusive = false},
         .producer_period = 2ms,
         .command_capacity = 16U});
    CHECK(service.has_value());
    if (!service) {
        return 1;
    }
    CHECK((*service)->snapshot().state == LocalAuditionState::empty);
    CHECK(((*service)->snapshot().configured_buffer ==
           trackknife::audio::PlaybackBufferDurationConfig{.capacity = 250ms,
                                                           .start_threshold = 50ms}));
    CHECK(!(*service)->snapshot().active_buffer);
    CHECK(!(*service)->set_buffer_config({.capacity = 0ms, .start_threshold = 0ms}).has_value());
    CHECK(!(*service)->set_buffer_config({.capacity = 11s, .start_threshold = 100ms}).has_value());
    const auto empty_path = (*service)->load_and_play({});
    CHECK(!empty_path);
    CHECK(empty_path.error().code == trackknife::core::ErrorCode::invalid_argument);

    const auto missing_path =
        (std::filesystem::temp_directory_path() /
         ("trackknife-missing-" + trackknife::core::StableId::random().to_string()))
            .native();
    const auto balanced_buffer =
        trackknife::audio::playback_buffer_preset_config(PlaybackBufferPreset::balanced);
    // A following load supersedes transport work, but not a setting queued
    // before it: the source must observe the latest buffer policy.
    CHECK((*service)->set_buffer_config(balanced_buffer).has_value());
    CHECK((*service)->load_and_play(missing_path).has_value());
    const auto missing = wait_for(**service, [](const auto& snapshot) {
        return snapshot.state == LocalAuditionState::failed;
    });
    CHECK(missing.state == LocalAuditionState::failed);
    CHECK(missing.raw_path == missing_path);
    CHECK(missing.error.has_value());
    CHECK(missing.configured_buffer == balanced_buffer);
    const auto responsive_buffer =
        trackknife::audio::playback_buffer_preset_config(PlaybackBufferPreset::responsive);
    CHECK((*service)->set_buffer_config(responsive_buffer).has_value());
    const auto responsive_configured = wait_for(**service, [&responsive_buffer](const auto& value) {
        return value.configured_buffer == responsive_buffer;
    });
    CHECK(responsive_configured.configured_buffer == responsive_buffer);

    // Volume is validated at the API boundary, survives without a source, and
    // is reapplied when the next source connects.
    const auto negative_volume = (*service)->set_volume_percent(-1);
    CHECK(!negative_volume);
    CHECK(negative_volume.error().code == trackknife::core::ErrorCode::invalid_argument);
    const auto oversized_volume = (*service)->set_volume_percent(101);
    CHECK(!oversized_volume);
    CHECK((*service)->set_volume_percent(40).has_value());
    const auto volume_set =
        wait_for(**service, [](const auto& snapshot) { return snapshot.volume_percent == 40; });
    CHECK(volume_set.volume_percent == 40);

    const auto path =
        std::filesystem::temp_directory_path() /
        ("trackknife-local-audition-" + trackknife::core::StableId::random().to_string() + ".wav");
    write_silent_wave(path, 48'000U);
    CHECK((*service)->load_and_play(path.native()).has_value());
    auto active = wait_for(**service, [](const auto& snapshot) {
        return snapshot.state == LocalAuditionState::playing ||
               snapshot.state == LocalAuditionState::draining ||
               snapshot.state == LocalAuditionState::ended ||
               snapshot.state == LocalAuditionState::failed;
    });
    if (active.state == LocalAuditionState::failed) {
        std::cout << "SKIP: PipeWire server/output unavailable: "
                  << (active.error ? active.error->message : "unknown error") << '\n';
        std::filesystem::remove(path);
        return failures == 0 ? 77 : 1;
    }
    CHECK(active.format.has_value());
    CHECK(active.format->sample_rate == 48'000);
    CHECK(active.format->channels == 1);
    CHECK((active.active_buffer == trackknife::audio::PlaybackBufferDurationConfig{
                                       .capacity = 250ms, .start_threshold = 50ms}));
    // The retained percent was applied cubically to the connected stream.
    CHECK(active.volume_percent == 40);
    CHECK(active.output.volume > 0.063 && active.output.volume < 0.065);
    CHECK((*service)->set_volume_percent(100).has_value());
    const auto volume_restored = wait_for(**service, [](const auto& snapshot) {
        return snapshot.volume_percent == 100 && snapshot.output.volume > 0.99;
    });
    CHECK(volume_restored.volume_percent == 100);

    CHECK((*service)->pause().has_value());
    const auto paused = wait_for(**service, [](const auto& snapshot) {
        return snapshot.state == LocalAuditionState::paused;
    });
    CHECK(paused.state == LocalAuditionState::paused);
    const auto paused_position = paused.position_sample;
    std::this_thread::sleep_for(30ms);
    CHECK((*service)->snapshot().position_sample == paused_position);

    // A new profile is published without resizing the ring beneath the RT
    // consumer. The active value changes on the following ordinary load.
    CHECK((*service)->set_buffer_config(balanced_buffer).has_value());
    const auto buffer_pending = wait_for(**service, [&balanced_buffer](const auto& snapshot) {
        return snapshot.configured_buffer == balanced_buffer;
    });
    CHECK(buffer_pending.configured_buffer == balanced_buffer);
    CHECK((buffer_pending.active_buffer == trackknife::audio::PlaybackBufferDurationConfig{
                                               .capacity = 250ms, .start_threshold = 50ms}));

    // Device selection: enumeration fills the snapshot, an explicit target
    // reconnects the paused source in place, and default restores it.
    const auto empty_target = (*service)->set_output_target(std::string{});
    CHECK(!empty_target);
    CHECK(empty_target.error().code == trackknife::core::ErrorCode::invalid_argument);
    CHECK((*service)->refresh_output_devices().has_value());
    const auto with_devices =
        wait_for(**service, [](const auto& snapshot) { return !snapshot.devices.empty(); });
    CHECK(!with_devices.devices.empty());
    CHECK(with_devices.device_generation > 0U);
    CHECK(!with_devices.device_monitor_error.has_value());
    CHECK(with_devices.output_target_available);
    if (!with_devices.devices.empty()) {
        const auto target = with_devices.devices.front().name;
        const auto unavailable_target = "trackknife.test.missing-output";
        CHECK((*service)->set_output_target(unavailable_target).has_value());
        const auto unavailable = wait_for(**service, [&unavailable_target](const auto& snapshot) {
            return snapshot.output_target == unavailable_target &&
                   !snapshot.output_target_available;
        });
        CHECK(!unavailable.output_target_available);
        CHECK(unavailable.output_suspended);
        CHECK(unavailable.state == LocalAuditionState::paused);
        CHECK(unavailable.position_sample == paused_position);
        CHECK(unavailable.output.state == trackknife::audio::PipeWireOutputState::unconnected);

        CHECK((*service)->set_output_target(target).has_value());
        const auto retargeted = wait_for(**service, [&target](const auto& snapshot) {
            return snapshot.output_target == target && snapshot.output_target_available &&
                   snapshot.state == LocalAuditionState::paused;
        });
        CHECK(retargeted.output_target == target);
        CHECK(retargeted.output_target_available);
        CHECK(!retargeted.output_suspended);
        CHECK(retargeted.state == LocalAuditionState::paused);
        CHECK(retargeted.position_sample == paused_position);
        CHECK((*service)->set_output_target(std::nullopt).has_value());
        const auto defaulted = wait_for(**service, [](const auto& snapshot) {
            return !snapshot.output_target.has_value() &&
                   snapshot.state == LocalAuditionState::paused;
        });
        CHECK(!defaulted.output_target.has_value());
    }

    CHECK((*service)->seek_to_sample(24'000).has_value());
    const auto sought = wait_for(**service, [](const auto& snapshot) {
        return snapshot.state == LocalAuditionState::paused && snapshot.position_sample == 24'000;
    });
    CHECK(sought.position_sample == 24'000);
    CHECK((*service)->play().has_value());
    const auto resumed = wait_for(**service, [](const auto& snapshot) {
        return snapshot.position_sample >= 36'000 || snapshot.state == LocalAuditionState::ended ||
               snapshot.state == LocalAuditionState::failed;
    });
    CHECK(resumed.state != LocalAuditionState::failed);
    CHECK(resumed.position_sample >= 36'000);
    CHECK(resumed.output.source_frames >= 12'000U);

    CHECK((*service)->stop().has_value());
    const auto stopped = wait_for(**service, [](const auto& snapshot) {
        return snapshot.state == LocalAuditionState::ready;
    });
    CHECK(stopped.state == LocalAuditionState::ready);
    CHECK(stopped.position_sample == 0);
    CHECK(stopped.output.state == trackknife::audio::PipeWireOutputState::paused);

    constexpr trackknife::formats::SampleRange second_segment{
        .start_sample = 12'000,
        .end_sample = 24'000,
    };
    CHECK((*service)->load_segment_and_play(path.native(), second_segment).has_value());
    const auto segment_playback = wait_for(**service, [](const auto& snapshot) {
        return (snapshot.segment.has_value() && snapshot.state != LocalAuditionState::loading) ||
               snapshot.state == LocalAuditionState::failed;
    });
    CHECK(segment_playback.state != LocalAuditionState::failed);
    CHECK(segment_playback.raw_path == path.native());
    CHECK(segment_playback.segment == second_segment);
    CHECK(segment_playback.active_buffer == balanced_buffer);
    CHECK(segment_playback.end_sample == 12'000);
    CHECK(segment_playback.position_sample >= 0);
    CHECK(segment_playback.position_sample <= 12'000);

    CHECK((*service)->clear().has_value());
    const auto cleared = wait_for(**service, [](const auto& snapshot) {
        return snapshot.state == LocalAuditionState::empty;
    });
    CHECK(cleared.state == LocalAuditionState::empty);

    std::filesystem::remove(path);
    return failures == 0 ? 0 : 1;
}
