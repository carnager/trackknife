// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/operations/loudness_sidecar_apply.hpp"

#include "trackknife/core/error.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/formats/cue_sheet.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/metadata/loudness_sidecar.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace trackknife::operations {
namespace {

[[nodiscard]] core::Error sidecar_apply_error(const core::ErrorCode code, std::string message,
                                              const std::string& raw_audio_path) {
    return core::Error{
        .code = code,
        .message = std::move(message),
        .context = {{.key = "path", .value = core::escape_raw_path(raw_audio_path)}},
    };
}

[[nodiscard]] core::Error errno_error(std::string message, const std::string& raw_audio_path) {
    auto error = sidecar_apply_error(core::ErrorCode::io, std::move(message), raw_audio_path);
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

[[nodiscard]] std::optional<double>* entry_member(metadata::LoudnessSidecarEntry& entry,
                                                  const std::string& canonical_name) {
    if (canonical_name == "replaygaintrackgain") {
        return &entry.track_gain_db;
    }
    if (canonical_name == "replaygaintrackpeak") {
        return &entry.track_peak;
    }
    if (canonical_name == "replaygainalbumgain") {
        return &entry.album_gain_db;
    }
    return &entry.album_peak;
}

[[nodiscard]] metadata::LoudnessSidecarEntry
identity_entry(const metadata::StagedLogicalIdentity& identity) {
    return metadata::LoudnessSidecarEntry{
        .stream_index = identity.stream_index,
        .subsong_index = identity.subsong_index,
        .start_sample = identity.start_sample,
        .end_sample = identity.end_sample,
        .track_gain_db = std::nullopt,
        .track_peak = std::nullopt,
        .album_gain_db = std::nullopt,
        .album_peak = std::nullopt,
    };
}

} // namespace

core::Result<LoudnessSidecarCommitResult>
commit_loudness_sidecar(const metadata::MetadataWritePlanSidecar& sidecar_plan,
                        const core::CancellationToken& cancellation) {
    const auto& raw_audio_path = sidecar_plan.raw_audio_path;
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(sidecar_apply_error(
            core::ErrorCode::cancelled, "loudness sidecar apply was cancelled", raw_audio_path));
    }
    if (raw_audio_path.empty() || raw_audio_path.find('\0') != std::string::npos ||
        !sidecar_plan.ready() || !sidecar_plan.expected_revision ||
        !sidecar_plan.observed_revision ||
        *sidecar_plan.expected_revision != *sidecar_plan.observed_revision ||
        sidecar_plan.entries.empty()) {
        return std::unexpected(sidecar_apply_error(
            core::ErrorCode::invalid_argument,
            "loudness sidecar apply requires a ready revision-bound plan", raw_audio_path));
    }

    auto fresh_revision = core::observe_local_source_revision(raw_audio_path);
    if (!fresh_revision) {
        return std::unexpected(std::move(fresh_revision.error()));
    }
    if (*fresh_revision != *sidecar_plan.expected_revision) {
        return std::unexpected(sidecar_apply_error(
            core::ErrorCode::conflict, "the source changed after its loudness draft was captured",
            raw_audio_path));
    }

    auto existing = metadata::read_loudness_sidecar(raw_audio_path);
    if (!existing) {
        return std::unexpected(std::move(existing.error()));
    }
    metadata::LoudnessSidecar merged;
    const auto sidecar_existed = existing->has_value();
    if (*existing && (*existing)->matches(*sidecar_plan.expected_revision)) {
        merged = std::move(**existing);
    }
    // A stale or absent sidecar starts fresh; its old entries described
    // different audio bytes and must not survive the merge.
    merged.source_size = sidecar_plan.expected_revision->size;
    merged.source_modified_seconds = sidecar_plan.expected_revision->modification_time_seconds;
    merged.source_modified_nanoseconds =
        sidecar_plan.expected_revision->modification_time_nanoseconds;

    LoudnessSidecarCommitResult result{
        .raw_audio_path = raw_audio_path,
        .sidecar_raw_path = metadata::loudness_sidecar_path(raw_audio_path),
        .audio_revision = *sidecar_plan.expected_revision,
        .sidecar_removed = false,
        .entries = {},
    };

