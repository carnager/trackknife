// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/audio/pipewire_output.hpp"

#include <pipewire/keys.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/buffer.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/props.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <thread>
#include <utility>

namespace trackknife::audio {
namespace {

static_assert(std::atomic<PipeWireOutputState>::is_always_lock_free);
static_assert(std::atomic_bool::is_always_lock_free);
static_assert(std::atomic_uint32_t::is_always_lock_free);
static_assert(std::atomic_uint64_t::is_always_lock_free);

[[nodiscard]] core::Error pipewire_error(std::string message, const int result) {
    return core::Error{
        .code = core::ErrorCode::backend,
        .message = std::move(message),
        .context = {{.key = "pipewire_error", .value = spa_strerror(result)}},
    };
}

[[nodiscard]] core::Error invalid_config(std::string message) {
    return core::Error{
        .code = core::ErrorCode::invalid_argument,
        .message = std::move(message),
        .context = {},
    };
}

[[nodiscard]] PipeWireOutputState map_state(const pw_stream_state state) noexcept {
    switch (state) {
    case PW_STREAM_STATE_UNCONNECTED:
        return PipeWireOutputState::unconnected;
    case PW_STREAM_STATE_CONNECTING:
        return PipeWireOutputState::connecting;
    case PW_STREAM_STATE_PAUSED:
        return PipeWireOutputState::paused;
    case PW_STREAM_STATE_STREAMING:
        return PipeWireOutputState::streaming;
    case PW_STREAM_STATE_ERROR:
        return PipeWireOutputState::error;
    }
    return PipeWireOutputState::error;
}

class PipeWireLifetime final {
  public:
    PipeWireLifetime() { pw_init(nullptr, nullptr); }
    ~PipeWireLifetime() { pw_deinit(); }

    PipeWireLifetime(const PipeWireLifetime&) = delete;
    PipeWireLifetime& operator=(const PipeWireLifetime&) = delete;
};

void initialize_pipewire() {
    static const PipeWireLifetime lifetime;
    static_cast<void>(lifetime);
}

} // namespace

struct PipeWireOutput::Impl {
    LocalPlayback* source;
    PipeWireOutputConfig config;
    std::size_t channels;
    pw_thread_loop* loop{nullptr};
    pw_stream* stream{nullptr};
    std::atomic<PipeWireOutputState> state{PipeWireOutputState::unconnected};
    std::atomic_uint32_t active_callbacks{0U};
    std::atomic_bool drain_requested{false};
    std::atomic_uint64_t callback_count{0U};
    std::atomic_uint64_t device_frames{0U};
    std::atomic_uint64_t source_frames{0U};
    std::atomic_uint64_t invalid_buffer_count{0U};
    std::atomic<double> volume{1.0};
    std::string error_message;
    std::atomic_bool loop_started{false};
    bool drained{false};

    Impl(LocalPlayback& playback, PipeWireOutputConfig output_config)
        : source(&playback), config(std::move(output_config)),
          channels(static_cast<std::size_t>(playback.output_format().channels)) {}

    ~Impl() {
        if (loop_started.load(std::memory_order_acquire)) {
            pw_thread_loop_stop(loop);
            loop_started.store(false, std::memory_order_release);
        }
        if (stream != nullptr) {
            pw_stream_destroy(stream);
            stream = nullptr;
        }
        if (loop != nullptr) {
            pw_thread_loop_destroy(loop);
            loop = nullptr;
        }
    }

    static void on_state_changed(void* data, const pw_stream_state /*old_state*/,
                                 const pw_stream_state new_state, const char* error) {
        auto& output = *static_cast<Impl*>(data);
        output.state.store(map_state(new_state), std::memory_order_release);
        if (new_state == PW_STREAM_STATE_ERROR) {
            output.error_message = error != nullptr ? error : "PipeWire stream entered error state";
        }
        if (output.loop_started.load(std::memory_order_acquire)) {
            pw_thread_loop_signal(output.loop, false);
        }
    }

