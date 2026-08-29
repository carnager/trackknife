// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/error.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/progress.hpp"
#include "trackknife/core/revision.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/core/unicode.hpp"

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ranges>
#include <string_view>

namespace {

int failures = 0;

void check(const bool condition, const std::string_view expression, const int line) {
    if (!condition) {
        std::cerr << "line " << line << ": check failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

void stableIdsRoundTrip() {
    const auto id = trackknife::core::StableId::random();
    CHECK(!id.is_nil());
    const auto text = id.to_string();
    CHECK(text.size() == 36U);
    const auto parsed = trackknife::core::StableId::parse(text);
    CHECK(parsed.has_value());
    CHECK(parsed && *parsed == id);

    const auto known = trackknife::core::StableId::parse("00112233-4455-4677-8899-aabbccddeeff");
    CHECK(known.has_value());
    CHECK(known && known->to_string() == "00112233-4455-4677-8899-aabbccddeeff");
    CHECK(!trackknife::core::StableId::parse("not-an-id").has_value());
    CHECK(!trackknife::core::StableId::parse("00112233-4455-4677-8899-aabbccddeefg").has_value());
}

void cancellationIsSharedAndMonotonic() {
    trackknife::core::CancellationSource source;
    const auto first = source.token();
    const auto second = source.token();
    CHECK(!first.is_cancellation_requested());
    source.request_cancellation();
    CHECK(first.is_cancellation_requested());
    CHECK(second.is_cancellation_requested());
    CHECK(source.is_cancellation_requested());
}

void progressClampsItsFraction() {
    trackknife::core::Progress unknown{5, std::nullopt, "scan", "discovering"};
    CHECK(!unknown.fraction().has_value());
    trackknife::core::Progress halfway{5, 10, "scan", "decoding"};
    CHECK(halfway.fraction().has_value());
    CHECK(std::abs(*halfway.fraction() - 0.5) < 0.000001);
    trackknife::core::Progress overrun{11, 10, "scan", "finalizing"};
    CHECK(overrun.fraction() == 1.0);
}

void errorsRetainStructuredContext() {
    auto error = trackknife::core::Error{trackknife::core::ErrorCode::conflict,
                                         "source revision changed",
                                         {}}
                     .with_context("source_id", "abc")
                     .with_context("operation", "rename");
    CHECK(error.context.size() == 2U);
    CHECK(error.context.front().key == "source_id");
}

void unicodeSimpleCaseComparisonIsDeterministic() {
    const auto accented = trackknife::core::unicodeSimpleCaseEqual("Ärger", "äRGER");
    CHECK(accented && *accented);

    const auto no_full_fold = trackknife::core::unicodeSimpleCaseEqual("Straße", "STRASSE");
    CHECK(no_full_fold && !*no_full_fold);

    const auto no_normalization = trackknife::core::unicodeSimpleCaseEqual("é", "e\u0301");
    CHECK(no_normalization && !*no_normalization);

    const std::string_view invalid{"\xC3", 1};
    const auto rejected = trackknife::core::unicodeSimpleCaseEqual(invalid, invalid);
    CHECK(!rejected.has_value());
    CHECK(rejected.error().code == trackknife::core::ErrorCode::invalid_argument);

    const std::string invalid_after_mismatch{"x\xC3", 2};
    const auto trailing_invalid =
        trackknife::core::unicodeSimpleCaseEqual(invalid_after_mismatch, "y");
    CHECK(!trailing_invalid.has_value());
}

void unicodeCodePointIndexingUsesDecodedCharacters() {
    const std::string text = "AÄ🙂e\u0301";
    const auto count = trackknife::core::unicodeCodePointCount(text);
    CHECK(count && *count == 5U);

    const auto start = trackknife::core::unicodeByteOffset(text, 0U);
    const auto after_ascii = trackknife::core::unicodeByteOffset(text, 1U);
    const auto after_bmp = trackknife::core::unicodeByteOffset(text, 2U);
    const auto after_non_bmp = trackknife::core::unicodeByteOffset(text, 3U);
    const auto past_end = trackknife::core::unicodeByteOffset(text, 99U);
    CHECK(start && *start == 0U);
    CHECK(after_ascii && *after_ascii == 1U);
    CHECK(after_bmp && *after_bmp == 3U);
    CHECK(after_non_bmp && *after_non_bmp == 7U);
    CHECK(past_end && *past_end == text.size());

    const std::string invalid_after_boundary{"A\xC3", 2};
    CHECK(!trackknife::core::unicodeByteOffset(invalid_after_boundary, 1U).has_value());
    CHECK(!trackknife::core::unicodeCodePointCount(invalid_after_boundary).has_value());
}

void unicodeCaseTransformsAndEncodingAreValidated() {
    const auto lowered = trackknife::core::unicodeSimpleLower("ÄBC🙂");
    CHECK(lowered && *lowered == "äbc🙂");
    const auto uppered = trackknife::core::unicodeSimpleUpper("äbc🙂");
    CHECK(uppered && *uppered == "ÄBC🙂");

    const auto lozenge = trackknife::core::unicodeEncodeCodePoint(9674U);
    CHECK(lozenge && *lozenge == "◊");
    const auto non_bmp = trackknife::core::unicodeEncodeCodePoint(0x1F642U);
    CHECK(non_bmp && *non_bmp == "🙂");
    CHECK(!trackknife::core::unicodeEncodeCodePoint(0xD800U).has_value());
    CHECK(!trackknife::core::unicodeSimpleLower(std::string_view{"\xC3", 1}).has_value());
}

void localSourceDiscoveryPreservesRawPathsAndOrder() {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("trackknife-local-sources-" + trackknife::core::StableId::random().to_string());
    std::error_code error;
    CHECK(std::filesystem::create_directories(root / "nested", error));
    const auto first = root / "b.flac";
    const auto second = root / "nested" / "a.opus";
    const std::string invalid_name{"invalid-\xFF.wav", 13U};
    const auto invalid = root / std::filesystem::path{invalid_name};
    std::ofstream{first}.put('\0');
    std::ofstream{second}.put('\0');
    std::ofstream{invalid}.put('\0');

    const std::array inputs{root.native(), first.native()};
    const auto discovered = trackknife::core::discover_local_sources(inputs, {});
    CHECK(!discovered.cancelled);
    CHECK(!discovered.truncated);
    CHECK(discovered.issues.empty());
    CHECK(discovered.raw_files.size() == 4U);
    CHECK(discovered.raw_files.at(0) == first.native());
    CHECK(discovered.raw_files.at(1) == invalid.native());
    CHECK(discovered.raw_files.at(2) == second.native());
    CHECK(discovered.raw_files.back() == first.native());
    CHECK(std::ranges::count(discovered.raw_files, first.native()) == 2);
    CHECK(std::ranges::find(discovered.raw_files, invalid.native()) != discovered.raw_files.end());
    CHECK(trackknife::core::escape_raw_path(invalid_name) == "invalid-\\xFF.wav");

    trackknife::core::CancellationSource cancellation;
    cancellation.request_cancellation();
    const auto cancelled = trackknife::core::discover_local_sources(inputs, cancellation.token());
    CHECK(cancelled.cancelled);
    CHECK(cancelled.raw_files.empty());
    const auto limited = trackknife::core::discover_local_sources(inputs, {}, 2U);
    CHECK(limited.truncated);
    CHECK(limited.raw_files.size() == 2U);
    std::filesystem::remove_all(root, error);
    CHECK(!error);
}

} // namespace

int main() {
    stableIdsRoundTrip();
    cancellationIsSharedAndMonotonic();
    progressClampsItsFraction();
    errorsRetainStructuredContext();
    unicodeSimpleCaseComparisonIsDeterministic();
    unicodeCodePointIndexingUsesDecodedCharacters();
    unicodeCaseTransformsAndEncodingAreValidated();
    localSourceDiscoveryPreservesRawPathsAndOrder();
    return failures == 0 ? 0 : 1;
}
