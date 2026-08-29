// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/core/stable_id.hpp"
#include "trackknife/mpd/client.hpp"
#include "trackknife/mpd/music_root.hpp"

#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

struct Options {
    std::string host{"127.0.0.1"};
    unsigned port{6600};
    std::optional<std::filesystem::path> music_root;
};

struct ParsedOptions {
    std::optional<Options> options;
    int exit_code{EXIT_SUCCESS};
};

void usage(std::ostream& output) {
    output << "Usage: trackknife-mpd-probe [--host HOST|SOCKET] [--port PORT] "
              "[--music-root PATH]\n"
              "Password authentication uses the MPD_PASSWORD environment variable so the "
              "secret is not exposed in process arguments.\n";
}

[[nodiscard]] ParsedOptions parse_options(int argc, char** argv) {
    Options result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--help" || argument == "-h") {
            usage(std::cout);
            return {};
        }
        if (index + 1 >= argc) {
            std::cerr << "Missing value for " << argument << '\n';
            usage(std::cerr);
            return {.options = std::nullopt, .exit_code = EXIT_FAILURE};
        }
        const std::string value{argv[++index]};
        if (argument == "--host") {
            result.host = value;
        } else if (argument == "--port") {
            unsigned port = 0U;
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), port);
            if (error != std::errc{} || end != value.data() + value.size() || port == 0U ||
                port > 65'535U) {
                std::cerr << "Invalid MPD port: " << value << '\n';
                return {.options = std::nullopt, .exit_code = EXIT_FAILURE};
            }
            result.port = port;
        } else if (argument == "--music-root") {
            result.music_root = std::filesystem::path{value};
        } else {
            std::cerr << "Unknown option: " << argument << '\n';
            usage(std::cerr);
            return {.options = std::nullopt, .exit_code = EXIT_FAILURE};
        }
    }
    return {.options = std::move(result), .exit_code = EXIT_SUCCESS};
}

void print_error(const trackknife::core::Error& error) {
    std::cerr << "MPD probe failed: " << error.message << '\n';
    for (const auto& context : error.context) {
        std::cerr << "  " << context.key << ": " << context.value << '\n';
    }
}

[[nodiscard]] std::string display_track(const trackknife::mpd::Track& track) {
    const auto artist = track.metadata.first("Artist").value_or("<unknown artist>");
    const auto title = track.metadata.first("Title").value_or(track.uri);
    return std::string{artist} + " — " + std::string{title};
}

} // namespace

int main(int argc, char** argv) {
    auto parsed = parse_options(argc, argv);
    if (!parsed.options) {
        return parsed.exit_code;
    }
    auto options = std::move(*parsed.options);

    std::optional<std::string> password;
    // The process is single-threaded here; reading the environment before any
    // client worker exists is deliberate.
    if (const char* environment_password =
            std::getenv("MPD_PASSWORD"); // NOLINT(concurrency-mt-unsafe)
        environment_password != nullptr && *environment_password != '\0') {
        password = environment_password;
    }

    trackknife::mpd::Profile profile{
        .id = trackknife::core::StableId::random(),
        .name = "probe",
        .host = options.host,
        .port = options.port,
        .password = std::move(password),
        .local_music_root = options.music_root,
        .connect_timeout = std::chrono::milliseconds{5'000},
        .command_timeout = std::chrono::milliseconds{10'000},
    };

    auto connection = trackknife::mpd::Client::connect(profile);
    if (!connection) {
        print_error(connection.error());
        return EXIT_FAILURE;
    }
    auto client = std::move(*connection);
    const auto version = client.protocol_version();
    std::cout << "Connected to MPD protocol " << version.major << '.' << version.minor << '.'
              << version.patch << " at " << options.host << ':' << options.port << '\n';

    const auto capabilities = client.capabilities();
    if (!capabilities) {
        print_error(capabilities.error());
        return EXIT_FAILURE;
    }
    std::cout << "Capabilities: " << capabilities->commands.size() << " commands, "
              << capabilities->tag_types.size() << " tag types";
    if (capabilities->supports_command("switchoutput")) {
        std::cout << ", Melody exclusive output switch advertised";
    }
    std::cout << '\n';

    const auto current = client.current_song();
    if (!current) {
        print_error(current.error());
        return EXIT_FAILURE;
    }
    if (current->empty()) {
        std::cout << "Now playing: nothing\n";
    } else {
        std::cout << "Now playing: " << display_track(current->front()) << '\n';
    }

    const auto queue = client.queue_snapshot();
    if (!queue) {
        print_error(queue.error());
        return EXIT_FAILURE;
    }
    std::cout << "Live queue: " << queue->size() << " occurrences\n";
    const auto preview_count = std::min<std::size_t>(queue->size(), 5U);
    for (std::size_t index = 0U; index < preview_count; ++index) {
        const auto& track = queue->at(index);
        std::cout << "  " << index + 1U << ". " << display_track(track);
        if (track.queue_id) {
            std::cout << " [id " << *track.queue_id << ']';
        }
        if (options.music_root) {
            const auto local =
                trackknife::mpd::resolve_below_music_root(*options.music_root, track.uri);
            std::cout << (local ? " [local mapping valid]" : " [remote only]");
        }
        std::cout << '\n';
    }

    const auto outputs = client.outputs();
    if (!outputs) {
        print_error(outputs.error());
        return EXIT_FAILURE;
    }
    std::cout << "Outputs: " << outputs->size() << '\n';
    for (const auto& output : *outputs) {
        std::cout << "  " << output.id << ": " << output.name << " — "
                  << (output.enabled ? "enabled" : "disabled");
        if (output.online) {
            std::cout << (*output.online ? ", online" : ", offline");
        }
        if (output.primary && *output.primary) {
            std::cout << ", primary";
        }
        if (output.stream_format) {
            std::cout << ", stream " << *output.stream_format;
        }
        std::cout << '\n';
    }
    return EXIT_SUCCESS;
}
