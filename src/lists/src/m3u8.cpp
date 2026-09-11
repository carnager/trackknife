// SPDX-License-Identifier: GPL-3.0-only
#include "trackknife/lists/m3u8.hpp"
#include "trackknife/core/unicode.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace trackknife::lists {
namespace {
core::Error error(core::ErrorCode code, std::string message, std::size_t row = 0) {
    core::Error result{.code = code, .message = std::move(message), .context = {}};
    if (row != 0)
        result.context.push_back({"line/row", std::to_string(row)});
    return result;
}
core::Error cancelled() { return error(core::ErrorCode::cancelled, "Playlist transfer cancelled"); }
bool absolutePath(std::string_view path) {
    return path.starts_with('/') && path.find('\0') == std::string_view::npos &&
           !path.ends_with('/');
}
std::string directory(const std::string& path) {
    return path.substr(0, path.find_last_of('/') + 1);
}
bool scheme(std::string_view value) {
    const auto colon = value.find(':');
    const auto slash = value.find('/');
    return colon != std::string_view::npos && (slash == std::string_view::npos || colon < slash);
}
int hex(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
core::Result<std::string> localReference(std::string_view value, const std::string& base,
                                         std::size_t line) {
    if (value.starts_with("file:")) {
        value.remove_prefix(5);
        if (value.starts_with("//localhost/"))
            value.remove_prefix(11);
        else if (value.starts_with("///"))
            value.remove_prefix(2);
        else if (value.starts_with("//"))
            return std::unexpected(
                error(core::ErrorCode::unsupported,
                      "Remote file URI authorities cannot be imported into a local list", line));
        if (!value.starts_with('/') || value.find_first_of("?#") != std::string_view::npos)
            return std::unexpected(
                error(core::ErrorCode::invalid_argument, "Invalid local file URI", line));
        std::string path;
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (value[i] != '%') {
                path += value[i];
                continue;
            }
            if (i + 2 >= value.size() || hex(value[i + 1]) < 0 || hex(value[i + 2]) < 0)
                return std::unexpected(
                    error(core::ErrorCode::invalid_argument, "Invalid file URI escape", line));
            path += static_cast<char>(hex(value[i + 1]) * 16 + hex(value[i + 2]));
            i += 2;
        }
        if (!absolutePath(path))
            return std::unexpected(
                error(core::ErrorCode::invalid_argument, "Invalid local file path", line));
        return path;
    }
    if (scheme(value))
        return std::unexpected(error(
            core::ErrorCode::unsupported,
            "Remote URIs and non-Linux drive paths cannot be imported into a local list", line));
    auto path = value.starts_with('/') ? std::string{value} : base + std::string{value};
    if (!absolutePath(path))
        return std::unexpected(
            error(core::ErrorCode::invalid_argument, "Expected a file reference", line));
    return path;
}
std::string fileUri(std::string_view path) {
    constexpr std::string_view digits = "0123456789ABCDEF";
    std::string result{"file://"};
    for (const char raw : path) {
        const auto c = static_cast<unsigned char>(raw);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '/' || c == '-' || c == '_' || c == '.' || c == '~')
            result += static_cast<char>(c);
        else {
            result += '%';
            result += digits[c >> 4U];
            result += digits[c & 15U];
        }
    }
    return result;
}
struct Descriptor {
    int fd{-1};
    ~Descriptor() {
        if (fd >= 0)
            ::close(fd);
    }
};
core::Error ioError(std::string message) {
    return error(core::ErrorCode::io,
                 std::move(message) + ": " + std::generic_category().message(errno));
}
} // namespace

