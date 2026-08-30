// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/formats/decoder.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::formats {

inline constexpr std::int64_t cue_frames_per_second = 75;

struct CueParseLimits {
    std::size_t source_bytes{1024U * 1024U};
    std::size_t lines{32'768U};
    std::size_t files{1'024U};
    std::size_t tracks{10'000U};
    std::size_t indexes_per_track{100U};
    std::size_t metadata_fields{10'000U};
};

struct CueMetadataField {
    std::string name;
    std::string value;

    friend bool operator==(const CueMetadataField&, const CueMetadataField&) = default;
};

struct CueMetadata {
    std::optional<std::string> title;
    std::optional<std::string> performer;
    std::optional<std::string> songwriter;
    std::vector<CueMetadataField> remarks;

    friend bool operator==(const CueMetadata&, const CueMetadata&) = default;
};

struct CueIndex {
    int number{0};
    std::int64_t cue_frame{0};

    friend bool operator==(const CueIndex&, const CueIndex&) = default;
};

struct CueTrack {
    int number{0};
    std::string mode;
    CueMetadata metadata;
    std::vector<CueIndex> indexes;
    std::vector<std::string> flags;
    std::optional<std::string> isrc;
    std::optional<std::int64_t> pregap_frames;
    std::optional<std::int64_t> postgap_frames;

    friend bool operator==(const CueTrack&, const CueTrack&) = default;
};

struct CueFile {
    // The exact path bytes between CUE syntax delimiters. Resolution against
    // the sheet directory belongs to the source-containment layer.
    std::string raw_reference;
    std::string type;
    std::vector<CueTrack> tracks;

    friend bool operator==(const CueFile&, const CueFile&) = default;
};

struct CueUnknownDirective {
    std::size_t line{0};
    std::string name;
    std::string argument;

    friend bool operator==(const CueUnknownDirective&, const CueUnknownDirective&) = default;
};

struct CueSheet {
    CueMetadata metadata;
    std::optional<std::string> catalog;
    std::optional<std::string> cd_text_file;
    std::vector<CueFile> files;
    std::vector<CueUnknownDirective> unknown_directives;

    friend bool operator==(const CueSheet&, const CueSheet&) = default;
};

struct CueLogicalTrack {
    std::size_t file_index{0};
    std::size_t track_index{0};
    int track_number{0};
    std::string raw_file_reference;
    std::string file_type;
    std::int64_t start_cue_frame{0};
    std::optional<std::int64_t> end_cue_frame;
    std::optional<std::string> title;
    std::optional<std::string> performer;
    std::optional<std::string> songwriter;
    std::optional<std::string> album_title;
    std::optional<std::string> album_performer;
    std::optional<std::string> isrc;
    // Sheet remarks first, then track remarks, retaining duplicates so a
    // consumer can apply an explicit first/last-value policy.
    std::vector<CueMetadataField> remarks;

    friend bool operator==(const CueLogicalTrack&, const CueLogicalTrack&) = default;
};

struct ResolvedCueTrack {
    CueLogicalTrack cue;
    std::string raw_source_path;
    SampleRange sample_range;
    std::optional<std::int64_t> duration_ms;

    friend bool operator==(const ResolvedCueTrack&, const ResolvedCueTrack&) = default;
};

struct ResolvedCueSheet {
    std::string raw_cue_path;
    std::vector<std::string> physical_sources;
    std::vector<ResolvedCueTrack> tracks;

    friend bool operator==(const ResolvedCueSheet&, const ResolvedCueSheet&) = default;
};

[[nodiscard]] core::Result<CueSheet> parse_cue_sheet(std::string_view source,
                                                     const CueParseLimits& limits = {});

[[nodiscard]] core::Result<std::vector<CueLogicalTrack>>
plan_cue_logical_tracks(const CueSheet& sheet);

[[nodiscard]] core::Result<SampleRange>
cue_track_sample_range(const CueLogicalTrack& track, int sample_rate,
                       std::optional<std::int64_t> physical_duration_samples = std::nullopt);

// Opens one external cue sheet, contains every referenced AUDIO source inside
// the resolved sheet directory, and maps its logical boundaries using each
// decoder's actual sample rate and duration. File and decode work belongs on a
// bounded worker, never the UI thread.
[[nodiscard]] core::Result<ResolvedCueSheet>
resolve_external_cue_sheet(std::string raw_cue_path,
                           const core::CancellationToken& cancellation = {},
                           const CueParseLimits& limits = {});

} // namespace trackknife::formats
