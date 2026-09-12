// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::query::internal {

enum class TokenKind : std::uint8_t {
    word,        // bare text, possibly a reserved keyword
    quoted,      // double-quoted text with "" escaping, quotes stripped
    star,        // *
    open_paren,  // (
    close_paren, // )
};

struct Token {
    TokenKind kind{TokenKind::word};
    std::string text;
    std::size_t begin{0U};
    std::size_t end{0U};
    // True only for bare words that exactly match a reserved keyword
    // (keywords are uppercase; a lowercase spelling is ordinary text).
    bool keyword{false};
};

// Tokenizes the full source. Fails closed on unterminated quotes and
// oversized input; never guesses.
[[nodiscard]] core::Result<std::vector<Token>> lex_tkq(std::string_view source,
                                                       std::size_t maximum_source_bytes);

[[nodiscard]] bool is_reserved_keyword(std::string_view word);

} // namespace trackknife::query::internal