core::Result<std::vector<PlaylistEntry>> parse_m3u8(std::string_view bytes,
                                                    const std::string& absolute_playlist_path,
                                                    const core::CancellationToken& cancellation,
                                                    std::atomic<std::size_t>* progress) {
    if (cancellation.is_cancellation_requested())
        return std::unexpected(cancelled());
    if (!absolutePath(absolute_playlist_path))
        return std::unexpected(
            error(core::ErrorCode::invalid_argument, "Playlist path must be absolute"));
    if (bytes.size() > m3u8_max_bytes)
        return std::unexpected(error(core::ErrorCode::limit_exceeded, "Playlist exceeds 32 MiB"));
    if (bytes.starts_with("\xef\xbb\xbf"))
        bytes.remove_prefix(3);
    std::vector<PlaylistEntry> entries;
    PlaylistEntry pending;
    const auto base = directory(absolute_playlist_path);
    std::size_t number = 0, retained_bytes = 0;
    while (!bytes.empty()) {
        if (cancellation.is_cancellation_requested())
            return std::unexpected(cancelled());
        ++number;
        const auto end = bytes.find('\n');
        auto line = bytes.substr(0, end);
        bytes.remove_prefix(end == std::string_view::npos ? bytes.size() : end + 1);
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        if (line.size() > m3u8_max_line)
            return std::unexpected(
                error(core::ErrorCode::limit_exceeded, "Playlist line exceeds 64 KiB", number));
        if (line.find('\0') != std::string_view::npos || !core::unicodeCodePointCount(line))
            return std::unexpected(error(core::ErrorCode::invalid_argument,
                                         "M3U8 requires UTF-8 without NUL bytes", number));
        if (line.empty())
            continue;
        // Leading whitespace is commonly ignored by readers. Export encodes
        // such names as file URIs; relative './ name' remains unambiguous.
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.remove_prefix(1);
        if (line.empty())
            continue;
        if (line.starts_with("#EXT-X-"))
            return std::unexpected(error(core::ErrorCode::unsupported,
                                         "HLS manifests are not local playlists", number));
        if (line.starts_with("#EXTINF:")) {
            const auto comma = line.find(',');
            if (comma == std::string_view::npos)
                return std::unexpected(error(core::ErrorCode::invalid_argument,
                                             "EXTINF requires duration and title", number));
            auto duration = line.substr(8, comma - 8);
            double seconds = 0;
            const auto parsed =
                std::from_chars(duration.data(), duration.data() + duration.size(), seconds);
            if (parsed.ec != std::errc{} || parsed.ptr != duration.data() + duration.size() ||
                !std::isfinite(seconds) || seconds < -1 || seconds > 1.0e12)
                return std::unexpected(
                    error(core::ErrorCode::invalid_argument, "Invalid EXTINF duration", number));
            pending.duration_ms =
                seconds < 0
                    ? std::nullopt
                    : std::optional{static_cast<std::int64_t>(std::llround(seconds * 1000))};
            pending.title = line.substr(comma + 1);
            continue;
        }
        if (line.starts_with("#EXT") && line != "#EXTM3U")
            return std::unexpected(error(
                core::ErrorCode::unsupported,
                "Unsupported playlist directive; only EXTM3U and EXTINF are supported", number));
        if (line.starts_with('#'))
            continue;
        if (entries.size() == m3u8_max_entries)
            return std::unexpected(
                error(core::ErrorCode::limit_exceeded, "Playlist exceeds 100,000 entries", number));
        auto path = localReference(line, base, number);
        if (!path)
            return std::unexpected(path.error());
        retained_bytes += sizeof(PlaylistEntry) + path->size() + pending.title.size();
        if (path->size() > m3u8_max_line || retained_bytes > 64U * 1024U * 1024U)
            return std::unexpected(error(core::ErrorCode::limit_exceeded,
                                         "Resolved playlist exceeds the in-memory transfer limit",
                                         number));
        pending.raw_path = std::move(*path);
        entries.push_back(std::move(pending));
        pending = {};
        if (progress)
            progress->store(entries.size(), std::memory_order_relaxed);
    }
    return entries;
}

