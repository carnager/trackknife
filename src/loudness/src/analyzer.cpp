// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/loudness/analyzer.hpp"

#include "trackknife/formats/decoder.hpp"

#include <ebur128.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace trackknife::loudness {
namespace {

[[nodiscard]] core::Error analyzer_error(const core::ErrorCode code, std::string message) {
    return core::Error{.code = code, .message = std::move(message), .context = {}};
}

} // namespace

struct LoudnessAnalyzer::Impl {
    ebur128_state* state{nullptr};
    int channels{0};
    bool true_peak{false};

    ~Impl() {
        if (state != nullptr) {
            ebur128_destroy(&state);
        }
    }
};

LoudnessAnalyzer::LoudnessAnalyzer(std::unique_ptr<Impl> implementation)
    : implementation_{std::move(implementation)} {}
LoudnessAnalyzer::LoudnessAnalyzer(LoudnessAnalyzer&&) noexcept = default;
LoudnessAnalyzer& LoudnessAnalyzer::operator=(LoudnessAnalyzer&&) noexcept = default;
LoudnessAnalyzer::~LoudnessAnalyzer() = default;

core::Result<LoudnessAnalyzer> LoudnessAnalyzer::create(const int sample_rate, const int channels,
                                                        const bool measure_true_peak) {
    if (sample_rate <= 0 || channels <= 0) {
        return std::unexpected(
            analyzer_error(core::ErrorCode::invalid_argument,
                           "loudness analysis needs a positive sample rate and channel count"));
    }
    auto implementation = std::make_unique<Impl>();
    implementation->channels = channels;
    implementation->true_peak = measure_true_peak;
    auto mode = EBUR128_MODE_I | EBUR128_MODE_SAMPLE_PEAK;
    if (measure_true_peak) {
        mode |= EBUR128_MODE_TRUE_PEAK;
    }
    implementation->state =
        ebur128_init(static_cast<unsigned int>(channels), static_cast<unsigned long>(sample_rate),
                     static_cast<int>(mode));
    if (implementation->state == nullptr) {
        return std::unexpected(analyzer_error(
            core::ErrorCode::unsupported, "libebur128 rejected the sample rate or channel count"));
    }
    return LoudnessAnalyzer{std::move(implementation)};
}

core::Result<void> LoudnessAnalyzer::add_frames(const std::span<const float> interleaved_samples) {
    const auto channels = static_cast<std::size_t>(implementation_->channels);
    if (interleaved_samples.size() % channels != 0U) {
        return std::unexpected(
            analyzer_error(core::ErrorCode::invalid_argument,
                           "interleaved loudness input must contain whole frames"));
    }
    const auto frames = interleaved_samples.size() / channels;
    if (frames == 0U) {
        return {};
    }
    if (ebur128_add_frames_float(implementation_->state, interleaved_samples.data(), frames) !=
        EBUR128_SUCCESS) {
        return std::unexpected(
            analyzer_error(core::ErrorCode::backend, "libebur128 could not consume audio frames"));
    }
    return {};
}

core::Result<TrackLoudness> LoudnessAnalyzer::finish() const {
    TrackLoudness result{};
    if (ebur128_loudness_global(implementation_->state, &result.integrated_lufs) !=
        EBUR128_SUCCESS) {
        return std::unexpected(analyzer_error(core::ErrorCode::backend,
                                              "libebur128 could not compute integrated loudness"));
    }
    for (int channel = 0; channel < implementation_->channels; ++channel) {
        double peak = 0.0;
        if (ebur128_sample_peak(implementation_->state, static_cast<unsigned int>(channel),
                                &peak) != EBUR128_SUCCESS) {
            return std::unexpected(analyzer_error(core::ErrorCode::backend,
                                                  "libebur128 could not report the sample peak"));
        }
        result.sample_peak = std::max(result.sample_peak, peak);
    }
    if (implementation_->true_peak) {
        double maximum = 0.0;
        for (int channel = 0; channel < implementation_->channels; ++channel) {
            double peak = 0.0;
            if (ebur128_true_peak(implementation_->state, static_cast<unsigned int>(channel),
                                  &peak) != EBUR128_SUCCESS) {
                return std::unexpected(analyzer_error(core::ErrorCode::backend,
                                                      "libebur128 could not report the true peak"));
            }
            maximum = std::max(maximum, peak);
        }
        result.true_peak = maximum;
    }
    return result;
}

core::Result<double>
album_integrated_lufs(const std::span<const LoudnessAnalyzer* const> analyzers) {
    if (analyzers.empty()) {
        return std::unexpected(analyzer_error(core::ErrorCode::invalid_argument,
                                              "album loudness needs at least one analyzed track"));
    }
    std::vector<ebur128_state*> states;
    states.reserve(analyzers.size());
    for (const auto* analyzer : analyzers) {
        if (analyzer == nullptr || analyzer->implementation_ == nullptr) {
            return std::unexpected(analyzer_error(core::ErrorCode::invalid_argument,
                                                  "album loudness received an empty analyzer"));
        }
        states.push_back(analyzer->implementation_->state);
    }
    double integrated = 0.0;
    if (ebur128_loudness_global_multiple(states.data(), states.size(), &integrated) !=
        EBUR128_SUCCESS) {
        return std::unexpected(analyzer_error(core::ErrorCode::backend,
                                              "libebur128 could not compute album loudness"));
    }
    return integrated;
}

core::Result<TrackLoudness> analyze_decoded_source(formats::AudioDecoder& decoder,
                                                   const bool measure_true_peak,
                                                   const core::CancellationToken& cancellation) {
    const auto& format = decoder.output_format();
    auto analyzer =
        LoudnessAnalyzer::create(format.sample_rate, format.channels, measure_true_peak);
    if (!analyzer) {
        return std::unexpected(std::move(analyzer.error()));
    }
    while (true) {
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(
                analyzer_error(core::ErrorCode::cancelled, "loudness analysis was cancelled"));
        }
        auto chunk = decoder.next_chunk();
        if (!chunk) {
            return std::unexpected(std::move(chunk.error()));
        }
        if (!*chunk) {
            break;
        }
        auto added = analyzer->add_frames((*chunk)->interleaved_samples);
        if (!added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    return analyzer->finish();
}

} // namespace trackknife::loudness
