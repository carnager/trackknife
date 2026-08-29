// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/titleformat/compiler.hpp"
#include "trackknife/titleformat/evaluator.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

class SyntheticContext final : public trackknife::titleformat::EvaluationContext {
  public:
    void setIndex(const std::uint64_t index) noexcept { index_ = index; }

    [[nodiscard]] trackknife::titleformat::FormatContextKind kind() const noexcept override {
        return trackknife::titleformat::FormatContextKind::track_display;
    }

    [[nodiscard]] std::optional<std::string>
    resolveField(const std::string_view name) const override {
        if (name == "artist") {
            return "Artist " + std::to_string((index_ / 11U) % 8'192U);
        }
        if (name == "title") {
            return "Synthetic track " + std::to_string(index_ + 1U);
        }
        if (name == "album") {
            return "Album " + std::to_string(index_ / 11U + 1U);
        }
        return std::nullopt;
    }

  private:
    std::uint64_t index_{0};
};

struct Summary {
    double elapsed_ms;
    double tracks_per_second;
    double page_p50_microseconds;
    double page_p95_microseconds;
    double page_worst_microseconds;
    std::uint64_t checksum;
};

[[nodiscard]] std::optional<Summary> run(const trackknife::titleformat::Program& program,
                                         const std::uint64_t track_count) {
    constexpr std::uint64_t page_size = 256U;
    SyntheticContext context;
    std::vector<double> page_timings;
    page_timings.reserve(static_cast<std::size_t>((track_count + page_size - 1U) / page_size));
    std::uint64_t checksum = 0;
    const auto all_started = Clock::now();

    for (std::uint64_t first = 0; first < track_count; first += page_size) {
        const auto page_started = Clock::now();
        const auto end = std::min(first + page_size, track_count);
        for (auto index = first; index < end; ++index) {
            context.setIndex(index);
            const auto result = trackknife::titleformat::evaluate(program, context);
            if (!result) {
                std::cerr << "evaluation failed at track " << index << ": "
                          << result.error().message << '\n';
                return std::nullopt;
            }
            checksum += result->text.size();
        }
        page_timings.push_back(
            std::chrono::duration<double, std::micro>(Clock::now() - page_started).count());
    }

    const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - all_started);
    std::ranges::sort(page_timings);
    const auto percentile = [&page_timings](const double fraction) {
        const auto position =
            static_cast<std::size_t>(fraction * static_cast<double>(page_timings.size() - 1U));
        return page_timings.at(position);
    };
    return Summary{
        .elapsed_ms = elapsed.count(),
        .tracks_per_second = static_cast<double>(track_count) * 1'000.0 / elapsed.count(),
        .page_p50_microseconds = percentile(0.50),
        .page_p95_microseconds = percentile(0.95),
        .page_worst_microseconds = page_timings.back(),
        .checksum = checksum,
    };
}

} // namespace

int main(const int argc, char* argv[]) {
    const bool quick = argc > 1 && std::string_view{argv[1]} == "--quick";
    const auto compiled = trackknife::titleformat::compile(
        "$if(%artist%,%artist% - ,)%title%$if(%album%, - %album%,)");
    if (!compiled.program) {
        std::cerr << "benchmark script failed to compile\n";
        return EXIT_FAILURE;
    }

    constexpr std::array<std::uint64_t, 3> full_sizes{10'000U, 100'000U, 1'000'000U};
    constexpr std::array<std::uint64_t, 1> quick_sizes{1'000U};
    std::cout
        << "tracks,elapsed_ms,tracks_per_second,page_p50_us,page_p95_us,page_worst_us,checksum\n";
    const auto report = [&program = *compiled.program](const std::uint64_t size) {
        const auto summary = run(program, size);
        if (!summary) {
            return false;
        }
        std::cout << size << ',' << std::fixed << std::setprecision(2) << summary->elapsed_ms << ','
                  << summary->tracks_per_second << ',' << summary->page_p50_microseconds << ','
                  << summary->page_p95_microseconds << ',' << summary->page_worst_microseconds
                  << ',' << summary->checksum << '\n';
        return true;
    };
    if (quick) {
        for (const auto size : quick_sizes) {
            if (!report(size)) {
                return EXIT_FAILURE;
            }
        }
    } else {
        for (const auto size : full_sizes) {
            if (!report(size)) {
                return EXIT_FAILURE;
            }
        }
    }
    return EXIT_SUCCESS;
}