core::Result<std::vector<PlaylistEntry>> read_m3u8(const std::string& absolute_playlist_path,
                                                   const core::CancellationToken& cancellation,
                                                   std::atomic<std::size_t>* progress) {
    if (cancellation.is_cancellation_requested())
        return std::unexpected(cancelled());
    if (!absolutePath(absolute_playlist_path))
        return std::unexpected(
            error(core::ErrorCode::invalid_argument, "Playlist path must be absolute"));
    Descriptor file{::open(absolute_playlist_path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK)};
    if (file.fd < 0)
        return std::unexpected(ioError("Cannot open playlist"));
    struct stat info{};
    if (::fstat(file.fd, &info) != 0)
        return std::unexpected(ioError("Cannot inspect playlist"));
    if (!S_ISREG(info.st_mode))
        return std::unexpected(
            error(core::ErrorCode::unsupported, "Playlist must be a regular file"));
    std::string bytes;
    std::array<char, 16U * 1024U> buffer{};
    for (;;) {
        if (cancellation.is_cancellation_requested())
            return std::unexpected(cancelled());
        const auto size = ::read(file.fd, buffer.data(), buffer.size());
        if (size < 0) {
            if (errno == EINTR)
                continue;
            return std::unexpected(ioError("Cannot read playlist"));
        }
        if (size == 0)
            break;
        if (bytes.size() + static_cast<std::size_t>(size) > m3u8_max_bytes)
            return std::unexpected(
                error(core::ErrorCode::limit_exceeded, "Playlist exceeds 32 MiB"));
        bytes.append(buffer.data(), static_cast<std::size_t>(size));
    }
    return parse_m3u8(bytes, absolute_playlist_path, cancellation, progress);
}

core::Result<std::string> serialize_m3u8(std::span<const PlaylistEntry> entries,
                                         const std::string& absolute_playlist_path,
                                         const core::CancellationToken& cancellation,
                                         std::atomic<std::size_t>* progress) {
    if (!absolutePath(absolute_playlist_path))
        return std::unexpected(
            error(core::ErrorCode::invalid_argument, "Playlist path must be absolute"));
    if (entries.size() > m3u8_max_entries)
        return std::unexpected(
            error(core::ErrorCode::limit_exceeded, "Playlist exceeds 100,000 entries"));
    if (cancellation.is_cancellation_requested())
        return std::unexpected(cancelled());
    std::string bytes{"#EXTM3U\n"};
    const auto base = directory(absolute_playlist_path);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (cancellation.is_cancellation_requested())
            return std::unexpected(cancelled());
        const auto& entry = entries[i];
        if (entry.logical_reference || entry.segment || entry.selection.stream_index ||
            entry.selection.subsong_index)
            return std::unexpected(error(core::ErrorCode::unsupported,
                                         "M3U8 cannot preserve chapters, subsongs, or explicit "
                                         "stream selections; export a list of whole files",
                                         i + 1));
        if (!absolutePath(entry.raw_path))
            return std::unexpected(error(core::ErrorCode::unsupported,
                                         "M3U8 export requires an absolute local file path",
                                         i + 1));
        if (entry.raw_path.size() > m3u8_max_line || entry.title.size() > m3u8_max_line)
            return std::unexpected(
                error(core::ErrorCode::limit_exceeded, "Playlist entry exceeds 64 KiB", i + 1));
        auto path =
            entry.raw_path.starts_with(base) ? entry.raw_path.substr(base.size()) : entry.raw_path;
        if (path.empty() || path.front() == '#' || path.front() == ' ' || path.front() == '\t' ||
            scheme(path) || path.find_first_of("\r\n") != std::string::npos ||
            !core::unicodeCodePointCount(path))
            path = fileUri(entry.raw_path);
        if (path.size() > m3u8_max_line)
            return std::unexpected(error(core::ErrorCode::limit_exceeded,
                                         "Encoded playlist path exceeds 64 KiB", i + 1));
        if (!entry.title.empty() || entry.duration_ms) {
            if (entry.title.find_first_of("\r\n\0", 0, 3) != std::string::npos ||
                !core::unicodeCodePointCount(entry.title))
                return std::unexpected(error(core::ErrorCode::unsupported,
                                             "Playlist title contains invalid UTF-8 or line breaks",
                                             i + 1));
            std::string duration{"-1"};
            if (entry.duration_ms && *entry.duration_ms >= 0 &&
                *entry.duration_ms <= 1'000'000'000'000'000LL) {
                duration = std::to_string(*entry.duration_ms / 1000);
                const auto fraction = *entry.duration_ms % 1000;
                if (fraction != 0)
                    duration += "." + std::to_string(1000 + fraction).substr(1);
            }
            const auto info = "#EXTINF:" + duration + "," + entry.title;
            if (info.size() > m3u8_max_line)
                return std::unexpected(
                    error(core::ErrorCode::limit_exceeded, "EXTINF line exceeds 64 KiB", i + 1));
            bytes += info + '\n';
        }
        bytes += path + '\n';
        if (bytes.size() > m3u8_max_bytes)
            return std::unexpected(
                error(core::ErrorCode::limit_exceeded, "Playlist exceeds 32 MiB"));
        if (progress)
            progress->store(i + 1, std::memory_order_relaxed);
    }
    return bytes;
}

