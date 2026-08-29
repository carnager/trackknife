// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/titleformat/parser.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace trackknife::titleformat {
namespace {

enum class Terminator { end, comma, closing_parenthesis };

struct ParsedSequence {
    NodeId id;
    Terminator terminator;
};

[[nodiscard]] constexpr bool isFunctionNameCharacter(const char character) noexcept {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '_';
}

[[nodiscard]] constexpr bool isEscapable(const char character) noexcept {
    return character == '\\' || character == '$' || character == '%' || character == ',' ||
           character == '(' || character == ')';
}

} // namespace

class Parser final {
  public:
    Parser(std::string source, const ParseOptions options) : options_(options) {
        tree_.source_ = std::move(source);
    }

    [[nodiscard]] ParseOutput run() {
        if (tree_.source_.size() > options_.maximum_source_bytes) {
            diagnostics_.push_back({DiagnosticCode::source_too_large,
                                    DiagnosticSeverity::error,
                                    {0, tree_.source_.size()},
                                    "format-expression source exceeds configured byte limit"});
            tree_.nodes_.push_back({{0, tree_.source_.size()}, ErrorNode{tree_.source_}});
            tree_.nodes_.push_back({{0, tree_.source_.size()}, SequenceNode{{0}}});
            tree_.root_ = 1;
            return {std::move(tree_), std::move(diagnostics_), options_.mode};
        }

        const auto root = parseSequence(Terminator::end, 0);
        tree_.root_ = root.id;
        return {std::move(tree_), std::move(diagnostics_), options_.mode};
    }

  private:
    [[nodiscard]] NodeId addNode(const SourceSpan span, NodeData data) {
        if (tree_.nodes_.size() >= static_cast<std::size_t>(std::numeric_limits<NodeId>::max())) {
            throw std::length_error("format-expression syntax tree exceeds NodeId capacity");
        }
        const auto id = static_cast<NodeId>(tree_.nodes_.size());
        tree_.nodes_.push_back({span, std::move(data)});
        return id;
    }

    void addDiagnostic(const DiagnosticCode code, const SourceSpan span, std::string message) {
        diagnostics_.push_back({code, DiagnosticSeverity::error, span, std::move(message)});
    }

    [[nodiscard]] bool startsFunctionCall(const std::size_t position) const noexcept {
        if (position >= tree_.source_.size() || tree_.source_.at(position) != '$') {
            return false;
        }
        std::size_t cursor = position + 1U;
        while (cursor < tree_.source_.size() && isFunctionNameCharacter(tree_.source_.at(cursor))) {
            ++cursor;
        }
        return cursor > position + 1U && cursor < tree_.source_.size() &&
               tree_.source_.at(cursor) == '(';
    }

    [[nodiscard]] ParsedSequence parseSequence(const Terminator expected, const std::size_t depth) {
        const auto sequence_start = position_;
        std::vector<NodeId> children;

        while (position_ < tree_.source_.size()) {
            const char current = tree_.source_.at(position_);
            if (current == ',' && expected == Terminator::closing_parenthesis) {
                return finishSequence(sequence_start, std::move(children), Terminator::comma);
            }
            if (current == ')' && expected == Terminator::closing_parenthesis) {
                return finishSequence(sequence_start, std::move(children),
                                      Terminator::closing_parenthesis);
            }
            if (current == ')') {
                addDiagnostic(DiagnosticCode::unexpected_closing_parenthesis,
                              {position_, position_ + 1U}, "unexpected closing parenthesis");
                children.push_back(
                    addNode({position_, position_ + 1U}, ErrorNode{std::string(1, current)}));
                ++position_;
            } else if (current == '%') {
                children.push_back(parseField());
            } else if (current == '$' && startsFunctionCall(position_)) {
                children.push_back(parseCall(depth));
            } else {
                children.push_back(parseLiteral(expected));
            }
        }

        return finishSequence(sequence_start, std::move(children), Terminator::end);
    }

    [[nodiscard]] ParsedSequence finishSequence(const std::size_t start,
                                                std::vector<NodeId> children,
                                                const Terminator terminator) {
        return {addNode({start, position_}, SequenceNode{std::move(children)}), terminator};
    }

