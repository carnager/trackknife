// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/audio/local_audition.hpp"

#include "trackknife/core/cancellation.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

namespace trackknife::audio {
namespace {

enum class CommandKind {
    load_and_play,
    play,
    pause,
    stop,
    seek,
    set_volume,
    refresh_devices,
    set_target,
    clear,
};

struct Command {
    CommandKind kind{CommandKind::play};
    std::string raw_path;
    std::int64_t target_sample{0};
    int volume_percent{100};
    std::optional<std::string> target;
};

// Sliders are perceptual: PipeWire's stream mixer is linear amplitude, so the
// familiar PulseAudio-style cubic taper maps percent onto it.
[[nodiscard]] double cubic_volume(const int percent) {
    const auto normalized = static_cast<double>(percent) / 100.0;
    return normalized * normalized * normalized;
}

[[nodiscard]] core::Error invalid_config(std::string message) {
    return core::Error{
        .code = core::ErrorCode::invalid_argument,
        .message = std::move(message),
        .context = {},
    };
}

[[nodiscard]] core::Error no_source_error(const std::string& operation) {
    return core::Error{
        .code = core::ErrorCode::conflict,
        .message = "cannot " + operation + " without a loaded local source",
        .context = {},
    };
}

[[nodiscard]] LocalAuditionState map_state(const LocalPlaybackState state) noexcept {
    switch (state) {
    case LocalPlaybackState::stopped:
        return LocalAuditionState::ready;
    case LocalPlaybackState::buffering:
        return LocalAuditionState::buffering;
    case LocalPlaybackState::playing:
        return LocalAuditionState::playing;
    case LocalPlaybackState::paused:
        return LocalAuditionState::paused;
    case LocalPlaybackState::draining:
        return LocalAuditionState::draining;
    case LocalPlaybackState::ended:
        return LocalAuditionState::ended;
    case LocalPlaybackState::failed:
        return LocalAuditionState::failed;
    }
    return LocalAuditionState::failed;
}

} // namespace

struct LocalAuditionService::Impl {
    explicit Impl(LocalAuditionConfig audition_config) : config(std::move(audition_config)) {
        worker = std::jthread{[this](const std::stop_token stop_token) { run(stop_token); }};
    }

    ~Impl() {
        {
            std::lock_guard lock{cancellation_mutex};
            if (source_cancellation) {
                source_cancellation->request_cancellation();
            }
        }
        worker.request_stop();
        command_ready.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
    }

