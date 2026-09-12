// SPDX-License-Identifier: GPL-3.0-only

#include "tkq_internal.hpp"

#include "trackknife/core/error.hpp"

#include <array>
#include <string_view>

namespace trackknife::query::internal {
namespace {

[[nodiscard]] core::Error lex_error(std::string message, const std::size_t begin,
                                    const std::size_t end) {
    return core::Error{
        .code = core::ErrorCode::invalid_argument,
        .message = std::move(message),
        .context = {{.key = "span_begin", .value = std::to_string(begin)},
                    {.key = "span_end", .value = std::to_string(end)}},
    };
}

[[nodiscard]] bool whitespace(const char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r';
}

} // namespace

bool is_reserved_keyword(const std::string_view word) {
    constexpr std::array keywords{"ALL",     "AND",     "OR",        "NOT",        "HAS",
                                  "IS",      "GREATER", "LESS",      "EQUAL",      "PRESENT",
                                  "MISSING", "SORT",    "ASCENDING", "DESCENDING", "BY"};
    for (const auto* keyword : keywords) {
        if (word == keyword) {
            return true;
        }
    }
    return false;
}

core::Result<std::vector<Token>> lex_tkq(const std::string_view source,
                                         const std::size_t maximum_source_bytes) {
    if (source.size() > maximum_source_bytes) {
        return std::unexpected(
            lex_error("the query is longer than " + std::to_string(maximum_source_bytes) + " bytes",
                      0U, source.size()));
    }
    std::vector<Token> tokens;
    std::size_t offset = 0U;
    while (offset < source.size()) {
        const auto character = source[offset];
        if (whitespace(character)) {
            ++offset;
            continue;
        }
        if (character == '(') {
            tokens.push_back({TokenKind::open_paren, "(", offset, offset + 1U, false});
            ++offset;
            continue;
        }
        if (character == ')') {
            tokens.push_back({TokenKind::close_paren, ")", offset, offset + 1U, false});
            ++offset;
            continue;
        }
        if (character == '*') {
            tokens.push_back({TokenKind::star, "*", offset, offset + 1U, false});
            ++offset;
            continue;
        }
        if (character == '"') {
            const auto begin = offset;
            ++offset;
            std::string text;
            bool terminated = false;
            while (offset < source.size()) {
                if (source[offset] == '"') {
                    // A doubled quote is a literal quote inside the string.
                    if (offset + 1U < source.size() && source[offset + 1U] == '"') {
                        text.push_back('"');
                        offset += 2U;
                        continue;
                    }
                    ++offset;
                    terminated = true;
                    break;
                }
                text.push_back(source[offset]);
                ++offset;
            }
            if (!terminated) {
                return std::unexpected(lex_error("the quoted string never closes", begin, offset));
            }
            tokens.push_back({TokenKind::quoted, std::move(text), begin, offset, false});
            continue;
        }
        const auto begin = offset;
        std::string text;
        while (offset < source.size() && !whitespace(source[offset]) && source[offset] != '(' &&
               source[offset] != ')' && source[offset] != '"') {
            text.push_back(source[offset]);
            ++offset;
        }
        const auto keyword = is_reserved_keyword(text);
        tokens.push_back({TokenKind::word, std::move(text), begin, offset, keyword});
    }
    return tokens;
}

} // namespace trackknife::query::internal