    static void on_drained(void* data) {
        auto& output = *static_cast<Impl*>(data);
        output.drained = true;
        pw_thread_loop_signal(output.loop, false);
    }

    static void on_process(void* data) noexcept {
        auto& output = *static_cast<Impl*>(data);
        output.active_callbacks.fetch_add(1U, std::memory_order_acq_rel);
        output.callback_count.fetch_add(1U, std::memory_order_relaxed);
        if (output.drain_requested.load(std::memory_order_acquire)) {
            output.active_callbacks.fetch_sub(1U, std::memory_order_release);
            return;
        }

        auto* buffer = pw_stream_dequeue_buffer(output.stream);
        if (buffer == nullptr) {
            output.active_callbacks.fetch_sub(1U, std::memory_order_release);
            return;
        }

        const auto channels = output.channels;
        auto* spa_buffer = buffer->buffer;
        auto* plane =
            spa_buffer != nullptr && spa_buffer->n_datas == 1U ? &spa_buffer->datas[0] : nullptr;
        const auto stride = channels * sizeof(float);
        if (plane == nullptr || plane->data == nullptr || plane->chunk == nullptr || stride == 0U ||
            plane->maxsize < stride) {
            output.invalid_buffer_count.fetch_add(1U, std::memory_order_relaxed);
            buffer->size = 0U;
            if (plane != nullptr && plane->chunk != nullptr) {
                plane->chunk->offset = 0U;
                plane->chunk->size = 0U;
                plane->chunk->stride = static_cast<std::int32_t>(stride);
                plane->chunk->flags = SPA_CHUNK_FLAG_EMPTY;
            }
            if (pw_stream_return_buffer(output.stream, buffer) < 0) {
                output.invalid_buffer_count.fetch_add(1U, std::memory_order_relaxed);
            }
            output.active_callbacks.fetch_sub(1U, std::memory_order_release);
            return;
        }

        const auto maximum_frames = static_cast<std::size_t>(plane->maxsize) / stride;
        const auto requested = buffer->requested == 0U
                                   ? maximum_frames
                                   : static_cast<std::size_t>(std::min<std::uint64_t>(
                                         buffer->requested, maximum_frames));
        const auto sample_count = requested * channels;
        auto samples = std::span<float>{static_cast<float*>(plane->data), sample_count};
        const auto copied = output.source->render(samples);

        plane->chunk->offset = 0U;
        plane->chunk->size = static_cast<std::uint32_t>(sample_count * sizeof(float));
        plane->chunk->stride = static_cast<std::int32_t>(stride);
        plane->chunk->flags = 0U;
        buffer->size = requested;
        output.device_frames.fetch_add(requested, std::memory_order_relaxed);
        output.source_frames.fetch_add(copied, std::memory_order_relaxed);
        if (pw_stream_queue_buffer(output.stream, buffer) < 0) {
            output.invalid_buffer_count.fetch_add(1U, std::memory_order_relaxed);
        }
        output.active_callbacks.fetch_sub(1U, std::memory_order_release);
    }

    static constexpr pw_stream_events events{
        .version = PW_VERSION_STREAM_EVENTS,
        .destroy = nullptr,
        .state_changed = on_state_changed,
        .control_info = nullptr,
        .io_changed = nullptr,
        .param_changed = nullptr,
        .add_buffer = nullptr,
        .remove_buffer = nullptr,
        .process = on_process,
        .drained = on_drained,
        .command = nullptr,
        .trigger_done = nullptr,
    };

