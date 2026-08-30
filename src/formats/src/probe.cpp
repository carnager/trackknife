// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/formats/probe.hpp"

extern "C" {
#include <libavcodec/codec_desc.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>
#include <libopenmpt/libopenmpt.h>
#include <libopenmpt/libopenmpt_stream_callbacks_file.h>
}

#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>

namespace trackknife::formats {
namespace {

struct FormatCloser {
    void operator()(AVFormatContext* context) const noexcept {
        if (context != nullptr) {
            avformat_close_input(&context);
        }
    }
};

struct FileCloser {
    void operator()(std::FILE* file) const noexcept {
        if (file != nullptr) {
            std::fclose(file);
        }
    }
};

struct OpenMptCloser {
    void operator()(openmpt_module* module) const noexcept {
        if (module != nullptr) {
            openmpt_module_destroy(module);
        }
    }
};

struct InterruptState {
    const core::CancellationToken* cancellation{nullptr};
};

int interrupt_callback(void* opaque) {
    const auto* state = static_cast<const InterruptState*>(opaque);
    return state != nullptr && state->cancellation != nullptr &&
                   state->cancellation->is_cancellation_requested()
               ? 1
               : 0;
}

[[nodiscard]] std::string ffmpeg_error(const int code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    if (av_strerror(code, buffer.data(), buffer.size()) < 0) {
        return "unknown FFmpeg error " + std::to_string(code);
    }
    return buffer.data();
}

[[nodiscard]] core::Error error_for(const int backend_code, const std::string& operation,
                                    const std::string& raw_path,
                                    const core::CancellationToken& cancellation) {
    if (cancellation.is_cancellation_requested() || backend_code == AVERROR_EXIT) {
        return core::Error{.code = core::ErrorCode::cancelled,
                           .message = operation + " was cancelled",
                           .context = {{.key = "path", .value = raw_path}}};
    }
    return core::Error{
        .code =
            backend_code == AVERROR(ENOENT) ? core::ErrorCode::not_found : core::ErrorCode::backend,
        .message = operation + " failed: " + ffmpeg_error(backend_code),
        .context = {{.key = "path", .value = raw_path},
                    {.key = "ffmpeg_error", .value = std::to_string(backend_code)}},
    };
}

[[nodiscard]] std::string channel_layout(const AVChannelLayout& layout) {
    std::array<char, 128> description{};
    const auto length = av_channel_layout_describe(&layout, description.data(), description.size());
    return length < 0 ? std::string{} : std::string{description.data()};
}

void append_tags(const AVDictionary* dictionary, std::vector<ProbedTag>& tags) {
    const AVDictionaryEntry* entry = nullptr;
    while ((entry = av_dict_iterate(dictionary, entry)) != nullptr) {
        if (entry->key == nullptr || entry->value == nullptr) {
            continue;
        }
        tags.push_back(ProbedTag{.name = entry->key, .value = entry->value});
    }
}

constexpr unsigned maximum_chapter_count = 10'000U;
constexpr std::size_t maximum_subsong_count = 10'000U;

[[nodiscard]] std::optional<std::int64_t> sample_boundary(const std::int64_t timestamp,
                                                          const AVRational time_base,
                                                          const int sample_rate,
                                                          const std::int64_t origin_sample) {
    if (timestamp == AV_NOPTS_VALUE || time_base.num <= 0 || time_base.den <= 0 ||
        sample_rate <= 0) {
        return std::nullopt;
    }
    const auto scaled = av_rescale_q_rnd(timestamp, time_base,
                                         AVRational{.num = 1, .den = sample_rate}, AV_ROUND_DOWN);
    if (scaled == std::numeric_limits<std::int64_t>::min() || scaled < origin_sample ||
        (origin_sample < 0 && scaled > std::numeric_limits<std::int64_t>::max() + origin_sample)) {
        return std::nullopt;
    }
    return scaled - origin_sample;
}

[[nodiscard]] core::Result<std::vector<ProbedSubsong>>
probe_openmpt_subsongs(const std::string& raw_path, const int stream_index, const int sample_rate,
                       const core::CancellationToken& cancellation) {
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::cancelled,
            .message = "media subsong probe was cancelled",
            .context = {{.key = "path", .value = raw_path}},
        });
    }
    std::unique_ptr<std::FILE, FileCloser> file{std::fopen(raw_path.c_str(), "rb")};
    if (!file) {
        return std::unexpected(core::Error{
            .code = errno == ENOENT ? core::ErrorCode::not_found : core::ErrorCode::backend,
            .message = "opening tracker module for subsong discovery failed",
            .context = {{.key = "path", .value = raw_path},
                        {.key = "errno", .value = std::to_string(errno)}},
        });
    }
    int backend_error = OPENMPT_ERROR_OK;
    const char* backend_message = nullptr;
    std::unique_ptr<openmpt_module, OpenMptCloser> module{openmpt_module_create2(
        openmpt_stream_get_file_callbacks2(), file.get(), openmpt_log_func_silent, nullptr, nullptr,
        nullptr, &backend_error, &backend_message, nullptr)};
    std::string message = backend_message == nullptr ? std::string{} : backend_message;
    if (backend_message != nullptr) {
        openmpt_free_string(backend_message);
    }
    if (!module) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::backend,
            .message = "reading tracker module subsongs failed" +
                       (message.empty() ? std::string{} : ": " + message),
            .context = {{.key = "path", .value = raw_path},
                        {.key = "openmpt_error", .value = std::to_string(backend_error)}},
        });
    }
    file.reset();
    const auto count = openmpt_module_get_num_subsongs(module.get());
    if (count < 0 || static_cast<std::size_t>(count) > maximum_subsong_count) {
        return std::unexpected(core::Error{
            .code = count < 0 ? core::ErrorCode::backend : core::ErrorCode::limit_exceeded,
            .message = count < 0 ? "reading tracker module subsong count failed"
                                 : "local media exceeds the codec-native subsong limit",
            .context = {{.key = "path", .value = raw_path},
                        {.key = "subsongs", .value = std::to_string(count)},
                        {.key = "limit", .value = std::to_string(maximum_subsong_count)}},
        });
    }
    if (count < 2) {
        return std::vector<ProbedSubsong>{};
    }

    std::vector<ProbedSubsong> subsongs;
    subsongs.reserve(static_cast<std::size_t>(count));
    for (std::int32_t index = 0; index < count; ++index) {
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::cancelled,
                .message = "media subsong probe was cancelled",
                .context = {{.key = "path", .value = raw_path}},
            });
        }
        if (openmpt_module_select_subsong(module.get(), index) == 0) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::backend,
                .message = "selecting tracker module subsong failed",
                .context = {{.key = "path", .value = raw_path},
                            {.key = "subsong", .value = std::to_string(index)}},
            });
        }
        const auto duration = openmpt_module_get_duration_seconds(module.get());
        const bool valid_duration = std::isfinite(duration) && duration > 0.0;
        const auto duration_ms = valid_duration ? duration * 1'000.0 : 0.0;
        const auto duration_samples =
            valid_duration && sample_rate > 0 ? duration * static_cast<double>(sample_rate) : 0.0;
        const auto* raw_name = openmpt_module_get_subsong_name(module.get(), index);
        auto name = raw_name == nullptr ? std::string{} : std::string{raw_name};
        if (raw_name != nullptr) {
            openmpt_free_string(raw_name);
        }
        subsongs.push_back(ProbedSubsong{
            .selection = AudioSourceSelection{.stream_index = stream_index, .subsong_index = index},
            .source_index = static_cast<std::size_t>(index),
            .name = std::move(name),
            .duration_ms =
                valid_duration &&
                        duration_ms <= static_cast<double>(std::numeric_limits<std::int64_t>::max())
                    ? std::optional{static_cast<std::int64_t>(std::llround(duration_ms))}
                    : std::nullopt,
            .duration_samples =
                valid_duration && sample_rate > 0 &&
                        duration_samples <=
                            static_cast<double>(std::numeric_limits<std::int64_t>::max())
                    ? std::optional{static_cast<std::int64_t>(std::llround(duration_samples))}
                    : std::nullopt,
            .tags = {},
        });
    }
    return subsongs;
}

} // namespace

