// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/formats/probe.hpp"

extern "C" {
#include <libavcodec/codec_desc.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>
}

#include <array>
#include <cerrno>
#include <cstdint>
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
    return probe;
}

} // namespace trackknife::formats
