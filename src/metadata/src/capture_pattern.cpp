// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/capture_pattern.hpp"

#include "trackknife/core/unicode.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace trackknife::metadata {
namespace {

[[nodiscard]] core::Error capture_error(const core::ErrorCode code, std::string message,
                                        const std::size_t offset = 0U) {
    return core::Error{.code = code,
                       .message = std::move(message),
                       .context = {{.key = "offset", .value = std::to_string(offset)}}};
}

[[nodiscard]] std::size_t scalar_bytes(const unsigned char first) noexcept {
    if ((first & 0x80U) == 0U) {
        return 1U;
    }
    if ((first & 0xE0U) == 0xC0U) {
        return 2U;
    }
    if ((first & 0xF0U) == 0xE0U) {
        return 3U;
    }
    return 4U;
}

void append_literal(std::vector<CapturePatternToken>& tokens, std::string value) {
    if (value.empty()) {
        return;
    }
    if (!tokens.empty() && tokens.back().kind == CapturePatternTokenKind::literal) {
        tokens.back().text += value;
        return;
    }
    tokens.push_back(
        CapturePatternToken{.kind = CapturePatternTokenKind::literal, .text = std::move(value)});
}

[[nodiscard]] bool is_scalar_boundary(const std::string_view source,
                                      const std::size_t offset) noexcept {
    return offset == 0U || offset == source.size() ||
           (static_cast<unsigned char>(source[offset]) & 0xC0U) != 0x80U;
}

} // namespace

core::Result<CapturePatternProgram> compile_capture_pattern(std::string source,
                                                            CapturePatternDialectVersion dialect,
                                                            const CapturePatternLimits& limits) {
    if (dialect != CapturePatternDialectVersion{}) {
        return std::unexpected(
            capture_error(core::ErrorCode::invalid_argument,
                          "capture pattern uses an unsupported dialect or compiler schema"));
    }
    if (source.empty() || source.size() > limits.pattern_bytes) {
        return std::unexpected(
            capture_error(core::ErrorCode::invalid_argument,
                          "capture pattern requires bounded non-empty UTF-8 source"));
    }
    if (!core::unicodeCodePointCount(source)) {
        return std::unexpected(capture_error(core::ErrorCode::invalid_argument,
                                             "capture pattern must be valid UTF-8"));
    }

    CapturePatternProgram program{.dialect = std::move(dialect),
                                  .source = std::move(source),
                                  .tokens = {},
                                  .named_capture_count = 0U,
                                  .literal_path_separator_count = 0U};
    std::string literal;
    for (std::size_t offset = 0U; offset < program.source.size();) {
        const auto character = program.source[offset];
        if (character == '\\') {
            if (offset + 1U == program.source.size()) {
                return std::unexpected(capture_error(core::ErrorCode::invalid_argument,
                                                     "capture pattern has a trailing escape",
                                                     offset));
            }
            const auto quoted = offset + 1U;
            const auto bytes = scalar_bytes(static_cast<unsigned char>(program.source[quoted]));
            literal.append(program.source, quoted, bytes);
            if (program.source[quoted] == '/') {
                ++program.literal_path_separator_count;
            }
            offset = quoted + bytes;
            continue;
        }
        if (character != '%') {
            literal.push_back(character);
            if (character == '/') {
                ++program.literal_path_separator_count;
            }
            ++offset;
            continue;
        }

        append_literal(program.tokens, std::move(literal));
        literal.clear();
        if (offset + 1U < program.source.size() && program.source[offset + 1U] == '%') {
            program.tokens.push_back(
                CapturePatternToken{.kind = CapturePatternTokenKind::discard, .text = {}});
            offset += 2U;
        } else {
            const auto close = program.source.find('%', offset + 1U);
            if (close == std::string::npos) {
                return std::unexpected(capture_error(core::ErrorCode::invalid_argument,
                                                     "capture pattern has an unclosed field",
                                                     offset));
            }
            auto field = program.source.substr(offset + 1U, close - offset - 1U);
            if (field.empty()) {
                return std::unexpected(capture_error(core::ErrorCode::invalid_argument,
                                                     "capture pattern field cannot be empty",
                                                     offset));
            }
            program.tokens.push_back(CapturePatternToken{.kind = CapturePatternTokenKind::field,
                                                         .text = std::move(field)});
            ++program.named_capture_count;
            offset = close + 1U;
        }
        if (program.tokens.size() > limits.tokens ||
            program.named_capture_count > limits.captures) {
            return std::unexpected(capture_error(core::ErrorCode::limit_exceeded,
                                                 "capture pattern exceeds its token limit",
                                                 offset));
        }
    }
    append_literal(program.tokens, std::move(literal));
    if (program.tokens.size() > limits.tokens) {
        return std::unexpected(capture_error(core::ErrorCode::limit_exceeded,
                                             "capture pattern exceeds its token limit",
                                             program.source.size()));
    }
    if (program.named_capture_count == 0U) {
        return std::unexpected(capture_error(core::ErrorCode::invalid_argument,
                                             "capture pattern requires a named field capture"));
    }
    return program;
}

