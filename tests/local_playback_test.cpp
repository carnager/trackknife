// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/audio/local_playback.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/error.hpp"
#include "trackknife/core/stable_id.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
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

[[nodiscard]] std::int16_t pattern_sample(const std::int64_t sample) {
    return static_cast<std::int16_t>(((sample * 37) % 20'001) - 10'000);
}

[[nodiscard]] float expected_float_sample(const std::int64_t sample) {
    return static_cast<float>(pattern_sample(sample)) / 32'768.0F;
}

void write_wave(const std::filesystem::path& path, const std::size_t frame_count,
                const std::int64_t pattern_offset = 0, const std::uint32_t sample_rate = 8'000U) {
    constexpr std::uint16_t channels = 1U;
    constexpr std::uint16_t bits_per_sample = 16U;
    const auto data_bytes = static_cast<std::uint32_t>(frame_count) * (bits_per_sample / 8U);
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
    for (std::size_t sample = 0U; sample < frame_count; ++sample) {
        const auto bits = static_cast<std::uint16_t>(
            pattern_sample(static_cast<std::int64_t>(sample) + pattern_offset));
        const std::array bytes{static_cast<char>(bits & 0xFFU),
                               static_cast<char>((bits >> 8U) & 0xFFU)};
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
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

void transportIsBufferedAndSampleAccurate(const std::filesystem::path& path) {
    constexpr trackknife::audio::PlaybackBufferConfig config{
        .capacity_frames = 256U,
        .start_threshold_frames = 128U,
    };
    auto playback = trackknife::audio::LocalPlayback::open(path.native(), config);
    CHECK(playback.has_value());
    if (!playback) {
        return;
    }
    CHECK(playback->output_format().sample_rate == 8'000);
    CHECK(playback->output_format().channels == 1);
    CHECK(playback->snapshot().state == trackknife::audio::LocalPlaybackState::stopped);
    CHECK(playback->snapshot().position_sample == 0);
    CHECK(playback->snapshot().end_sample == 4'096);

    std::array<float, 128> output{};
    std::ranges::fill(output, 1.0F);
    CHECK(playback->render(output) == 0U);
    CHECK(std::ranges::all_of(output, [](const float sample) { return sample == 0.0F; }));

    CHECK(playback->play().has_value());
    CHECK(playback->snapshot().state == trackknife::audio::LocalPlaybackState::buffering);
    CHECK(playback->render(output) == 0U);
    CHECK(playback->snapshot().underrun_count == 0U);
    CHECK(playback->fill_buffer().has_value());
    CHECK(playback->snapshot().state == trackknife::audio::LocalPlaybackState::playing);
    CHECK(playback->snapshot().buffered_frames == config.capacity_frames);
    CHECK(playback->play().has_value());
    CHECK(playback->snapshot().state == trackknife::audio::LocalPlaybackState::playing);
    CHECK(playback->snapshot().buffered_frames == config.capacity_frames);

    CHECK(playback->render(output) == output.size());
    for (std::size_t sample = 0U; sample < output.size(); ++sample) {
        CHECK(output[sample] == expected_float_sample(static_cast<std::int64_t>(sample)));
    }
    CHECK(playback->snapshot().position_sample == 128);
    CHECK(playback->snapshot().buffered_frames == 128U);

    playback->pause();
    CHECK(playback->snapshot().state == trackknife::audio::LocalPlaybackState::paused);
    CHECK(playback->render(output) == 0U);
    CHECK(playback->snapshot().position_sample == 128);
    CHECK(playback->play().has_value());
    CHECK(playback->snapshot().state == trackknife::audio::LocalPlaybackState::playing);
    CHECK(playback->render(output) == output.size());
    CHECK(playback->snapshot().position_sample == 256);

    CHECK(playback->render(output) == 0U);
    CHECK(playback->snapshot().state == trackknife::audio::LocalPlaybackState::buffering);
    CHECK(playback->snapshot().underrun_count == 1U);
    CHECK(playback->fill_buffer().has_value());
    CHECK(playback->snapshot().state == trackknife::audio::LocalPlaybackState::playing);

    CHECK(playback->seek_to_sample(1'000).has_value());
    CHECK(playback->snapshot().state == trackknife::audio::LocalPlaybackState::buffering);
    CHECK(playback->snapshot().position_sample == 1'000);
    CHECK(playback->snapshot().buffered_frames == 0U);
    CHECK(playback->fill_buffer().has_value());
    CHECK(playback->render(output) == output.size());
    for (std::size_t sample = 0U; sample < output.size(); ++sample) {
        CHECK(output[sample] == expected_float_sample(1'000 + static_cast<std::int64_t>(sample)));
    }

    playback->pause();
    CHECK(playback->seek_to_sample(1'200).has_value());
    CHECK(playback->snapshot().state == trackknife::audio::LocalPlaybackState::paused);
    CHECK(playback->fill_buffer().has_value());
    CHECK(playback->snapshot().buffered_frames == 0U);
    CHECK(playback->play().has_value());
    CHECK(playback->fill_buffer().has_value());
    CHECK(playback->render(output) == output.size());
    CHECK(output.front() == expected_float_sample(1'200));

    const auto before_invalid_seek = playback->snapshot();
    const auto invalid_seek = playback->seek_to_sample(5'000);
    CHECK(!invalid_seek);
    CHECK(invalid_seek.error().code == trackknife::core::ErrorCode::invalid_argument);
    CHECK(playback->snapshot() == before_invalid_seek);

    CHECK(playback->stop().has_value());
    CHECK(playback->snapshot().state == trackknife::audio::LocalPlaybackState::stopped);
    CHECK(playback->snapshot().position_sample == 0);
    CHECK(playback->snapshot().buffered_frames == 0U);
}

void boundedSegmentDrainsAndRestarts(const std::filesystem::path& path) {
    constexpr trackknife::formats::SampleRange range{
        .start_sample = 100,
        .end_sample = 700,
    };
    auto playback = trackknife::audio::LocalPlayback::open_segment(
        path.native(), range, {.capacity_frames = 128U, .start_threshold_frames = 64U});
    CHECK(playback.has_value());
    if (!playback) {
        return;
    }
    CHECK(playback->play().has_value());
    std::array<float, 97> output{};
    std::vector<float> rendered;
    for (std::size_t iteration = 0U; iteration < 64U; ++iteration) {
        CHECK(playback->fill_buffer().has_value());
        const auto frames = playback->render(output);
        rendered.insert(rendered.end(), output.begin(),
                        output.begin() + static_cast<std::ptrdiff_t>(frames));
        if (playback->snapshot().state == trackknife::audio::LocalPlaybackState::ended) {
            break;
        }
    }
    CHECK(playback->snapshot().state == trackknife::audio::LocalPlaybackState::ended);
    CHECK(playback->snapshot().position_sample == 700);
    CHECK(playback->snapshot().buffered_frames == 0U);
    CHECK(playback->snapshot().underrun_count == 0U);
    CHECK(rendered.size() == 600U);
    for (std::size_t sample = 0U; sample < rendered.size(); ++sample) {
        CHECK(rendered[sample] == expected_float_sample(100 + static_cast<std::int64_t>(sample)));
    }

    CHECK(playback->play().has_value());
    CHECK(playback->snapshot().state == trackknife::audio::LocalPlaybackState::buffering);
    CHECK(playback->snapshot().position_sample == 100);
    CHECK(playback->fill_buffer().has_value());
    CHECK(playback->render(output) == output.size());
    CHECK(output.front() == expected_float_sample(100));
}

void validatesConfigurationAndPropagatesCancellation(const std::filesystem::path& path) {
    const auto zero_capacity = trackknife::audio::LocalPlayback::open(
        path.native(), {.capacity_frames = 0U, .start_threshold_frames = 1U});
    CHECK(!zero_capacity);
    CHECK(zero_capacity.error().code == trackknife::core::ErrorCode::invalid_argument);
    const auto excessive_threshold = trackknife::audio::LocalPlayback::open(
        path.native(), {.capacity_frames = 16U, .start_threshold_frames = 17U});
    CHECK(!excessive_threshold);
    CHECK(excessive_threshold.error().code == trackknife::core::ErrorCode::invalid_argument);
    const auto excessive_capacity = trackknife::audio::LocalPlayback::open(
        path.native(),
        {.capacity_frames = std::numeric_limits<std::size_t>::max(), .start_threshold_frames = 1U});
    CHECK(!excessive_capacity);
    CHECK(excessive_capacity.error().code == trackknife::core::ErrorCode::limit_exceeded);

    const auto zero_duration = trackknife::audio::LocalPlayback::open(
        path.native(), trackknife::audio::PlaybackBufferDurationConfig{
                           .capacity = std::chrono::milliseconds{0},
                           .start_threshold = std::chrono::milliseconds{1}});
    CHECK(!zero_duration);
    CHECK(zero_duration.error().code == trackknife::core::ErrorCode::invalid_argument);
    const auto excessive_duration = trackknife::audio::LocalPlayback::open(
        path.native(), trackknife::audio::PlaybackBufferDurationConfig{
                           .capacity = std::chrono::milliseconds::max(),
                           .start_threshold = std::chrono::milliseconds{1}});
    CHECK(!excessive_duration);
    CHECK(excessive_duration.error().code == trackknife::core::ErrorCode::limit_exceeded);

    auto duration_buffer = trackknife::audio::LocalPlayback::open(
        path.native(), trackknife::audio::PlaybackBufferDurationConfig{
                           .capacity = std::chrono::milliseconds{100},
                           .start_threshold = std::chrono::milliseconds{25}});
    CHECK(duration_buffer.has_value());
    if (duration_buffer) {
        CHECK(duration_buffer->play().has_value());
        CHECK(duration_buffer->fill_buffer().has_value());
        CHECK(duration_buffer->snapshot().buffered_frames == 800U);
        CHECK(duration_buffer->snapshot().state == trackknife::audio::LocalPlaybackState::playing);
    }

    trackknife::core::CancellationSource cancellation;
    auto cancelled = trackknife::audio::LocalPlayback::open(
        path.native(), {.capacity_frames = 128U, .start_threshold_frames = 64U},
        cancellation.token());
    CHECK(cancelled.has_value());
    if (cancelled) {
        CHECK(cancelled->play().has_value());
        cancellation.request_cancellation();
        const auto fill = cancelled->fill_buffer();
        CHECK(!fill);
        CHECK(fill.error().code == trackknife::core::ErrorCode::cancelled);
        CHECK(cancelled->snapshot().state == trackknife::audio::LocalPlaybackState::failed);
        const auto replay = cancelled->play();
        CHECK(!replay);
        CHECK(replay.error().code == trackknife::core::ErrorCode::conflict);
        const auto seek = cancelled->seek_to_sample(100);
        CHECK(!seek);
        CHECK(seek.error().code == trackknife::core::ErrorCode::conflict);
    }
}

void producerAndConsumerRunConcurrently(const std::filesystem::path& path) {
    auto playback = trackknife::audio::LocalPlayback::open(
        path.native(), {.capacity_frames = 127U, .start_threshold_frames = 31U});
    CHECK(playback.has_value());
    if (!playback) {
        return;
    }
    CHECK(playback->play().has_value());
    std::atomic_bool producer_failed{false};
    std::jthread producer{[&playback, &producer_failed] {
        while (true) {
            const auto state = playback->snapshot().state;
            if (state == trackknife::audio::LocalPlaybackState::ended ||
                state == trackknife::audio::LocalPlaybackState::failed) {
                return;
            }
            if (!playback->fill_buffer()) {
                producer_failed.store(true, std::memory_order_release);
                return;
            }
            std::this_thread::yield();
        }
    }};

    std::array<float, 43> output{};
    std::vector<float> rendered;
    for (std::size_t iteration = 0U; iteration < 100'000U; ++iteration) {
        const auto frames = playback->render(output);
        rendered.insert(rendered.end(), output.begin(),
                        output.begin() + static_cast<std::ptrdiff_t>(frames));
        if (playback->snapshot().state == trackknife::audio::LocalPlaybackState::ended) {
            break;
        }
        std::this_thread::yield();
    }
    producer.join();
    CHECK(!producer_failed.load(std::memory_order_acquire));
    CHECK(playback->snapshot().state == trackknife::audio::LocalPlaybackState::ended);
    CHECK(playback->snapshot().position_sample == 4'096);
    CHECK(rendered.size() == 4'096U);
    for (std::size_t sample = 0U; sample < rendered.size(); ++sample) {
        CHECK(rendered[sample] == expected_float_sample(static_cast<std::int64_t>(sample)));
    }
}

// Gapless continuation: a queued source takes over in the same ring at decode
// end and the consumed stream is the byte-exact concatenation of both
// sources — no gap, duplicate, or reordering at the boundary.
void chainsQueuedSourceGaplessly(const std::filesystem::path& first,
                                 const std::filesystem::path& second,
                                 const std::filesystem::path& other_rate) {
    constexpr trackknife::audio::PlaybackBufferConfig config{
        .capacity_frames = 256U,
        .start_threshold_frames = 128U,
    };
    constexpr std::int64_t first_frames = 4'096;
    constexpr std::int64_t second_frames = 2'048;
    constexpr std::int64_t second_pattern_offset = 100'000;

    auto playback = trackknife::audio::LocalPlayback::open(first.native(), config);
    CHECK(playback.has_value());
    if (!playback) {
        return;
    }

    // A continuation with a different PCM format is rejected up front.
    const auto mismatched = playback->queue_next(other_rate.native());
    CHECK(!mismatched.has_value());
    CHECK(!mismatched && mismatched.error().code == trackknife::core::ErrorCode::unsupported);
    CHECK(!playback->snapshot().next_queued);

    CHECK(playback->queue_next(second.native()).has_value());
    CHECK(playback->snapshot().next_queued);
    // Only one continuation may be queued at a time.
    const auto duplicate = playback->queue_next(second.native());
    CHECK(!duplicate.has_value());
    CHECK(!duplicate && duplicate.error().code == trackknife::core::ErrorCode::conflict);

    // A seek drops the queued continuation with the flushed buffer.
    CHECK(playback->seek_to_sample(64).has_value());
    CHECK(!playback->snapshot().next_queued);
    CHECK(playback->seek_to_sample(0).has_value());
    CHECK(playback->queue_next(second.native()).has_value());

    CHECK(playback->play().has_value());
    std::vector<float> collected;
    collected.reserve(static_cast<std::size_t>(first_frames + second_frames));
    std::array<float, 64> block{};
    bool crossed_before_boundary = false;
    for (int guard = 0; guard < 10'000 &&
                        playback->snapshot().state != trackknife::audio::LocalPlaybackState::ended;
         ++guard) {
        CHECK(playback->fill_buffer().has_value());
        const auto copied = playback->render(block);
        if (playback->snapshot().chain_crossed &&
            collected.size() + copied < static_cast<std::size_t>(first_frames)) {
            crossed_before_boundary = true;
        }
        collected.insert(collected.end(), block.begin(),
                         block.begin() + static_cast<std::ptrdiff_t>(copied));
    }
    CHECK(!crossed_before_boundary);
    CHECK(playback->snapshot().state == trackknife::audio::LocalPlaybackState::ended);
    CHECK(collected.size() == static_cast<std::size_t>(first_frames + second_frames));
    CHECK(playback->snapshot().end_sample == first_frames + second_frames);

    bool exact = true;
    for (std::int64_t sample = 0; sample < static_cast<std::int64_t>(collected.size()); ++sample) {
        const auto expected =
            sample < first_frames
                ? expected_float_sample(sample)
                : expected_float_sample(sample - first_frames + second_pattern_offset);
        exact = exact && collected[static_cast<std::size_t>(sample)] == expected;
    }
    CHECK(exact);

    const auto crossing = playback->take_chain_crossing();
    CHECK(crossing.has_value());
    CHECK(crossing && *crossing == first_frames);
    CHECK(!playback->take_chain_crossing().has_value());
}

void chainsSegmentsOfOnePhysicalSource(const std::filesystem::path& path) {
    constexpr trackknife::audio::PlaybackBufferConfig config{
        .capacity_frames = 128U,
        .start_threshold_frames = 64U,
    };
    constexpr trackknife::formats::SampleRange first_range{
        .start_sample = 100,
        .end_sample = 700,
    };
    constexpr trackknife::formats::SampleRange second_range{
        .start_sample = 700,
        .end_sample = 1'200,
    };
    auto playback =
        trackknife::audio::LocalPlayback::open_segment(path.native(), first_range, config);
    CHECK(playback.has_value());
    if (!playback) {
        return;
    }
    CHECK(playback->queue_next_segment(path.native(), second_range).has_value());
    CHECK(playback->play().has_value());

    std::vector<float> collected;
    std::array<float, 73> block{};
    for (int guard = 0; guard < 1'000 &&
                        playback->snapshot().state != trackknife::audio::LocalPlaybackState::ended;
         ++guard) {
        CHECK(playback->fill_buffer().has_value());
        const auto copied = playback->render(block);
        collected.insert(collected.end(), block.begin(),
                         block.begin() + static_cast<std::ptrdiff_t>(copied));
    }

    CHECK(playback->snapshot().state == trackknife::audio::LocalPlaybackState::ended);
    CHECK(collected.size() == 1'100U);
    CHECK(playback->snapshot().end_sample == 1'200);
    CHECK(playback->take_chain_crossing() == 700);
    for (std::size_t index = 0U; index < collected.size(); ++index) {
        CHECK(collected[index] == expected_float_sample(100 + static_cast<std::int64_t>(index)));
    }
}

void chainsSelectedCodecSubsongs(const std::filesystem::path& fixture_directory,
                                 const std::filesystem::path& root) {
    const auto binary = decode_base64_file(fixture_directory / "two-subsongs-mod.b64");
    CHECK(binary.has_value());
    if (!binary) {
        return;
    }
    const auto path = root / "two-subsongs.mod";
    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(binary->data()),
                 static_cast<std::streamsize>(binary->size()));
    output.close();
    CHECK(output.good());

    constexpr trackknife::audio::PlaybackBufferDurationConfig config{
        .capacity = std::chrono::milliseconds{100},
        .start_threshold = std::chrono::milliseconds{20},
    };
    constexpr trackknife::formats::SampleRange range{
        .start_sample = 0,
        .end_sample = 28'800,
    };
    auto playback = trackknife::audio::LocalPlayback::open_selected_segment(
        path.native(), {.stream_index = 0, .subsong_index = 0}, range, config);
    CHECK(playback.has_value());
    if (!playback) {
        return;
    }
    CHECK(playback
              ->queue_next_selected_segment(path.native(), {.stream_index = 0, .subsong_index = 1},
                                            range)
              .has_value());
    CHECK(playback->play().has_value());

    std::vector<float> collected;
    std::array<float, 512> block{};
    for (int guard = 0; guard < 2'000 &&
                        playback->snapshot().state != trackknife::audio::LocalPlaybackState::ended;
         ++guard) {
        CHECK(playback->fill_buffer().has_value());
        const auto copied_frames = playback->render(block);
        collected.insert(collected.end(), block.begin(),
                         block.begin() + static_cast<std::ptrdiff_t>(copied_frames * 2U));
    }
    CHECK(playback->snapshot().state == trackknife::audio::LocalPlaybackState::ended);
    CHECK(playback->snapshot().end_sample == 57'600);
    CHECK(playback->take_chain_crossing() == 28'800);
    CHECK(collected.size() == 115'200U);
    CHECK(std::ranges::any_of(collected,
                              [](const float sample) { return std::abs(sample) > 0.0001F; }));
}

} // namespace

int main(const int argc, char** argv) {
    CHECK(argc == 2);
    const auto root =
        std::filesystem::temp_directory_path() /
        ("trackknife-local-playback-" + trackknife::core::StableId::random().to_string());
    std::error_code error;
    CHECK(std::filesystem::create_directories(root, error));
    const auto path = root / "pattern.wav";
    write_wave(path, 4'096U);
    const auto continuation = root / "continuation.wav";
    write_wave(continuation, 2'048U, 100'000);
    const auto other_rate = root / "other-rate.wav";
    write_wave(other_rate, 2'048U, 0, 44'100U);

    transportIsBufferedAndSampleAccurate(path);
    boundedSegmentDrainsAndRestarts(path);
    validatesConfigurationAndPropagatesCancellation(path);
    producerAndConsumerRunConcurrently(path);
    chainsQueuedSourceGaplessly(path, continuation, other_rate);
    chainsSegmentsOfOnePhysicalSource(path);
    if (argc == 2) {
        chainsSelectedCodecSubsongs(argv[1], root);
    }

    std::filesystem::remove_all(root, error);
    CHECK(!error);
    return failures == 0 ? 0 : 1;
}
