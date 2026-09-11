// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/result.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::metadata {

// ADR-0141: one loudness record per logical source inside the audio
// file. A whole-file entry carries neither selection nor segment.
struct LoudnessSidecarEntry {
    std::optional<int> stream_index;
    std::optional<int> subsong_index;
    std::optional<std::int64_t> start_sample;
    std::optional<std::int64_t> end_sample;
    std::optional<double> track_gain_db;
    std::optional<double> track_peak;
    std::optional<double> album_gain_db;
    std::optional<double> album_peak;
    // ADR-0148: true when the peak values are oversampled true peaks
    // rather than sample peaks. Serialized only when set, so sample-peak
    // sidecars keep their existing bytes.
    bool true_peak{false};

    [[nodiscard]] bool same_identity(const LoudnessSidecarEntry& other) const noexcept {
        return stream_index == other.stream_index && subsong_index == other.subsong_index &&
               start_sample == other.start_sample && end_sample == other.end_sample;
    }
    [[nodiscard]] bool empty() const noexcept {
        return !track_gain_db && !track_peak && !album_gain_db && !album_peak;
    }

    friend bool operator==(const LoudnessSidecarEntry&, const LoudnessSidecarEntry&) = default;
};

struct LoudnessSidecar {
    // The audio file's identity at write time. Deliberately without
    // device/inode so the sidecar survives timestamp-preserving copies;
    // a mismatch marks every entry stale.
    std::uint64_t source_size{0U};
    std::int64_t source_modified_seconds{0};
    std::int64_t source_modified_nanoseconds{0};
    double reference_lufs{-18.0};
    std::vector<LoudnessSidecarEntry> entries;

    [[nodiscard]] bool matches(const core::LocalSourceRevision& revision) const noexcept {
        return source_size == revision.size &&
               source_modified_seconds == revision.modification_time_seconds &&
               source_modified_nanoseconds == revision.modification_time_nanoseconds;
    }

    friend bool operator==(const LoudnessSidecar&, const LoudnessSidecar&) = default;
};

struct LoudnessSidecarLimits {
    std::size_t source_bytes{64U * 1024U};
    std::size_t entries{4'096U};
};

// The sidecar lives directly beside its audio file: <name>.tkmeta.
[[nodiscard]] std::string loudness_sidecar_path(const std::string& raw_audio_path);

// Strict, fail-closed parsing of the versioned tkmeta document: unknown
// keys, non-numeric values, duplicate identities, and unsupported
// versions are errors, never silently ignored.
[[nodiscard]] core::Result<LoudnessSidecar>
parse_loudness_sidecar(std::string_view source, const LoudnessSidecarLimits& limits = {});

[[nodiscard]] core::Result<std::string> serialize_loudness_sidecar(const LoudnessSidecar& sidecar);

// Reads <raw_audio_path>.tkmeta. An absent sidecar is a successful
// nullopt; an unreadable or unparseable one is an error so callers
// never mistake corruption for absence.
[[nodiscard]] core::Result<std::optional<LoudnessSidecar>>
read_loudness_sidecar(const std::string& raw_audio_path, const LoudnessSidecarLimits& limits = {});

} // namespace trackknife::metadata