    [[nodiscard]] core::Result<void> wait_for_state(const PipeWireOutputState wanted,
                                                    const std::string& operation) {
        timespec deadline{};
        const auto timeout_nanoseconds =
            std::chrono::duration_cast<std::chrono::nanoseconds>(config.transition_timeout).count();
        const auto clock_result = pw_thread_loop_get_time(loop, &deadline, timeout_nanoseconds);
        if (clock_result < 0) {
            return std::unexpected(
                pipewire_error("could not start PipeWire state timeout", clock_result));
        }
        while (state.load(std::memory_order_acquire) != wanted) {
            if (state.load(std::memory_order_acquire) == PipeWireOutputState::error) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::backend,
                    .message = error_message.empty() ? "PipeWire stream failed" : error_message,
                    .context = {{.key = "operation", .value = operation}},
                });
            }
            const auto wait_result = pw_thread_loop_timed_wait_full(loop, &deadline);
            if (wait_result == -ETIMEDOUT) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::backend,
                    .message = "PipeWire stream state transition timed out",
                    .context = {{.key = "operation", .value = operation}},
                });
            }
            if (wait_result < 0) {
                return std::unexpected(
                    pipewire_error("could not wait for PipeWire stream state", wait_result));
            }
        }
        return {};
    }

    [[nodiscard]] core::Result<void> wait_for_drain() {
        timespec deadline{};
        const auto timeout_nanoseconds =
            std::chrono::duration_cast<std::chrono::nanoseconds>(config.transition_timeout).count();
        const auto clock_result = pw_thread_loop_get_time(loop, &deadline, timeout_nanoseconds);
        if (clock_result < 0) {
            return std::unexpected(
                pipewire_error("could not start PipeWire drain timeout", clock_result));
        }
        while (!drained) {
            if (state.load(std::memory_order_acquire) == PipeWireOutputState::error) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::backend,
                    .message = error_message.empty() ? "PipeWire stream failed" : error_message,
                    .context = {{.key = "operation", .value = "drain"}},
                });
            }
            const auto wait_result = pw_thread_loop_timed_wait_full(loop, &deadline);
            if (wait_result == -ETIMEDOUT) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::backend,
                    .message = "PipeWire stream drain timed out",
                    .context = {},
                });
            }
            if (wait_result < 0) {
                return std::unexpected(
                    pipewire_error("could not wait for PipeWire stream drain", wait_result));
            }
        }
        return {};
    }
};

PipeWireOutput::PipeWireOutput(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}

PipeWireOutput::PipeWireOutput(PipeWireOutput&&) noexcept = default;
PipeWireOutput& PipeWireOutput::operator=(PipeWireOutput&&) noexcept = default;
PipeWireOutput::~PipeWireOutput() = default;

core::Result<PipeWireOutput> PipeWireOutput::connect(LocalPlayback& source,
                                                     PipeWireOutputConfig config) {
    if (config.stream_name.empty()) {
        return std::unexpected(invalid_config("PipeWire stream name must not be empty"));
    }
    if (config.transition_timeout <= std::chrono::milliseconds::zero()) {
        return std::unexpected(invalid_config("PipeWire transition timeout must be positive"));
    }
    const auto& format = source.output_format();
    if (format.sample_rate <= 0 || (format.channels != 1 && format.channels != 2)) {
        return std::unexpected(invalid_config(
            "PipeWire output currently requires positive-rate mono or stereo source PCM"));
    }

    initialize_pipewire();
    auto output = std::make_unique<Impl>(source, std::move(config));
    output->loop = pw_thread_loop_new("trackknife-pipewire", nullptr);
    if (output->loop == nullptr) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::backend,
            .message = "could not create PipeWire thread loop",
            .context = {},
        });
    }

    auto* properties = pw_properties_new(
        PW_KEY_APP_NAME, "Trackknife", PW_KEY_APP_ID, "io.trackknife.Trackknife", PW_KEY_MEDIA_TYPE,
        "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Music", PW_KEY_MEDIA_NAME,
        output->config.stream_name.c_str(), nullptr);
    if (properties == nullptr) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::backend,
            .message = "could not allocate PipeWire stream properties",
            .context = {},
        });
    }
    if (output->config.target_object) {
        pw_properties_set(properties, PW_KEY_TARGET_OBJECT, output->config.target_object->c_str());
    }
    output->stream = pw_stream_new_simple(pw_thread_loop_get_loop(output->loop),
                                          output->config.stream_name.c_str(), properties,
                                          &Impl::events, output.get());
    if (output->stream == nullptr) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::backend,
            .message = "could not create PipeWire playback stream",
            .context = {},
        });
    }

    spa_audio_info_raw audio_info{};
    audio_info.format = SPA_AUDIO_FORMAT_F32;
    audio_info.rate = static_cast<std::uint32_t>(format.sample_rate);
    audio_info.channels = static_cast<std::uint32_t>(format.channels);
    if (format.channels == 1) {
        audio_info.position[0] = SPA_AUDIO_CHANNEL_MONO;
    } else {
        audio_info.position[0] = SPA_AUDIO_CHANNEL_FL;
        audio_info.position[1] = SPA_AUDIO_CHANNEL_FR;
    }
    std::byte pod_storage[1'024]{};
    spa_pod_builder builder{};
    spa_pod_builder_init(&builder, pod_storage, sizeof(pod_storage));
    const spa_pod* params[]{
        spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &audio_info)};
    // PipeWire declares its bitmask as an enum even though combined values are
    // intentionally passed to pw_stream_connect().
    // NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
    auto flags =
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_INACTIVE |
                                     PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS);
    if (output->config.exclusive) {
        flags = static_cast<pw_stream_flags>(flags | PW_STREAM_FLAG_EXCLUSIVE);
    }
    // NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
    const auto connect_result =
        pw_stream_connect(output->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1U);
    if (connect_result < 0) {
        return std::unexpected(
            pipewire_error("could not connect PipeWire playback stream", connect_result));
    }
    const auto start_result = pw_thread_loop_start(output->loop);
    if (start_result < 0) {
        return std::unexpected(
            pipewire_error("could not start PipeWire playback thread", start_result));
    }
    output->loop_started.store(true, std::memory_order_release);

    pw_thread_loop_lock(output->loop);
    auto ready = output->wait_for_state(PipeWireOutputState::paused, "connect");
    pw_thread_loop_unlock(output->loop);
    if (!ready) {
        return std::unexpected(std::move(ready.error()));
    }
    return PipeWireOutput{std::move(output)};
}

