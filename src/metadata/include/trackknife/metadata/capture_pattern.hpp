// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::metadata {

struct CapturePatternDialectVersion {
    std::string dialect{"tkcapture"};
    std::uint32_t dialect_version{1U};
    std::uint32_t compiler_schema{1U};

    friend bool operator==(const CapturePatternDialectVersion&,
                           const CapturePatternDialectVersion&) = default;
};

enum class CapturePatternTokenKind : std::uint8_t {
    literal,
    field,
    discard,
};

struct CapturePatternToken {
    CapturePatternTokenKind kind{CapturePatternTokenKind::literal};
    std::string text;

    friend bool operator==(const CapturePatternToken&, const CapturePatternToken&) = default;
};

struct CapturePatternProgram {
    CapturePatternDialectVersion dialect;
    std::string source;
    std::vector<CapturePatternToken> tokens;
    std::size_t named_capture_count{0U};
    std::size_t literal_path_separator_count{0U};

    friend bool operator==(const CapturePatternProgram&, const CapturePatternProgram&) = default;
};

struct CapturePatternLimits {
    std::size_t pattern_bytes{1U * 1'024U * 1'024U};
    std::size_t source_bytes{1U * 1'024U * 1'024U};
    std::size_t tokens{256U};
    std::size_t captures{256U};
    std::size_t match_steps{4'000'000U};
};

struct CapturedMetadataValue {
    std::string field;
    std::string value;

    friend bool operator==(const CapturedMetadataValue&, const CapturedMetadataValue&) = default;
};

enum class CapturePatternMatchKind : std::uint8_t {
    unmatched,
    unique,
    ambiguous,
};

struct CapturePatternMatch {
    CapturePatternMatchKind kind{CapturePatternMatchKind::unmatched};
    std::vector<CapturedMetadataValue> values;

    friend bool operator==(const CapturePatternMatch&, const CapturePatternMatch&) = default;
};

[[nodiscard]] core::Result<CapturePatternProgram>
compile_capture_pattern(std::string source, CapturePatternDialectVersion dialect = {},
                        const CapturePatternLimits& limits = {});

[[nodiscard]] core::Result<CapturePatternMatch>
match_capture_pattern(const CapturePatternProgram& program, std::string_view source,
                      const CapturePatternLimits& limits = {});

} // namespace trackknife::metadata
