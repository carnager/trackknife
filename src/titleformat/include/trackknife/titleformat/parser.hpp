// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/titleformat/syntax.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace trackknife::titleformat {

enum class ParseMode { editor, strict };

enum class DiagnosticCode {
    source_too_large,
    nesting_limit_exceeded,
    unterminated_field,
    unterminated_call,
    invalid_escape,
    unexpected_closing_parenthesis,
};

enum class DiagnosticSeverity { error, warning };

struct Diagnostic {
    DiagnosticCode code;
    DiagnosticSeverity severity;
    SourceSpan span;
    std::string message;

    friend bool operator==(const Diagnostic&, const Diagnostic&) = default;
};

struct ParseOptions {
    ParseMode mode{ParseMode::strict};
    std::size_t maximum_source_bytes{1024U * 1024U};
    std::size_t maximum_nesting_depth{256U};
};

struct ParseOutput {
    SyntaxTree tree;
    std::vector<Diagnostic> diagnostics;
    ParseMode mode{ParseMode::strict};

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] bool canExecute() const noexcept {
        return mode == ParseMode::strict && isValid();
    }
};

[[nodiscard]] ParseOutput parse(std::string source, ParseOptions options = {});

} // namespace trackknife::titleformat