core::Result<CapturePatternMatch> match_capture_pattern(const CapturePatternProgram& program,
                                                        const std::string_view source,
                                                        const CapturePatternLimits& limits) {
    if (program.dialect != CapturePatternDialectVersion{} || program.tokens.empty() ||
        program.tokens.size() > limits.tokens || program.named_capture_count == 0U ||
        program.named_capture_count > limits.captures ||
        program.source.size() > limits.pattern_bytes) {
        return std::unexpected(
            capture_error(core::ErrorCode::invalid_argument, "capture pattern program is invalid"));
    }
    if (source.size() > limits.source_bytes) {
        return std::unexpected(capture_error(core::ErrorCode::limit_exceeded,
                                             "capture source exceeds its byte limit"));
    }
    if (!core::unicodeCodePointCount(source)) {
        return std::unexpected(
            capture_error(core::ErrorCode::invalid_argument, "capture source must be valid UTF-8"));
    }

    std::vector<CapturedMetadataValue> current;
    current.reserve(program.named_capture_count);
    std::vector<CapturedMetadataValue> first;
    std::size_t solutions = 0U;
    std::size_t steps = 0U;
    bool limit_exceeded = false;

    const std::function<void(std::size_t, std::size_t)> visit =
        [&](const std::size_t token_index, const std::size_t source_offset) {
            if (solutions >= 2U || limit_exceeded) {
                return;
            }
            if (++steps > limits.match_steps) {
                limit_exceeded = true;
                return;
            }
            if (token_index == program.tokens.size()) {
                if (source_offset == source.size()) {
                    ++solutions;
                    if (solutions == 1U) {
                        first = current;
                    }
                }
                return;
            }
            const auto& token = program.tokens[token_index];
            if (token.kind == CapturePatternTokenKind::literal) {
                if (source.substr(source_offset).starts_with(token.text)) {
                    visit(token_index + 1U, source_offset + token.text.size());
                }
                return;
            }

            if (token_index + 1U == program.tokens.size()) {
                if (token.kind == CapturePatternTokenKind::field) {
                    current.push_back(CapturedMetadataValue{
                        .field = token.text,
                        .value = std::string{source.substr(source_offset)},
                    });
                }
                visit(token_index + 1U, source.size());
                if (token.kind == CapturePatternTokenKind::field) {
                    current.pop_back();
                }
                return;
            }

            for (std::size_t end = source_offset; end <= source.size(); ++end) {
                if (!is_scalar_boundary(source, end)) {
                    continue;
                }
                if (token.kind == CapturePatternTokenKind::field) {
                    current.push_back(CapturedMetadataValue{
                        .field = token.text,
                        .value = std::string{source.substr(source_offset, end - source_offset)},
                    });
                }
                visit(token_index + 1U, end);
                if (token.kind == CapturePatternTokenKind::field) {
                    current.pop_back();
                }
                if (solutions >= 2U || limit_exceeded) {
                    return;
                }
            }
        };
    visit(0U, 0U);

    if (limit_exceeded) {
        return std::unexpected(capture_error(core::ErrorCode::limit_exceeded,
                                             "capture pattern match work limit was exceeded"));
    }
    if (solutions == 0U) {
        return CapturePatternMatch{.kind = CapturePatternMatchKind::unmatched, .values = {}};
    }
    if (solutions > 1U) {
        return CapturePatternMatch{.kind = CapturePatternMatchKind::ambiguous, .values = {}};
    }
    return CapturePatternMatch{.kind = CapturePatternMatchKind::unique, .values = std::move(first)};
}

} // namespace trackknife::metadata