    [[nodiscard]] core::Result<void> enqueue(Command command) {
        std::lock_guard lock{command_mutex};
        if (command.kind == CommandKind::load_and_play) {
            {
                std::lock_guard cancellation_lock{cancellation_mutex};
                if (source_cancellation) {
                    source_cancellation->request_cancellation();
                }
            }
            commands.clear();
        } else if (command.kind == CommandKind::seek) {
            std::erase_if(commands,
                          [](const Command& pending) { return pending.kind == CommandKind::seek; });
        } else if (command.kind == CommandKind::set_volume ||
                   command.kind == CommandKind::refresh_devices ||
                   command.kind == CommandKind::set_target) {
            std::erase_if(commands, [kind = command.kind](const Command& pending) {
                return pending.kind == kind;
            });
        }
        if (commands.size() >= config.command_capacity) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::limit_exceeded,
                .message = "local audition command queue is full",
                .context = {{.key = "capacity", .value = std::to_string(config.command_capacity)}},
            });
        }
        if (command.kind == CommandKind::load_and_play) {
            std::lock_guard snapshot_lock{snapshot_mutex};
            published.state = LocalAuditionState::loading;
            published.raw_path = command.raw_path;
            published.format.reset();
            published.position_sample = 0;
            published.end_sample.reset();
            published.buffered_frames = 0U;
            published.underrun_count = 0U;
            published.output = {};
            published.error.reset();
        }
        commands.push_back(std::move(command));
        command_ready.notify_one();
        return {};
    }

    [[nodiscard]] LocalAuditionSnapshot snapshot() const {
        std::lock_guard lock{snapshot_mutex};
        return published;
    }

    void publish(std::optional<core::Error> error = std::nullopt) {
        LocalAuditionSnapshot next;
        next.raw_path = current_path;
        next.error = std::move(error);
        if (source) {
            const auto playback = source->snapshot();
            next.state = map_state(playback.state);
            next.format = source->output_format();
            next.position_sample = playback.position_sample;
            next.end_sample = playback.end_sample;
            next.buffered_frames = playback.buffered_frames;
            next.underrun_count = playback.underrun_count;
        }
        next.volume_percent = volume_percent;
        next.output_target = config.output.target_object;
        next.devices = devices;
        if (output) {
            next.output = output->snapshot();
        }
        // A failed load stays failed across unrelated publishes (volume or
        // device updates) until the next load or clear resets it.
        if (!next.error && sticky_failure) {
            next.error = sticky_failure;
        }
        if (!source && sticky_failure) {
            next.state = LocalAuditionState::failed;
        }
        if (next.error && (!source || next.state == LocalAuditionState::failed)) {
            next.state = LocalAuditionState::failed;
        }
        std::lock_guard lock{snapshot_mutex};
        published = std::move(next);
        last_publish = std::chrono::steady_clock::now();
    }

    void fail(core::Error error) {
        if (output) {
            static_cast<void>(output->quiesce());
            output_active = false;
        }
        sticky_failure = error;
        publish(std::move(error));
        std::lock_guard lock{snapshot_mutex};
        published.state = LocalAuditionState::failed;
    }

    void clear_source() {
        if (output) {
            const auto quiet = output->quiesce();
            if (!quiet) {
                fail(quiet.error());
                return;
            }
            output_active = false;
        }
        output.reset();
        source.reset();
        {
            std::lock_guard lock{cancellation_mutex};
            source_cancellation.reset();
        }
        current_path.clear();
        sticky_failure.reset();
        publish();
    }

    void load(Command command) {
        if (output) {
            const auto quiet = output->quiesce();
            if (!quiet) {
                fail(quiet.error());
                return;
            }
            output_active = false;
        }
        output.reset();
        source.reset();
        sticky_failure.reset();
        current_path = std::move(command.raw_path);
        std::shared_ptr<core::CancellationSource> cancellation;
        {
            std::lock_guard lock{cancellation_mutex};
            source_cancellation = std::make_shared<core::CancellationSource>();
            cancellation = source_cancellation;
        }
        auto opened = LocalPlayback::open(current_path, config.buffer, cancellation->token());
        if (!opened) {
            {
                std::lock_guard lock{cancellation_mutex};
                if (source_cancellation == cancellation) {
                    source_cancellation.reset();
                }
            }
            fail(std::move(opened.error()));
            return;
        }
        source.emplace(std::move(*opened));
        auto connected = PipeWireOutput::connect(*source, config.output);
        if (!connected) {
            fail(std::move(connected.error()));
            return;
        }
        output.emplace(std::move(*connected));
        if (auto volume_applied = output->set_volume(cubic_volume(volume_percent));
            !volume_applied) {
            fail(std::move(volume_applied.error()));
            return;
        }
        auto started = source->play();
        if (!started) {
            fail(std::move(started.error()));
            return;
        }
        auto filled = source->fill_buffer();
        if (!filled) {
            fail(std::move(filled.error()));
            return;
        }
        auto active = output->activate();
        if (!active) {
            fail(std::move(active.error()));
            return;
        }
        output_active = true;
        publish();
    }

    void play() {
        if (!source || !output) {
            publish(no_source_error("play"));
            return;
        }
        if (source->snapshot().state == LocalPlaybackState::ended) {
            if (output_active) {
                auto drained = output->drain();
                if (!drained) {
                    fail(std::move(drained.error()));
                    return;
                }
            }
            auto quiet = output->quiesce();
            if (!quiet) {
                fail(std::move(quiet.error()));
                return;
            }
            output_active = false;
        }
        auto started = source->play();
        if (!started) {
            fail(std::move(started.error()));
            return;
        }
        auto filled = source->fill_buffer();
        if (!filled) {
            fail(std::move(filled.error()));
            return;
        }
        auto active = output->activate();
        if (!active) {
            fail(std::move(active.error()));
            return;
        }
        output_active = true;
        publish();
    }

    void pause() {
        if (!source || !output) {
            publish(no_source_error("pause"));
            return;
        }
        source->pause();
        auto quiet = output->quiesce();
        if (!quiet) {
            fail(std::move(quiet.error()));
            return;
        }
        output_active = false;
        publish();
    }

    void stop() {
        if (!source || !output) {
            publish(no_source_error("stop"));
            return;
        }
        auto quiet = output->quiesce();
        if (!quiet) {
            fail(std::move(quiet.error()));
            return;
        }
        output_active = false;
        auto stopped = source->stop();
        if (!stopped) {
            fail(std::move(stopped.error()));
            return;
        }
        publish();
    }

    void seek(const std::int64_t target_sample) {
        if (!source || !output) {
            publish(no_source_error("seek"));
            return;
        }
        const auto before = source->snapshot().state;
        const bool resume = before == LocalPlaybackState::buffering ||
                            before == LocalPlaybackState::playing ||
                            before == LocalPlaybackState::draining;
        auto quiet = output->quiesce();
        if (!quiet) {
            fail(std::move(quiet.error()));
            return;
        }
        output_active = false;
        auto sought = source->seek_to_sample(target_sample);
        if (!sought) {
            publish(std::move(sought.error()));
            return;
        }
        if (resume) {
            auto started = source->play();
            if (!started) {
                fail(std::move(started.error()));
                return;
            }
            auto filled = source->fill_buffer();
            if (!filled) {
                fail(std::move(filled.error()));
                return;
            }
            auto active = output->activate();
            if (!active) {
                fail(std::move(active.error()));
                return;
            }
            output_active = true;
        }
        publish();
    }

    void refresh_devices() {
        auto listed = list_pipewire_output_devices();
        if (!listed) {
            publish(std::move(listed.error()));
            return;
        }
        devices = std::move(*listed);
        publish();
    }

    void set_target(std::optional<std::string> target) {
        config.output.target_object = std::move(target);
        if (!source || !output) {
            publish();
            return;
        }
        const auto before = source->snapshot().state;
        const bool resume = before == LocalPlaybackState::buffering ||
                            before == LocalPlaybackState::playing ||
                            before == LocalPlaybackState::draining;
        auto quiet = output->quiesce();
        if (!quiet) {
            fail(std::move(quiet.error()));
            return;
        }
        output_active = false;
        output.reset();
        auto connected = PipeWireOutput::connect(*source, config.output);
        if (!connected) {
            fail(std::move(connected.error()));
            return;
        }
        output.emplace(std::move(*connected));
        if (auto volume_applied = output->set_volume(cubic_volume(volume_percent));
            !volume_applied) {
            fail(std::move(volume_applied.error()));
            return;
        }
        if (resume) {
            auto started = source->play();
            if (!started) {
                fail(std::move(started.error()));
                return;
            }
            auto filled = source->fill_buffer();
            if (!filled) {
                fail(std::move(filled.error()));
                return;
            }
            auto active = output->activate();
            if (!active) {
                fail(std::move(active.error()));
                return;
            }
            output_active = true;
        }
        publish();
    }

    void set_volume(const int percent) {
        volume_percent = percent;
        if (output) {
            // A volume failure is reported but never tears down playback.
            if (auto applied = output->set_volume(cubic_volume(percent)); !applied) {
                publish(std::move(applied.error()));
                return;
            }
        }
        publish();
    }

    void execute(Command command) {
        switch (command.kind) {
        case CommandKind::load_and_play:
            load(std::move(command));
            break;
        case CommandKind::play:
            play();
            break;
        case CommandKind::pause:
            pause();
            break;
        case CommandKind::stop:
            stop();
            break;
        case CommandKind::seek:
            seek(command.target_sample);
            break;
        case CommandKind::set_volume:
            set_volume(command.volume_percent);
            break;
        case CommandKind::refresh_devices:
            refresh_devices();
            break;
        case CommandKind::set_target:
            set_target(std::move(command.target));
            break;
        case CommandKind::clear:
            clear_source();
            break;
        }
    }

    void produce() {
        if (!source || !output) {
            return;
        }
        const auto before = source->snapshot().state;
        // The real-time renderer can finish the stream between producer
        // ticks; the terminal snapshot must still be published and the
        // output drained exactly once, or the UI keeps seeing a stale
        // "draining" forever.
        if (before == LocalPlaybackState::ended && output_active) {
            auto drained = output->drain();
            if (!drained) {
                fail(std::move(drained.error()));
                return;
            }
            auto quiet = output->quiesce();
            if (!quiet) {
                fail(std::move(quiet.error()));
                return;
            }
            output_active = false;
            publish();
            return;
        }
        if (before != LocalPlaybackState::buffering && before != LocalPlaybackState::playing &&
            before != LocalPlaybackState::draining) {
            return;
        }
        auto filled = source->fill_buffer();
        if (!filled) {
            fail(std::move(filled.error()));
            return;
        }
        if (source->snapshot().state == LocalPlaybackState::ended && output_active) {
            auto drained = output->drain();
            if (!drained) {
                fail(std::move(drained.error()));
                return;
            }
            auto quiet = output->quiesce();
            if (!quiet) {
                fail(std::move(quiet.error()));
                return;
            }
            output_active = false;
            publish();
            return;
        }
        constexpr auto publish_period = std::chrono::milliseconds{33};
        if (std::chrono::steady_clock::now() - last_publish >= publish_period) {
            publish();
        }
    }

    void run(const std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            std::optional<Command> command;
            {
                std::unique_lock lock{command_mutex};
                command_ready.wait_for(lock, config.producer_period, [this, &stop_token] {
                    return stop_token.stop_requested() || !commands.empty();
                });
                if (stop_token.stop_requested()) {
                    break;
                }
                if (!commands.empty()) {
                    command.emplace(std::move(commands.front()));
                    commands.pop_front();
                }
            }
            if (command) {
                execute(std::move(*command));
            } else {
                produce();
            }
        }
        if (output) {
            static_cast<void>(output->quiesce());
            output_active = false;
        }
        output.reset();
        source.reset();
    }

    LocalAuditionConfig config;
    mutable std::mutex snapshot_mutex;
    LocalAuditionSnapshot published;
    std::mutex command_mutex;
    std::condition_variable command_ready;
    std::deque<Command> commands;
    std::mutex cancellation_mutex;
    std::shared_ptr<core::CancellationSource> source_cancellation;
    std::optional<LocalPlayback> source;
    std::optional<PipeWireOutput> output;
    bool output_active{false};
    int volume_percent{100};
    std::vector<PipeWireDevice> devices;
    std::optional<core::Error> sticky_failure;
    std::string current_path;
    std::chrono::steady_clock::time_point last_publish{};
    std::jthread worker;
};