core::Result<void> write_m3u8_new(const std::string& absolute_playlist_path, std::string_view bytes,
                                  const core::CancellationToken& cancellation) {
    if (cancellation.is_cancellation_requested())
        return std::unexpected(cancelled());
    if (!absolutePath(absolute_playlist_path) || bytes.size() > m3u8_max_bytes)
        return std::unexpected(error(core::ErrorCode::invalid_argument, "Invalid playlist output"));
    const auto base = directory(absolute_playlist_path);
    Descriptor dir{::open(base.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY)};
    if (dir.fd < 0)
        return std::unexpected(ioError("Cannot open output directory"));
    // Keep the parent descriptor anchored throughout publication, even if it is
    // renamed. Exclusive temporary names and linkat avoid check/replace races.
    static std::atomic<std::uint64_t> sequence{0};
    std::string temporary;
    Descriptor file;
    for (int attempt = 0; attempt < 100 && file.fd < 0; ++attempt) {
        temporary = ".trackbench-playlist-" + std::to_string(::getpid()) + "-" +
                    std::to_string(sequence.fetch_add(1));
        file.fd = ::openat(dir.fd, temporary.c_str(),
                           O_WRONLY | O_CLOEXEC | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (file.fd < 0 && errno != EEXIST)
            return std::unexpected(ioError("Cannot prepare playlist"));
    }
    if (file.fd < 0)
        return std::unexpected(ioError("Cannot reserve playlist temporary file"));
    struct Cleanup {
        int dir;
        const std::string& name;
        ~Cleanup() { ::unlinkat(dir, name.c_str(), 0); }
    } cleanup{dir.fd, temporary};
    while (!bytes.empty()) {
        if (cancellation.is_cancellation_requested())
            return std::unexpected(cancelled());
        const auto size =
            ::write(file.fd, bytes.data(), std::min(bytes.size(), std::size_t{16U * 1024U}));
        if (size < 0 && errno == EINTR)
            continue;
        if (size <= 0)
            return std::unexpected(ioError("Cannot write playlist"));
        bytes.remove_prefix(static_cast<std::size_t>(size));
    }
    if (::fsync(file.fd) != 0)
        return std::unexpected(ioError("Cannot sync playlist"));
    if (cancellation.is_cancellation_requested())
        return std::unexpected(cancelled());
    const auto target = absolute_playlist_path.substr(base.size());
    if (::linkat(dir.fd, temporary.c_str(), dir.fd, target.c_str(), 0) != 0) {
        if (errno == EEXIST)
            return std::unexpected(
                error(core::ErrorCode::conflict,
                      "Destination already exists; choose a new playlist filename"));
        return std::unexpected(ioError("Cannot publish playlist without replacing existing files"));
    }
    // Publication is complete. A directory-sync failure must not be reported as
    // an aborted operation; the complete destination is already visible.
    (void)::fsync(dir.fd);
    return {};
}
} // namespace trackknife::lists
