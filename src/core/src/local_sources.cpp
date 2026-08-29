// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/core/local_sources.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace trackknife::core {
namespace {

[[nodiscard]] bool raw_less(const std::string& left, const std::string& right) {
    return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
                                        [](const char first, const char second) {
                                            return static_cast<unsigned char>(first) <
                                                   static_cast<unsigned char>(second);
                                        });
}

[[nodiscard]] LocalSourceIssue issue(std::string raw_path, const ErrorCode code,
                                     std::string message) {
    return LocalSourceIssue{
        .raw_path = std::move(raw_path),
        .error = Error{.code = code, .message = std::move(message), .context = {}},
    };
}

[[nodiscard]] std::string native_bytes(const std::filesystem::path& path) { return path.native(); }

} // namespace

LocalSourceDiscovery discover_local_sources(const std::span<const std::string> raw_paths,
                                            const CancellationToken& cancellation,
                                            const std::size_t file_limit) {
    LocalSourceDiscovery result;
    if (file_limit == 0U) {
        result.truncated = true;
        result.issues.push_back(
            issue({}, ErrorCode::limit_exceeded, "local source discovery file limit is zero"));
        return result;
    }

    for (const auto& raw_path : raw_paths) {
        if (cancellation.is_cancellation_requested()) {
            result.cancelled = true;
            break;
        }
        if (raw_path.empty()) {
            result.issues.push_back(
                issue(raw_path, ErrorCode::invalid_argument, "local source path is empty"));
            continue;
        }

        std::vector<std::filesystem::path> pending{std::filesystem::path{raw_path}};
        while (!pending.empty()) {
            if (cancellation.is_cancellation_requested()) {
                result.cancelled = true;
                break;
            }
            auto path = std::move(pending.back());
            pending.pop_back();
            const auto path_bytes = native_bytes(path);
            std::error_code error;
            const auto lexical_status = std::filesystem::symlink_status(path, error);
            ++result.visited_entries;
            if (error) {
                result.issues.push_back(issue(path_bytes, ErrorCode::io, error.message()));
                continue;
            }

            auto status = lexical_status;
            if (std::filesystem::is_symlink(lexical_status)) {
                status = std::filesystem::status(path, error);
                if (error) {
                    result.issues.push_back(issue(path_bytes, ErrorCode::io, error.message()));
                    continue;
                }
                if (std::filesystem::is_directory(status)) {
                    result.issues.push_back(issue(path_bytes, ErrorCode::unsupported,
                                                  "directory symlink is not followed"));
                    continue;
                }
            }

            if (std::filesystem::is_regular_file(status)) {
                if (result.raw_files.size() == file_limit) {
                    result.truncated = true;
                    result.issues.push_back(issue(raw_path, ErrorCode::limit_exceeded,
                                                  "local source discovery reached its file limit"));
                    break;
                }
                result.raw_files.push_back(path_bytes);
                continue;
            }
            if (!std::filesystem::is_directory(status)) {
                result.issues.push_back(issue(path_bytes, ErrorCode::unsupported,
                                              "path is not a regular file or directory"));
                continue;
            }

            std::vector<std::filesystem::path> children;
            std::filesystem::directory_iterator iterator{
                path, std::filesystem::directory_options::skip_permission_denied, error};
            const std::filesystem::directory_iterator end;
            if (error) {
                result.issues.push_back(issue(path_bytes, ErrorCode::io, error.message()));
                continue;
            }
            while (iterator != end) {
                children.push_back(iterator->path());
                iterator.increment(error);
                if (error) {
                    result.issues.push_back(issue(path_bytes, ErrorCode::io, error.message()));
                    error.clear();
                }
            }
            std::ranges::sort(children, [](const auto& left, const auto& right) {
                return raw_less(native_bytes(left), native_bytes(right));
            });
            pending.insert(pending.end(), std::make_move_iterator(children.rbegin()),
                           std::make_move_iterator(children.rend()));
        }
        if (result.cancelled) {
            break;
        }
        if (result.truncated) {
            break;
        }
    }
    return result;
}

std::string escape_raw_path(const std::string_view raw_path) {
    constexpr std::string_view digits{"0123456789ABCDEF"};
    std::string escaped;
    escaped.reserve(raw_path.size());
    for (const auto byte : raw_path) {
        const auto value = static_cast<unsigned char>(byte);
        if (value >= 0x20U && value <= 0x7EU && value != static_cast<unsigned char>('\\')) {
            escaped.push_back(static_cast<char>(value));
        } else if (value == static_cast<unsigned char>('\\')) {
            escaped += "\\\\";
        } else {
            escaped += "\\x";
            escaped.push_back(digits.at(value >> 4U));
            escaped.push_back(digits.at(value & 0x0FU));
        }
    }
    return escaped;
}

} // namespace trackknife::core