LocalAuditionService::LocalAuditionService(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}

LocalAuditionService::~LocalAuditionService() = default;

core::Result<std::unique_ptr<LocalAuditionService>>
LocalAuditionService::create(LocalAuditionConfig config) {
    if (config.buffer.capacity <= std::chrono::milliseconds::zero() ||
        config.buffer.start_threshold <= std::chrono::milliseconds::zero() ||
        config.buffer.start_threshold > config.buffer.capacity) {
        return std::unexpected(invalid_config(
            "local audition buffer duration and threshold must be positive and ordered"));
    }
    if (config.output.stream_name.empty()) {
        return std::unexpected(invalid_config("local audition stream name must not be empty"));
    }
    if (config.output.transition_timeout <= std::chrono::milliseconds::zero()) {
        return std::unexpected(invalid_config("local audition output timeout must be positive"));
    }
    if (config.producer_period <= std::chrono::milliseconds::zero()) {
        return std::unexpected(invalid_config("local audition producer period must be positive"));
    }
    if (config.command_capacity == 0U) {
        return std::unexpected(invalid_config("local audition command capacity must be positive"));
    }
    return std::unique_ptr<LocalAuditionService>{
        new LocalAuditionService{std::make_unique<Impl>(std::move(config))}};
}

LocalAuditionSnapshot LocalAuditionService::snapshot() const { return implementation_->snapshot(); }