PipeWireOutputSnapshot PipeWireOutput::snapshot() const {
    auto& output = *implementation_;
    pw_thread_loop_lock(output.loop);
    const auto node = pw_stream_get_node_id(output.stream);
    PipeWireOutputSnapshot result{
        .state = output.state.load(std::memory_order_acquire),
        .node_id = node == PW_ID_ANY ? std::nullopt : std::optional{node},
        .callback_count = output.callback_count.load(std::memory_order_acquire),
        .device_frames = output.device_frames.load(std::memory_order_acquire),
        .source_frames = output.source_frames.load(std::memory_order_acquire),
        .invalid_buffer_count = output.invalid_buffer_count.load(std::memory_order_acquire),
        .volume = output.volume.load(std::memory_order_acquire),
        .error_message = output.error_message,
    };
    pw_thread_loop_unlock(output.loop);
    return result;
}

core::Result<void> PipeWireOutput::set_volume(const double volume) {
    auto& output = *implementation_;
    if (output.stream == nullptr) {
        return std::unexpected(invalid_config("PipeWire stream is not connected"));
    }
    const auto clamped = std::clamp(volume, 0.0, 1.0);
    auto value = static_cast<float>(clamped);
    pw_thread_loop_lock(output.loop);
    const auto result = pw_stream_set_control(output.stream, SPA_PROP_volume, 1U, &value, 0);
    pw_thread_loop_unlock(output.loop);
    if (result < 0) {
        return std::unexpected(pipewire_error("PipeWire rejected the stream volume", result));
    }
    output.volume.store(clamped, std::memory_order_release);
    return {};
}

namespace {

struct DeviceEnumeration {
    pw_thread_loop* loop{nullptr};
    std::vector<PipeWireDevice> devices;
    bool done{false};
    int sync_sequence{0};

