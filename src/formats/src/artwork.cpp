// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/formats/artwork.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

#include <memory>

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

} // namespace

core::Result<std::vector<unsigned char>>
load_embedded_artwork(const std::string& raw_path, const core::CancellationToken& cancellation) {
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
            .message = "artwork load was cancelled",
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
    if (avformat_open_input(&opened, raw_path.c_str(), nullptr, nullptr) < 0) {
        if (opened != nullptr) {
            avformat_free_context(opened);
        }
        return std::unexpected(core::Error{
            .code = core::ErrorCode::io,
            .message = "opening local media for artwork failed",
            .context = {{.key = "path", .value = raw_path}},
        });
    }
    std::unique_ptr<AVFormatContext, FormatCloser> format{opened};
    if (avformat_find_stream_info(format.get(), nullptr) < 0) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::io,
            .message = "reading local media streams for artwork failed",
            .context = {{.key = "path", .value = raw_path}},
        });
    }

    for (unsigned index = 0U; index < format->nb_streams; ++index) {
        const auto* stream = format->streams[index];
        if ((stream->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0) {
            continue;
        }
        const auto& picture = stream->attached_pic;
        if (picture.data == nullptr || picture.size <= 0) {
            continue;
        }
        return std::vector<unsigned char>{picture.data, picture.data + picture.size};
    }
    return std::unexpected(core::Error{
        .code = core::ErrorCode::not_found,
        .message = "local media has no attached picture",
        .context = {{.key = "path", .value = raw_path}},
    });
}

} // namespace trackknife::formats