core::Result<void> LocalAuditionService::load_and_play(std::string raw_path) {
    if (raw_path.empty()) {
        return std::unexpected(invalid_config("local audition path must not be empty"));
    }
    return implementation_->enqueue(Command{.kind = CommandKind::load_and_play,
                                            .raw_path = std::move(raw_path),
                                            .target_sample = 0,
                                            .volume_percent = 100,
                                            .target = {}});
}

core::Result<void> LocalAuditionService::play() {
    return implementation_->enqueue(Command{.kind = CommandKind::play,
                                            .raw_path = {},
                                            .target_sample = 0,
                                            .volume_percent = 100,
                                            .target = {}});
}

core::Result<void> LocalAuditionService::pause() {
    return implementation_->enqueue(Command{.kind = CommandKind::pause,
                                            .raw_path = {},
                                            .target_sample = 0,
                                            .volume_percent = 100,
                                            .target = {}});
}

core::Result<void> LocalAuditionService::stop() {
    return implementation_->enqueue(Command{.kind = CommandKind::stop,
                                            .raw_path = {},
                                            .target_sample = 0,
                                            .volume_percent = 100,
                                            .target = {}});
}

core::Result<void> LocalAuditionService::seek_to_sample(const std::int64_t target_sample) {
    if (target_sample < 0) {
        return std::unexpected(invalid_config("local audition seek sample must not be negative"));
    }
    return implementation_->enqueue(Command{.kind = CommandKind::seek,
                                            .raw_path = {},
                                            .target_sample = target_sample,
                                            .volume_percent = 100,
                                            .target = {}});
}

