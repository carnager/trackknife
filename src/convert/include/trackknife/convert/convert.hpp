// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/convert/preset.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/metadata/document.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace trackknife::convert {

// One source-to-destination conversion. The selection and range address
// logical tracks inside container files exactly as the decoder does, so
// cue-sheet segments convert like whole files.
struct AudioConversionRequest {
    std::string source_raw_path;
    formats::AudioSourceSelection source_selection;
    std::optional<formats::SampleRange> source_range;
    std::string destination_raw_path;
    EncoderPreset preset;
    // Forces the output sample rate; absent keeps the source rate. Either
    // way the encoder's supported-rate constraint applies afterwards, so
    // Opus maps any request into its 48 kHz family.
    std::optional<int> target_sample_rate;
    // Effective text metadata to carry into the output, written at mux time
    // and verified by rereading the finished file with the project metadata
    // reader before it may become the destination. Vorbis-comment containers
    // receive exact native key spellings; MP3 maps the common fields onto
    // proper ID3 frames and passes the rest through as TXXX.
    metadata::MetadataDocument metadata;

    friend bool operator==(const AudioConversionRequest&, const AudioConversionRequest&) = default;
};

struct ConvertedAudioFile {
    std::string destination_raw_path;
    int sample_rate{0};
    int channels{0};
    // Verified duration of the written output, in output-rate samples.
    std::int64_t duration_samples{0};

    friend bool operator==(const ConvertedAudioFile&, const ConvertedAudioFile&) = default;
};

// Progress in decoded source frames; the total is absent when the source
// container does not declare a duration.
using ConversionProgress =
    std::function<void(std::uint64_t frames_done, std::optional<std::uint64_t> frames_total)>;

// Decodes one source and encodes it with the preset, atomically: the
// encoder writes a hidden temporary beside the destination, the result is
// verified by reopening it with the project decoder (format and duration),
// and only then is it renamed into place without replacing an existing
// file. Failure or cancellation leaves no partial output. This synchronous
// I/O primitive belongs on a bounded worker; it never touches the source.
[[nodiscard]] core::Result<ConvertedAudioFile>
convert_audio_file(const AudioConversionRequest& request, const ConversionProgress& progress = {},
                   const core::CancellationToken& cancellation = {});

} // namespace trackknife::convert