    for (const auto& planned : sidecar_plan.entries) {
        auto target = identity_entry(planned.identity);
        auto found = std::ranges::find_if(merged.entries,
                                          [&target](const metadata::LoudnessSidecarEntry& entry) {
                                              return entry.same_identity(target);
                                          });
        if (found == merged.entries.end()) {
            merged.entries.push_back(target);
            found = std::prev(merged.entries.end());
        }
        LoudnessSidecarAppliedEntry applied{
            .identity = planned.identity,
            .occurrence_indexes = planned.occurrence_indexes,
            .fields = {},
        };
        for (const auto& field : planned.fields) {
            auto* member = entry_member(*found, field.canonical_name);
            CueReplayGainAppliedField applied_field{
                .canonical_name = field.canonical_name,
                .display_name = std::string(display_replay_gain_name(field.canonical_name)),
                .value = std::nullopt,
            };
            if (field.kind == metadata::StagedMetadataPatchKind::remove_field) {
                member->reset();
            } else {
                if (field.values.size() != 1U) {
                    return std::unexpected(sidecar_apply_error(
                        core::ErrorCode::invariant,
                        "a planned sidecar replacement must carry exactly one value",
                        raw_audio_path));
                }
                const auto gain = is_gain_name(field.canonical_name);
                const auto parsed = gain ? formats::parse_replay_gain_decibels(field.values.front())
                                         : formats::parse_replay_gain_peak(field.values.front());
                const auto zero_peak =
                    !gain && !parsed &&
                    field.values.front().find_first_not_of("0. \t") == std::string::npos;
                if (!parsed && !zero_peak) {
                    return std::unexpected(sidecar_apply_error(
                        core::ErrorCode::invariant,
                        "a planned sidecar value stopped being parseable before commit",
                        raw_audio_path));
                }
                const auto value = parsed ? *parsed : 0.0;
                *member = value;
                applied_field.value = gain ? formats::replay_gain_decibel_text(value)
                                           : formats::replay_gain_peak_text(value);
            }
            applied.fields.push_back(std::move(applied_field));
        }
        if (found->empty()) {
            merged.entries.erase(found);
        }
        result.entries.push_back(std::move(applied));
    }

    if (merged.entries.empty()) {
        if (sidecar_existed && ::unlink(result.sidecar_raw_path.c_str()) != 0 && errno != ENOENT) {
            return std::unexpected(
                errno_error("the emptied loudness sidecar could not be removed", raw_audio_path));
        }
        result.sidecar_removed = true;
        return result;
    }

    auto serialized = metadata::serialize_loudness_sidecar(merged);
    if (!serialized) {
        return std::unexpected(std::move(serialized.error()));
    }
    const auto prepared_path =
        result.sidecar_raw_path + ".tk-prepared-" + core::StableId::random().to_string();
    FileDescriptor prepared;
    prepared.fd = ::open(prepared_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (prepared.fd < 0) {
        return std::unexpected(
            errno_error("the prepared loudness sidecar could not be created", raw_audio_path));
    }
    std::size_t written = 0U;
    while (written < serialized->size()) {
        const auto write_count =
            ::write(prepared.fd, serialized->data() + written, serialized->size() - written);
        if (write_count < 0) {
            if (errno == EINTR) {
                continue;
            }
            const auto failure =
                errno_error("the prepared loudness sidecar could not be written", raw_audio_path);
            ::unlink(prepared_path.c_str());
            return std::unexpected(failure);
        }
        written += static_cast<std::size_t>(write_count);
    }
    if (::fsync(prepared.fd) != 0) {
        const auto failure =
            errno_error("the prepared loudness sidecar could not be synced", raw_audio_path);
        ::unlink(prepared_path.c_str());
        return std::unexpected(failure);
    }
    if (std::rename(prepared_path.c_str(), result.sidecar_raw_path.c_str()) != 0) {
        const auto failure =
            errno_error("the loudness sidecar could not be published", raw_audio_path);
        ::unlink(prepared_path.c_str());
        return std::unexpected(failure);
    }
    const auto slash = result.sidecar_raw_path.find_last_of('/');
    const auto parent = slash == std::string::npos ? std::string{"."}
                                                   : result.sidecar_raw_path.substr(0U, slash + 1U);
    FileDescriptor directory;
    directory.fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory.fd >= 0) {
        static_cast<void>(::fsync(directory.fd));
    }
    return result;
}

} // namespace trackknife::operations
