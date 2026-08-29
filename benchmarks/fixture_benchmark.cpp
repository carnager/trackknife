// SPDX-License-Identifier: GPL-3.0-only

#include "synthetic_track_fixture.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Summary {
    double p50_microseconds;
    double p95_microseconds;
    double worst_microseconds;
    std::uint64_t checksum;
};

[[nodiscard]] Summary runFixture(const std::uint64_t size, const std::size_t samples) {
    const trackknife::benchmarks::SyntheticTrackFixture fixture(size);
    std::vector<double> timings;
    timings.reserve(samples);
    std::uint64_t checksum = 0;

    for (std::size_t sample = 0; sample < samples; ++sample) {
        const auto first = (static_cast<std::uint64_t>(sample) * 104'729U) % size;
        const auto started = Clock::now();
        for (std::uint64_t offset = 0; offset < 256U; ++offset) {
            const auto track = fixture.at((first + offset) % size);
            checksum += track.index + track.artist.size() + track.title.size() + track.album.size();
        }
        const auto elapsed = std::chrono::duration<double, std::micro>(Clock::now() - started);
        timings.push_back(elapsed.count());
    }

    std::ranges::sort(timings);
    const auto percentile = [&timings](const double fraction) {
        const auto position =
            static_cast<std::size_t>(fraction * static_cast<double>(timings.size() - 1U));
        return timings.at(position);
    };
    return {percentile(0.50), percentile(0.95), timings.back(), checksum};
}

} // namespace

int main(const int argc, char* argv[]) {
    const bool quick = argc > 1 && std::string_view{argv[1]} == "--quick";
    const std::size_t samples = quick ? 4U : 200U;
    constexpr std::array<std::uint64_t, 3> sizes{10'000U, 100'000U, 1'000'000U};

    std::cout << "generated fixture, 256-row page (microseconds)\n";
    std::cout << "tracks,p50,p95,worst,checksum\n";
    for (const auto size : sizes) {
        const auto summary = runFixture(size, samples);
        std::cout << size << ',' << std::fixed << std::setprecision(2) << summary.p50_microseconds
                  << ',' << summary.p95_microseconds << ',' << summary.worst_microseconds << ','
                  << summary.checksum << '\n';
    }
    return EXIT_SUCCESS;
}