core::Result<MediaProbe> probe_local_media(const std::string& raw_path,
                                           const core::CancellationToken& cancellation) {
    if (raw_path.empty()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "local media path is empty",
            .context = {},
        });
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::cancelled,
            .message = "media probe was cancelled",
            .context = {{.key = "path", .value = raw_path}},
        });
    }

    InterruptState interrupt{.cancellation = &cancellation};
    auto* allocated = avformat_alloc_context();
    if (allocated == nullptr) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::backend,
            .message = "FFmpeg could not allocate a format context",
            .context = {{.key = "path", .value = raw_path}},
        });
    }
    allocated->interrupt_callback = AVIOInterruptCB{
        .callback = interrupt_callback,
        .opaque = &interrupt,
    };
    auto* opened = allocated;
    const auto open_result = avformat_open_input(&opened, raw_path.c_str(), nullptr, nullptr);
    if (open_result < 0) {
        if (opened != nullptr) {
            avformat_free_context(opened);
        }
        return std::unexpected(
            error_for(open_result, "opening local media", raw_path, cancellation));
    }
    std::unique_ptr<AVFormatContext, FormatCloser> format{opened};

    const auto stream_result = avformat_find_stream_info(format.get(), nullptr);
    if (stream_result < 0) {
        return std::unexpected(
            error_for(stream_result, "reading local media streams", raw_path, cancellation));
    }

    MediaProbe probe{
        .raw_path = raw_path,
        .container_names = format->iformat != nullptr && format->iformat->name != nullptr
                               ? format->iformat->name
                               : "",
        .container_description = format->iformat != nullptr && format->iformat->long_name != nullptr
                                     ? format->iformat->long_name
                                     : "",
        .duration_ms = format->duration == AV_NOPTS_VALUE
                           ? std::nullopt
                           : std::optional{av_rescale_q(format->duration, AV_TIME_BASE_Q,
                                                        AVRational{.num = 1, .den = 1'000})},
        .bit_rate = format->bit_rate,
        .audio_streams = {},
        .best_audio_stream = std::nullopt,
        .tags = {},
        .chapters = {},
        .subsongs = {},
    };
    probe.audio_streams.reserve(format->nb_streams);
    for (unsigned index = 0U; index < format->nb_streams; ++index) {
        const auto* parameters = format->streams[index]->codecpar;
        if (parameters == nullptr || parameters->codec_type != AVMEDIA_TYPE_AUDIO) {
            continue;
        }
        const auto* descriptor = avcodec_descriptor_get(parameters->codec_id);
        const auto* sample_format_name =
            av_get_sample_fmt_name(static_cast<AVSampleFormat>(parameters->format));
        probe.audio_streams.push_back(AudioStreamInfo{
            .stream_index = static_cast<int>(index),
            .codec_name =
                descriptor != nullptr && descriptor->name != nullptr ? descriptor->name : "unknown",
            .codec_description = descriptor != nullptr && descriptor->long_name != nullptr
                                     ? descriptor->long_name
                                     : "",
            .sample_format = sample_format_name != nullptr ? sample_format_name : "",
            .channel_layout = channel_layout(parameters->ch_layout),
            .sample_rate = parameters->sample_rate,
            .channels = parameters->ch_layout.nb_channels,
            .bit_rate = parameters->bit_rate,
        });
    }
    const auto best = av_find_best_stream(format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (best >= 0) {
        probe.best_audio_stream = best;
    }
    append_tags(format->metadata, probe.tags);
    if (best >= 0 && static_cast<unsigned>(best) < format->nb_streams) {
        append_tags(format->streams[best]->metadata, probe.tags);
    }
    if (probe.audio_streams.empty()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::unsupported,
            .message = "local media contains no audio stream",
            .context = {{.key = "path", .value = raw_path}},
        });
    }
    if (format->nb_chapters > maximum_chapter_count) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::limit_exceeded,
            .message = "local media exceeds the chapter limit",
            .context = {{.key = "path", .value = raw_path},
                        {.key = "chapters", .value = std::to_string(format->nb_chapters)},
                        {.key = "limit", .value = std::to_string(maximum_chapter_count)}},
        });
    }
    if (best >= 0 && static_cast<unsigned>(best) < format->nb_streams) {
        const auto* stream = format->streams[best];
        const auto sample_rate = stream->codecpar == nullptr ? 0 : stream->codecpar->sample_rate;
        auto origin_sample = std::int64_t{0};
        if (sample_rate > 0 && stream->start_time != AV_NOPTS_VALUE) {
            origin_sample =
                av_rescale_q_rnd(stream->start_time, stream->time_base,
                                 AVRational{.num = 1, .den = sample_rate}, AV_ROUND_DOWN);
        }
        const auto duration_samples =
            format->duration != AV_NOPTS_VALUE && sample_rate > 0
                ? std::optional{av_rescale_q(format->duration, AV_TIME_BASE_Q,
                                             AVRational{.num = 1, .den = sample_rate})}
                : std::nullopt;
        probe.chapters.reserve(format->nb_chapters);
        auto expected_start = std::int64_t{0};
        auto chapters_valid = format->nb_chapters > 0U && duration_samples.has_value();
        for (unsigned index = 0U; chapters_valid && index < format->nb_chapters; ++index) {
            const auto* chapter = format->chapters[index];
            if (chapter == nullptr) {
                chapters_valid = false;
                break;
            }
            const auto start =
                sample_boundary(chapter->start, chapter->time_base, sample_rate, origin_sample);
            const auto end =
                sample_boundary(chapter->end, chapter->time_base, sample_rate, origin_sample);
            if (!start || !end || *start != expected_start || *end <= *start ||
                *end > *duration_samples) {
                chapters_valid = false;
                break;
            }
            ProbedChapter projected{
                .id = chapter->id,
                .source_index = index,
                .start_sample = *start,
                .end_sample = *end,
                .tags = {},
            };
            append_tags(chapter->metadata, projected.tags);
            probe.chapters.push_back(std::move(projected));
            expected_start = *end;
        }
        if (!chapters_valid || expected_start != duration_samples.value_or(-1)) {
            probe.chapters.clear();
        }
    }
    // FFmpeg's libopenmpt demuxer exposes a selected tracker subsong but not
    // the file's count. The format-specific libopenmpt adapter enumerates that
    // bounded identity metadata once; FFmpeg remains the playback decoder.
    // Other multi-stream containers are intentionally not expanded here.
    if (probe.container_names == "libopenmpt") {
        const auto sample_rate = best >= 0 && static_cast<unsigned>(best) < format->nb_streams &&
                                         format->streams[best]->codecpar != nullptr
                                     ? format->streams[best]->codecpar->sample_rate
                                     : 0;
        auto subsongs = probe_openmpt_subsongs(raw_path, best, sample_rate, cancellation);
        if (!subsongs) {
            return std::unexpected(std::move(subsongs.error()));
        }
        probe.subsongs = std::move(*subsongs);
    }
    return probe;
}

} // namespace trackknife::formats
