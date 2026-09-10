// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/operations/cue_replay_gain_apply.hpp"

#include "trackknife/core/error.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/formats/cue_sheet.hpp"
#include "trackknife/formats/decoder.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace trackknife::operations {
namespace {

[[nodiscard]] core::Error cue_apply_error(const core::ErrorCode code, std::string message,
                                          const std::string& raw_cue_path) {
    return core::Error{
        .code = code,
        .message = std::move(message),
        .context = {{.key = "path", .value = core::escape_raw_path(raw_cue_path)}},
    };
}

[[nodiscard]] core::Error errno_error(std::string message, const std::string& raw_cue_path) {
    auto error = cue_apply_error(core::ErrorCode::io, std::move(message), raw_cue_path);
    error.context.push_back({.key = "errno", .value = std::to_string(errno)});
    return error;
}

struct FileDescriptor {
    int fd{-1};
    ~FileDescriptor() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

[[nodiscard]] std::string_view display_replay_gain_name(const std::string& canonical_name) {
    if (canonical_name == "replaygaintrackgain") {
        return "REPLAYGAIN_TRACK_GAIN";
    }
    if (canonical_name == "replaygaintrackpeak") {
        return "REPLAYGAIN_TRACK_PEAK";
    }
    if (canonical_name == "replaygainalbumgain") {
        return "REPLAYGAIN_ALBUM_GAIN";
    }
    return "REPLAYGAIN_ALBUM_PEAK";
}

[[nodiscard]] bool is_gain_name(const std::string& canonical_name) {
    return canonical_name == "replaygaintrackgain" || canonical_name == "replaygainalbumgain";
}

// Converts one planned field into the rewriter's tri-state form and the
// canonical applied text used for occurrence refresh.
[[nodiscard]] core::Result<std::pair<formats::CueReplayGainField, CueReplayGainAppliedField>>
convert_planned_field(const metadata::MetadataWritePlanLoudnessField& field,
                      const std::string& raw_cue_path) {
    CueReplayGainAppliedField applied{
        .canonical_name = field.canonical_name,
        .display_name = std::string(display_replay_gain_name(field.canonical_name)),
        .value = std::nullopt,
    };
    if (field.kind == metadata::StagedMetadataPatchKind::remove_field) {
        return std::pair{formats::CueReplayGainField{.update = true, .value = std::nullopt},
                         std::move(applied)};
    }
    if (field.values.size() != 1U) {
        return std::unexpected(cue_apply_error(
            core::ErrorCode::invariant,
            "a planned CUE ReplayGain replacement must carry exactly one value", raw_cue_path));
    }
    const auto gain = is_gain_name(field.canonical_name);
    const auto parsed = gain ? formats::parse_replay_gain_decibels(field.values.front())
                             : formats::parse_replay_gain_peak(field.values.front());
    // Peaks of exact digital silence are 0, which the lenient tag parser
    // rejects but the sheet writer accepts.
    const auto zero_peak =
        !gain && !parsed && field.values.front().find_first_not_of("0. \t") == std::string::npos;
    if (!parsed && !zero_peak) {
        return std::unexpected(cue_apply_error(
            core::ErrorCode::invariant,
            "a planned CUE ReplayGain value stopped being parseable before commit", raw_cue_path));
    }
    const auto value = parsed ? *parsed : 0.0;
    applied.value =
        gain ? formats::replay_gain_decibel_text(value) : formats::replay_gain_peak_text(value);
    return std::pair{formats::CueReplayGainField{.update = true, .value = value},
                     std::move(applied)};
}

} // namespace

core::Result<CueReplayGainCommitResult>
commit_cue_replay_gain_sheet(const metadata::MetadataWritePlanCueSheet& sheet_plan,
                             const core::CancellationToken& cancellation) {
    const auto& raw_cue_path = sheet_plan.raw_cue_path;
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cue_apply_error(core::ErrorCode::cancelled,
                                               "CUE ReplayGain apply was cancelled", raw_cue_path));
    }
    if (raw_cue_path.empty() || raw_cue_path.find('\0') != std::string::npos ||
        !sheet_plan.ready() || !sheet_plan.expected_revision || !sheet_plan.observed_revision ||
        *sheet_plan.expected_revision != *sheet_plan.observed_revision ||
        (sheet_plan.tracks.empty() && sheet_plan.album_fields.empty())) {
        return std::unexpected(cue_apply_error(
            core::ErrorCode::invalid_argument,
            "CUE ReplayGain apply requires a ready revision-bound sheet plan", raw_cue_path));
    }

    auto fresh_revision = core::observe_local_source_revision(raw_cue_path);
    if (!fresh_revision) {
        return std::unexpected(std::move(fresh_revision.error()));
    }
    if (*fresh_revision != *sheet_plan.expected_revision) {
        return std::unexpected(cue_apply_error(
            core::ErrorCode::conflict,
            "the CUE sheet changed after its ReplayGain draft was captured", raw_cue_path));
    }

    FileDescriptor source;
    source.fd = ::open(raw_cue_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (source.fd < 0) {
        return std::unexpected(errno_error("the CUE sheet could not be opened", raw_cue_path));
    }
    struct stat source_status{};
    if (::fstat(source.fd, &source_status) != 0 || !S_ISREG(source_status.st_mode)) {
        return std::unexpected(
            errno_error("the CUE sheet is not a readable regular file", raw_cue_path));
    }
    const formats::CueParseLimits limits{};
    if (static_cast<std::size_t>(source_status.st_size) > limits.source_bytes) {
        return std::unexpected(cue_apply_error(
            core::ErrorCode::limit_exceeded, "the CUE sheet exceeds the byte limit", raw_cue_path));
    }
    std::string bytes(static_cast<std::size_t>(source_status.st_size), '\0');
    std::size_t consumed = 0U;
    while (consumed < bytes.size()) {
        const auto read_count = ::read(source.fd, bytes.data() + consumed, bytes.size() - consumed);
        if (read_count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(errno_error("the CUE sheet could not be read", raw_cue_path));
        }
        if (read_count == 0) {
            return std::unexpected(cue_apply_error(
                core::ErrorCode::conflict, "the CUE sheet shrank while being read", raw_cue_path));
        }
        consumed += static_cast<std::size_t>(read_count);
    }

    CueReplayGainCommitResult result{
        .raw_cue_path = raw_cue_path,
        .previous_revision = *sheet_plan.expected_revision,
        .published_revision = {},
        .album_fields = {},
        .tracks = {},
    };
    formats::CueReplayGainUpdate update;
    for (const auto& field : sheet_plan.album_fields) {
        auto converted = convert_planned_field(field, raw_cue_path);
        if (!converted) {
            return std::unexpected(std::move(converted.error()));
        }
        (field.canonical_name == "replaygainalbumgain" ? update.album_gain_db : update.album_peak) =
            converted->first;
        result.album_fields.push_back(std::move(converted->second));
    }
    for (const auto& track : sheet_plan.tracks) {
        formats::CueTrackReplayGainUpdate track_update{
            .file_index = track.file_index,
            .track_index = track.track_index,
            .track_gain_db = {},
            .track_peak = {},
        };
        CueReplayGainAppliedTrack applied{
            .file_index = track.file_index,
            .track_index = track.track_index,
            .occurrence_indexes = track.occurrence_indexes,
            .fields = {},
        };
        for (const auto& field : track.fields) {
            auto converted = convert_planned_field(field, raw_cue_path);
            if (!converted) {
                return std::unexpected(std::move(converted.error()));
            }
            (field.canonical_name == "replaygaintrackgain" ? track_update.track_gain_db
                                                           : track_update.track_peak) =
                converted->first;
            applied.fields.push_back(std::move(converted->second));
        }
        update.tracks.push_back(track_update);
        result.tracks.push_back(std::move(applied));
    }

    auto rewritten = formats::rewrite_cue_replay_gain(bytes, update, limits);
    if (!rewritten) {
        return std::unexpected(std::move(rewritten.error()));
    }

    const auto prepared_path =
        raw_cue_path + ".tk-prepared-" + core::StableId::random().to_string();
    FileDescriptor prepared;
    prepared.fd = ::open(prepared_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                         source_status.st_mode & 07777);
    if (prepared.fd < 0) {
        return std::unexpected(
            errno_error("the prepared CUE copy could not be created", raw_cue_path));
    }
    std::size_t written = 0U;
    while (written < rewritten->bytes.size()) {
        const auto write_count = ::write(prepared.fd, rewritten->bytes.data() + written,
                                         rewritten->bytes.size() - written);
        if (write_count < 0) {
            if (errno == EINTR) {
                continue;
            }
            const auto failure =
                errno_error("the prepared CUE copy could not be written", raw_cue_path);
            ::unlink(prepared_path.c_str());
            return std::unexpected(failure);
        }
        written += static_cast<std::size_t>(write_count);
    }
    if (::fsync(prepared.fd) != 0) {
        const auto failure = errno_error("the prepared CUE copy could not be synced", raw_cue_path);
        ::unlink(prepared_path.c_str());
        return std::unexpected(failure);
    }

    // Re-verify immediately before the atomic replace: the window since the
    // read is the narrowest this unjournaled first slice can make it.
    auto final_revision = core::observe_local_source_revision(raw_cue_path);
    if (!final_revision || *final_revision != *sheet_plan.expected_revision) {
        ::unlink(prepared_path.c_str());
        return std::unexpected(cue_apply_error(core::ErrorCode::conflict,
                                               "the CUE sheet changed while its rewrite was "
                                               "being prepared",
                                               raw_cue_path));
    }
    if (std::rename(prepared_path.c_str(), raw_cue_path.c_str()) != 0) {
        const auto failure =
            errno_error("the rewritten CUE sheet could not be published", raw_cue_path);
        ::unlink(prepared_path.c_str());
        return std::unexpected(failure);
    }

    const auto slash = raw_cue_path.find_last_of('/');
    const auto parent =
        slash == std::string::npos ? std::string{"."} : raw_cue_path.substr(0U, slash + 1U);
    FileDescriptor directory;
    directory.fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory.fd >= 0) {
        static_cast<void>(::fsync(directory.fd));
    }

    auto published = core::observe_local_source_revision(raw_cue_path);
    if (!published) {
        return std::unexpected(std::move(published.error()));
    }
    result.published_revision = *published;
    return result;
}

} // namespace trackknife::operations
