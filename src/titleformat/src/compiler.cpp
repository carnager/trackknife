// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/titleformat/compiler.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace trackknife::titleformat {
namespace {

[[nodiscard]] std::string asciiLower(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        result.push_back(character >= 'A' && character <= 'Z'
                             ? static_cast<char>(character - 'A' + 'a')
                             : character);
    }
    return result;
}

[[nodiscard]] std::string arityMessage(const FunctionSpec& function, const std::size_t actual) {
    std::string expected;
    if (!function.maximum_arguments) {
        expected = "at least " + std::to_string(function.minimum_arguments);
    } else if (*function.maximum_arguments == function.minimum_arguments) {
        expected = std::to_string(function.minimum_arguments);
    } else {
        expected = std::to_string(function.minimum_arguments) + " to " +
                   std::to_string(*function.maximum_arguments);
    }
    return "$" + std::string{function.name} + " expects " + expected + " argument(s), received " +
           std::to_string(actual);
}

} // namespace

class Compiler final {
  public:
    Compiler(std::string source, CompileOptions options)
        : options_(std::move(options)),
          parse_output_(parse(std::move(source), strictParseOptions(options_.parse_options))) {}

    [[nodiscard]] CompileOutput run() {
        if (options_.dialect.dialect != "tkfmt" || options_.dialect.dialect_version != 1U) {
            diagnostics_.push_back(
                {CompileDiagnosticCode::unsupported_dialect,
                 {0, parse_output_.tree.source().size()},
                 "unsupported formatting dialect; this compiler accepts tkfmt version 1"});
            return {.program = std::nullopt,
                    .parse_diagnostics = std::move(parse_output_.diagnostics),
                    .diagnostics = std::move(diagnostics_)};
        }
        if (!parse_output_.isValid()) {
            return {.program = std::nullopt,
                    .parse_diagnostics = std::move(parse_output_.diagnostics),
                    .diagnostics = {}};
        }

        Program program;
        program.syntax_ = std::move(parse_output_.tree);
        program.context_ = options_.context;
        program.dialect_ = std::move(options_.dialect);
        program.resolved_functions_.resize(program.syntax_.nodeCount());

        for (std::size_t index = 0; index < program.syntax_.nodeCount(); ++index) {
            const auto id = static_cast<NodeId>(index);
            const auto& node = program.syntax_.node(id);
            if (const auto* field = std::get_if<FieldNode>(&node.data)) {
                addFieldDependency(program, field->name);
            } else if (const auto* call = std::get_if<CallNode>(&node.data)) {
                resolveCall(program, id, node.span, *call);
            }
        }

        if (!diagnostics_.empty()) {
            return {.program = std::nullopt,
                    .parse_diagnostics = {},
                    .diagnostics = std::move(diagnostics_)};
        }
        return {.program = std::move(program), .parse_diagnostics = {}, .diagnostics = {}};
    }

  private:
    [[nodiscard]] static ParseOptions strictParseOptions(ParseOptions options) {
        options.mode = ParseMode::strict;
        return options;
    }

    static void addFieldDependency(Program& program, const std::string_view field) {
        auto normalized = asciiLower(field);
        if (std::ranges::find(program.field_dependencies_, normalized) ==
            program.field_dependencies_.end()) {
            program.field_dependencies_.push_back(std::move(normalized));
        }
    }

    [[nodiscard]] static std::optional<std::string_view> literalArgument(const Program& program,
                                                                         const NodeId argument) {
        const auto* sequence = std::get_if<SequenceNode>(&program.syntax_.node(argument).data);
        if (sequence == nullptr || sequence->children.size() != 1U) {
            return std::nullopt;
        }
        const auto* literal =
            std::get_if<LiteralNode>(&program.syntax_.node(sequence->children.front()).data);
        if (literal == nullptr || literal->value.empty()) {
            return std::nullopt;
        }
        return literal->value;
    }

    static void addExpansionDependency(Program& program, const std::string_view field) {
        auto normalized = asciiLower(field);
        if (std::ranges::find(program.expansion_dependencies_, normalized) ==
            program.expansion_dependencies_.end()) {
            program.expansion_dependencies_.push_back(std::move(normalized));
        }
    }

    static void addTechnicalDependency(Program& program, const std::string_view field) {
        auto normalized = asciiLower(field);
        if (std::ranges::find(program.technical_dependencies_, normalized) ==
            program.technical_dependencies_.end()) {
            program.technical_dependencies_.push_back(std::move(normalized));
        }
    }

    void resolveCall(Program& program, const NodeId id, const SourceSpan span,
                     const CallNode& call) {
        const auto* function = findFunction(call.name);
        if (function == nullptr) {
            diagnostics_.push_back({CompileDiagnosticCode::unknown_function, call.name_span,
                                    "unknown format function $" + call.name});
            return;
        }
        if (!function->acceptsArity(call.arguments.size())) {
            diagnostics_.push_back({CompileDiagnosticCode::wrong_argument_count, span,
                                    arityMessage(*function, call.arguments.size())});
            return;
        }
        if (function->id == FunctionId::replace && call.arguments.size() % 2U == 0U) {
            diagnostics_.push_back(
                {CompileDiagnosticCode::wrong_argument_count, span,
                 "$replace expects text followed by one or more search/replacement pairs"});
            return;
        }
        if (function->id == FunctionId::each) {
            if (options_.context != FormatContextKind::tree_level) {
                diagnostics_.push_back(
                    {CompileDiagnosticCode::unavailable_in_context, span,
                     "$each is available only in library-tree level expressions"});
                return;
            }
            const auto field = literalArgument(program, call.arguments.front());
            if (!field) {
                diagnostics_.push_back({CompileDiagnosticCode::invalid_argument, span,
                                        "$each requires one non-empty literal field name"});
                return;
            }
            addExpansionDependency(program, *field);
            addFieldDependency(program, *field);
        }
        const bool reads_metadata =
            function->id == FunctionId::get || function->id == FunctionId::getmulti ||
            function->id == FunctionId::join || function->id == FunctionId::lenmulti;
        if (reads_metadata || function->id == FunctionId::info) {
            const auto field = literalArgument(program, call.arguments.front());
            if (!field) {
                diagnostics_.push_back(
                    {CompileDiagnosticCode::invalid_argument, span,
                     "$" + call.name + " requires a non-empty literal field name"});
                return;
            }
            if (reads_metadata) {
                addFieldDependency(program, *field);
            } else {
                addTechnicalDependency(program, *field);
            }
        }
        program.resolved_functions_.at(static_cast<std::size_t>(id)) = function->id;
    }

    CompileOptions options_;
    ParseOutput parse_output_;
    std::vector<CompileDiagnostic> diagnostics_;
};

std::optional<FunctionId> Program::functionAt(const NodeId node) const {
    if (static_cast<std::size_t>(node) >= resolved_functions_.size()) {
        return std::nullopt;
    }
    return resolved_functions_.at(static_cast<std::size_t>(node));
}

CompileOutput compile(std::string source, CompileOptions options) {
    return Compiler{std::move(source), std::move(options)}.run();
}

} // namespace trackknife::titleformat