core::Result<void> LocalAuditionService::set_volume_percent(const int percent) {
    if (percent < 0 || percent > 100) {
        return std::unexpected(
            invalid_config("local audition volume must be between 0 and 100 percent"));
    }
    return implementation_->enqueue(Command{.kind = CommandKind::set_volume,
                                            .raw_path = {},
                                            .target_sample = 0,
                                            .volume_percent = percent,
                                            .target = {}});
}

core::Result<void> LocalAuditionService::refresh_output_devices() {
    return implementation_->enqueue(Command{.kind = CommandKind::refresh_devices,
                                            .raw_path = {},
                                            .target_sample = 0,
                                            .volume_percent = 100,
                                            .target = {}});
}

core::Result<void> LocalAuditionService::set_output_target(std::optional<std::string> target) {
    if (target && target->empty()) {
        return std::unexpected(
            invalid_config("local audition output target must be a device name or unset"));
    }
    return implementation_->enqueue(Command{.kind = CommandKind::set_target,
                                            .raw_path = {},
                                            .target_sample = 0,
                                            .volume_percent = 100,
                                            .target = std::move(target)});
}

core::Result<void> LocalAuditionService::clear() {
    return implementation_->enqueue(Command{.kind = CommandKind::clear,
                                            .raw_path = {},
                                            .target_sample = 0,
                                            .volume_percent = 100,
                                            .target = {}});
}

} // namespace trackknife::audio
