// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/formats/decoder.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::formats {

struct AudioStreamInfo {
    int stream_index{-1};
    std::string codec_name;
    std::string codec_description;
    std::string sample_format;
    std::string channel_layout;
    int sample_rate{0};
    int channels{0};
    std::int64_t bit_rate{0};

    friend bool operator==(const AudioStreamInfo&, const AudioStreamInfo&) = default;
};

struct ProbedTag {
    // Exactly as the demuxer reports it; no case folding or vocabulary
    // mapping happens at this boundary.
    std::string name;
    std::string value;

    friend bool operator==(const ProbedTag&, const ProbedTag&) = default;
};

struct ProbedChapter {
    // Stable only within the physical container. Consumers that persist a
    // logical identity also include the raw source and source_index.
    std::int64_t id{0};
    std::size_t source_index{0U};
    // Boundaries are in the selected audio stream's decoded sample domain and
    // share one origin, so adjacent chapters cannot drift independently.
    std::int64_t start_sample{0};
    std::int64_t end_sample{0};
    std::vector<ProbedTag> tags;

    friend bool operator==(const ProbedChapter&, const ProbedChapter&) = default;
};

struct ProbedSubsong {
    // The selector is persisted separately from the opaque logical identity
    // and is passed back to the decoder unchanged.
    AudioSourceSelection selection;
    std::size_t source_index{0U};
    std::string name;
    std::optional<std::int64_t> duration_ms;
    std::optional<std::int64_t> duration_samples;
    std::vector<ProbedTag> tags;

    friend bool operator==(const ProbedSubsong&, const ProbedSubsong&) = default;
};

struct MediaProbe {
    std::string raw_path;
    std::string container_names;
    std::string container_description;
    std::optional<std::int64_t> duration_ms;
    std::int64_t bit_rate{0};
    std::vector<AudioStreamInfo> audio_streams;
    std::optional<int> best_audio_stream;
    // Container-level metadata first, then the best audio stream's metadata,
    // each in demuxer order with repeated names preserved.
    std::vector<ProbedTag> tags;
    // Usable chapters projected against best_audio_stream. Projection is
    // all-or-nothing and must partition the known source duration exactly;
    // malformed, gapped, overlapping, or partial backend chapter tables leave
    // this empty so the physical file remains available as an ordinary row.
    std::vector<ProbedChapter> chapters;
    // Independently playable, non-chapter songs reported by a codec-aware
    // demuxer. Ordinary alternate language/commentary audio streams are never
    // expanded implicitly; they remain available only through an explicit
    // stream selector.
    std::vector<ProbedSubsong> subsongs;

    friend bool operator==(const MediaProbe&, const MediaProbe&) = default;
};

[[nodiscard]] core::Result<MediaProbe>
probe_local_media(const std::string& raw_path, const core::CancellationToken& cancellation = {});

} // namespace trackknife::formats
