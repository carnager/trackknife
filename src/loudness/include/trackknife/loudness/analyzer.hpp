// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace trackknife::formats {
class AudioDecoder;
}

namespace trackknife::loudness {

// ReplayGain 2.0 normalizes to -18 LUFS: the EBU R128 -23 LUFS reference
// raised by the 5 dB the ReplayGain 2.0 specification mandates.
inline constexpr double replaygain_reference_lufs = -18.0;

struct TrackLoudness {
    // EBU R128 gated integrated programme loudness.
    double integrated_lufs{0.0};
    // Largest absolute decoded sample, linear (1.0 == full scale).
    double sample_peak{0.0};
    // Oversampled inter-sample peak, linear, when measurement was enabled.
    std::optional<double> true_peak;

    [[nodiscard]] double track_gain_db() const noexcept {
        return replaygain_reference_lufs - integrated_lufs;
    }

    // Material shorter than one 400 ms gating block has no gated integrated
    // loudness (negative infinity); callers must not derive gains from it.
    [[nodiscard]] bool measurable() const noexcept {
        return integrated_lufs > -70.0 && integrated_lufs < 70.0;
    }

    friend bool operator==(const TrackLoudness&, const TrackLoudness&) = default;
};

// Streaming EBU R128 analysis over interleaved float PCM (libebur128).
// Analysis always runs at the source's native sample rate — the library
// derives its K-weighting filter per rate, so high-rate material is never
// resampled for measurement. The analyzer's state stays valid after
// finish() so album reduction can combine several tracks.
class LoudnessAnalyzer final {
  public:
    LoudnessAnalyzer(LoudnessAnalyzer&&) noexcept;
    LoudnessAnalyzer& operator=(LoudnessAnalyzer&&) noexcept;
    LoudnessAnalyzer(const LoudnessAnalyzer&) = delete;
    LoudnessAnalyzer& operator=(const LoudnessAnalyzer&) = delete;
    ~LoudnessAnalyzer();

    [[nodiscard]] static core::Result<LoudnessAnalyzer> create(int sample_rate, int channels,
                                                               bool measure_true_peak);

    // Adds interleaved frames; the span size must be a whole number of
    // frames for the configured channel count.
    [[nodiscard]] core::Result<void> add_frames(std::span<const float> interleaved_samples);

    [[nodiscard]] core::Result<TrackLoudness> finish() const;

  private:
    struct Impl;
    explicit LoudnessAnalyzer(std::unique_ptr<Impl> implementation);
    friend core::Result<double>
    album_integrated_lufs(std::span<const LoudnessAnalyzer* const> analyzers);

    std::unique_ptr<Impl> implementation_;
};

// EBU R128 album programme loudness across the tracks' retained analyzer
// states — the gated loudness of the whole programme, never an average of
// per-track values. The album gain is replaygain_reference_lufs minus this.
[[nodiscard]] core::Result<double>
album_integrated_lufs(std::span<const LoudnessAnalyzer* const> analyzers);

// Convenience: decodes one opened source to completion and analyzes it.
[[nodiscard]] core::Result<TrackLoudness>
analyze_decoded_source(formats::AudioDecoder& decoder, bool measure_true_peak,
                       const core::CancellationToken& cancellation = {});

} // namespace trackknife::loudness