    static void on_global(void* data, std::uint32_t /*id*/, std::uint32_t /*permissions*/,
                          const char* type, std::uint32_t /*version*/, const spa_dict* props) {
        auto& state = *static_cast<DeviceEnumeration*>(data);
        if (type == nullptr || props == nullptr ||
            std::string_view{type} != PW_TYPE_INTERFACE_Node) {
            return;
        }
        const auto* media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        if (media_class == nullptr || (std::string_view{media_class} != "Audio/Sink" &&
                                       std::string_view{media_class} != "Audio/Duplex")) {
            return;
        }
        const auto* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        if (name == nullptr || *name == '\0') {
            return;
        }
        const auto* description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
        state.devices.push_back(PipeWireDevice{
            .name = name,
            .description = description != nullptr && *description != '\0' ? description : name,
        });
    }

    static void on_done(void* data, const std::uint32_t id, const int sequence) {
        auto& state = *static_cast<DeviceEnumeration*>(data);
        if (id == PW_ID_CORE && sequence == state.sync_sequence) {
            state.done = true;
            pw_thread_loop_signal(state.loop, false);
        }
    }
};

constexpr pw_registry_events device_registry_events{
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = DeviceEnumeration::on_global,
    .global_remove = nullptr,
};

constexpr pw_core_events device_core_events{
    .version = PW_VERSION_CORE_EVENTS,
    .info = nullptr,
    .done = DeviceEnumeration::on_done,
    .ping = nullptr,
    .error = nullptr,
    .remove_id = nullptr,
    .bound_id = nullptr,
    .add_mem = nullptr,
    .remove_mem = nullptr,
    .bound_props = nullptr,
};

} // namespace

core::Result<std::vector<PipeWireDevice>>
list_pipewire_output_devices(const std::chrono::milliseconds timeout) {
    if (timeout <= std::chrono::milliseconds::zero()) {
        return std::unexpected(invalid_config("PipeWire enumeration timeout must be positive"));
    }
    initialize_pipewire();
    DeviceEnumeration state;
    state.loop = pw_thread_loop_new("trackknife-pw-devices", nullptr);
    if (state.loop == nullptr) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::backend,
            .message = "could not create PipeWire enumeration loop",
            .context = {},
        });
    }
    struct LoopGuard {
        pw_thread_loop* loop;
        pw_context* context{nullptr};
        pw_core* core{nullptr};
        pw_registry* registry{nullptr};
        bool started{false};
        ~LoopGuard() {
            if (started) {
                pw_thread_loop_lock(loop);
                if (registry != nullptr) {
                    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry));
                }
                if (core != nullptr) {
                    pw_core_disconnect(core);
                }
                pw_thread_loop_unlock(loop);
                pw_thread_loop_stop(loop);
            }
            if (context != nullptr) {
                pw_context_destroy(context);
            }
            pw_thread_loop_destroy(loop);
        }
    } guard{.loop = state.loop};

    guard.context = pw_context_new(pw_thread_loop_get_loop(state.loop), nullptr, 0);
    if (guard.context == nullptr) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::backend,
            .message = "could not create PipeWire enumeration context",
            .context = {},
        });
    }
    const auto start_result = pw_thread_loop_start(state.loop);
    if (start_result < 0) {
        return std::unexpected(
            pipewire_error("could not start PipeWire enumeration thread", start_result));
    }
    guard.started = true;

    pw_thread_loop_lock(state.loop);
    guard.core = pw_context_connect(guard.context, nullptr, 0);
    if (guard.core == nullptr) {
        pw_thread_loop_unlock(state.loop);
        return std::unexpected(core::Error{
            .code = core::ErrorCode::backend,
            .message = "could not connect to the PipeWire server",
            .context = {},
        });
    }
    guard.registry = pw_core_get_registry(guard.core, PW_VERSION_REGISTRY, 0);
    if (guard.registry == nullptr) {
        pw_thread_loop_unlock(state.loop);
        return std::unexpected(core::Error{
            .code = core::ErrorCode::backend,
            .message = "could not open the PipeWire registry",
            .context = {},
        });
    }
    spa_hook registry_listener{};
    spa_hook core_listener{};
    pw_registry_add_listener(guard.registry, &registry_listener, &device_registry_events, &state);
    pw_core_add_listener(guard.core, &core_listener, &device_core_events, &state);
    state.sync_sequence = pw_core_sync(guard.core, PW_ID_CORE, 0);

    const auto timeout_nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(timeout).count();
    timespec deadline{};
    const auto clock_result = pw_thread_loop_get_time(state.loop, &deadline, timeout_nanoseconds);
    bool timed_out = clock_result < 0;
    while (!state.done && !timed_out) {
        if (pw_thread_loop_timed_wait_full(state.loop, &deadline) == -ETIMEDOUT) {
            timed_out = true;
        }
    }
    spa_hook_remove(&registry_listener);
    spa_hook_remove(&core_listener);
    auto devices = std::move(state.devices);
    pw_thread_loop_unlock(state.loop);
    if (!state.done) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::backend,
            .message = "PipeWire device enumeration timed out",
            .context = {},
        });
    }
    return devices;
}