    [[nodiscard]] NodeId parseLiteral(const Terminator expected) {
        const auto start = position_;
        std::string value;

        while (position_ < tree_.source_.size()) {
            const char current = tree_.source_.at(position_);
            const bool terminates =
                (current == ',' && expected == Terminator::closing_parenthesis) ||
                (current == ')' && expected == Terminator::closing_parenthesis);
            if (terminates || current == '%' || current == ')' ||
                (current == '$' && startsFunctionCall(position_))) {
                break;
            }

            if (current != '\\') {
                value.push_back(current);
                ++position_;
                continue;
            }

            const auto escape_start = position_++;
            if (position_ >= tree_.source_.size()) {
                addDiagnostic(DiagnosticCode::invalid_escape, {escape_start, position_},
                              "trailing backslash in format expression");
                break;
            }
            const auto escaped = tree_.source_.at(position_++);
            if (!isEscapable(escaped)) {
                addDiagnostic(DiagnosticCode::invalid_escape, {escape_start, position_},
                              "backslash may escape only \\, $, %, comma, or parentheses");
            }
            value.push_back(escaped);
        }

        return addNode({start, position_}, LiteralNode{std::move(value)});
    }

    [[nodiscard]] NodeId parseField() {
        const auto start = position_++;
        const auto name_start = position_;
        const auto closing = tree_.source_.find('%', position_);
        if (closing == std::string::npos) {
            position_ = tree_.source_.size();
            addDiagnostic(DiagnosticCode::unterminated_field, {start, position_},
                          "unterminated field reference");
            return addNode(
                {start, position_},
                FieldNode{tree_.source_.substr(name_start), {name_start, position_}, false});
        }

        position_ = closing + 1U;
        return addNode({start, position_},
                       FieldNode{tree_.source_.substr(name_start, closing - name_start),
                                 {name_start, closing},
                                 true});
    }

    [[nodiscard]] NodeId parseCall(const std::size_t depth) {
        const auto start = position_++;
        const auto name_start = position_;
        while (position_ < tree_.source_.size() &&
               isFunctionNameCharacter(tree_.source_.at(position_))) {
            ++position_;
        }
        const auto name_end = position_;
        ++position_; // startsFunctionCall established the opening parenthesis.

        std::vector<NodeId> arguments;
        bool terminated = false;
        if (position_ < tree_.source_.size() && tree_.source_.at(position_) == ')') {
            ++position_;
            terminated = true;
        } else if (depth >= options_.maximum_nesting_depth) {
            addDiagnostic(DiagnosticCode::nesting_limit_exceeded, {start, position_},
                          "format-expression nesting limit exceeded");
        } else {
            while (true) {
                const auto argument = parseSequence(Terminator::closing_parenthesis, depth + 1U);
                arguments.push_back(argument.id);
                if (argument.terminator == Terminator::comma) {
                    ++position_;
                    continue;
                }
                if (argument.terminator == Terminator::closing_parenthesis) {
                    ++position_;
                    terminated = true;
                }
                break;
            }
        }

        if (!terminated) {
            addDiagnostic(DiagnosticCode::unterminated_call, {start, position_},
                          "unterminated function call");
        }
        return addNode({start, position_},
                       CallNode{tree_.source_.substr(name_start, name_end - name_start),
                                {name_start, name_end},
                                std::move(arguments),
                                terminated});
    }

    ParseOptions options_;
    SyntaxTree tree_;
    std::vector<Diagnostic> diagnostics_;
    std::size_t position_{0};
};

const SyntaxNode& SyntaxTree::node(const NodeId id) const {
    return nodes_.at(static_cast<std::size_t>(id));
}

std::string_view SyntaxTree::sourceText(const SourceSpan span) const {
    if (span.begin > span.end || span.end > source_.size()) {
        throw std::out_of_range("format-expression source span is outside the source");
    }
    return std::string_view{source_}.substr(span.begin, span.length());
}

bool ParseOutput::isValid() const noexcept {
    return std::ranges::none_of(diagnostics, [](const Diagnostic& diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::error;
    });
}

ParseOutput parse(std::string source, const ParseOptions options) {
    return Parser{std::move(source), options}.run();
}

} // namespace trackknife::titleformat