core::Result<void> PipeWireOutput::activate() {
    auto& output = *implementation_;
    pw_thread_loop_lock(output.loop);
    if (output.state.load(std::memory_order_acquire) == PipeWireOutputState::streaming) {
        pw_thread_loop_unlock(output.loop);
        return {};
    }
    output.drained = false;
    output.drain_requested.store(false, std::memory_order_release);
    const auto active_result = pw_stream_set_active(output.stream, true);
    if (active_result < 0) {
        pw_thread_loop_unlock(output.loop);
        return std::unexpected(
            pipewire_error("could not activate PipeWire playback stream", active_result));
    }
    auto streaming = output.wait_for_state(PipeWireOutputState::streaming, "activate");
    pw_thread_loop_unlock(output.loop);
    return streaming;
}

core::Result<void> PipeWireOutput::quiesce() {
    auto& output = *implementation_;
    pw_thread_loop_lock(output.loop);
    if (output.state.load(std::memory_order_acquire) != PipeWireOutputState::paused) {
        const auto inactive_result = pw_stream_set_active(output.stream, false);
        if (inactive_result < 0) {
            pw_thread_loop_unlock(output.loop);
            return std::unexpected(
                pipewire_error("could not deactivate PipeWire playback stream", inactive_result));
        }
        auto paused = output.wait_for_state(PipeWireOutputState::paused, "quiesce");
        if (!paused) {
            pw_thread_loop_unlock(output.loop);
            return paused;
        }
    }
    const auto flush_result = pw_stream_flush(output.stream, false);
    pw_thread_loop_unlock(output.loop);
    if (flush_result < 0) {
        return std::unexpected(
            pipewire_error("could not flush PipeWire playback stream", flush_result));
    }

    const auto deadline = std::chrono::steady_clock::now() + output.config.transition_timeout;
    while (output.active_callbacks.load(std::memory_order_acquire) != 0U) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::backend,
                .message = "PipeWire real-time callback did not quiesce before timeout",
                .context = {},
            });
        }
        std::this_thread::yield();
    }
    return {};
}

core::Result<void> PipeWireOutput::drain() {
    auto& output = *implementation_;
    if (output.source->snapshot().state != LocalPlaybackState::ended) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = "PipeWire output can drain only after the local source reaches its end",
            .context = {},
        });
    }
    output.drain_requested.store(true, std::memory_order_release);
    const auto callback_deadline =
        std::chrono::steady_clock::now() + output.config.transition_timeout;
    while (output.active_callbacks.load(std::memory_order_acquire) != 0U) {
        if (std::chrono::steady_clock::now() >= callback_deadline) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::backend,
                .message = "PipeWire real-time callback did not finish before drain timeout",
                .context = {},
            });
        }
        std::this_thread::yield();
    }

    pw_thread_loop_lock(output.loop);
    output.drained = false;
    const auto drain_result = pw_stream_flush(output.stream, true);
    if (drain_result < 0) {
        pw_thread_loop_unlock(output.loop);
        return std::unexpected(
            pipewire_error("could not begin PipeWire playback drain", drain_result));
    }
    auto drained = output.wait_for_drain();
    pw_thread_loop_unlock(output.loop);
    return drained;
}

} // namespace trackknife::audio
