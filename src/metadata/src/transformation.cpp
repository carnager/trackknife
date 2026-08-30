// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/transformation.hpp"

#include "trackknife/core/unicode.hpp"
#include "trackknife/metadata/document.hpp"
#include "trackknife/titleformat/evaluator.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace trackknife::metadata {
namespace {

using Values = std::vector<std::string>;
using OptionalValues = std::optional<Values>;
using WorkingDocument = std::unordered_map<std::string, Values>;

struct PreparedAction {
    const MetadataTransformationAction* action{nullptr};
    std::string canonical_field;
    std::string canonical_source_field;
    std::string display_field;
    std::optional<titleformat::Program> program;
};

struct Target {
    std::string canonical_field;
    std::string display_field;
    std::size_t last_action_index{0U};
};

struct PreparedChain {
    std::vector<PreparedAction> actions;
    std::vector<Target> targets;
};

[[nodiscard]] core::Error transformation_error(const core::ErrorCode code, std::string message,
                                               const std::optional<std::size_t> action = {},
                                               const std::optional<std::size_t> item = {}) {
    core::Error error{.code = code, .message = std::move(message), .context = {}};
    if (action) {
        error.context.push_back({.key = "action", .value = std::to_string(*action)});
    }
    if (item) {
        error.context.push_back({.key = "item", .value = std::to_string(*item)});
    }
    return error;
}

[[nodiscard]] core::Result<void> validate_utf8(const std::string_view value,
                                               const std::string_view description,
                                               const std::optional<std::size_t> action = {}) {
    if (const auto valid = core::unicodeCodePointCount(value); !valid) {
        return std::unexpected(
            transformation_error(core::ErrorCode::invalid_argument,
                                 std::string{description} + " must be valid UTF-8", action));
    }
    return {};
}

[[nodiscard]] const std::string& target_field(const MetadataTransformationAction& action) {
    return std::visit([](const auto& typed) -> const std::string& { return typed.target_field; },
                      action);
}

[[nodiscard]] OptionalValues values_for(const WorkingDocument& document,
                                        const std::string& canonical_field) {
    const auto found = document.find(canonical_field);
    return found == document.end() ? std::nullopt : OptionalValues{found->second};
}

[[nodiscard]] std::string trim_ascii(std::string value) {
    constexpr auto whitespace = std::string_view{" \t\n\r\f\v"};
    const auto first = value.find_first_not_of(whitespace);
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(whitespace);
    return value.substr(first, last - first + 1U);
}

[[nodiscard]] Values split_exact(const std::string_view value, const std::string_view separator) {
    Values result;
    std::size_t start = 0U;
    while (true) {
        const auto found = value.find(separator, start);
        if (found == std::string_view::npos) {
            result.emplace_back(value.substr(start));
            return result;
        }
        result.emplace_back(value.substr(start, found - start));
        start = found + separator.size();
    }
}

class TransformationEvaluationContext final : public titleformat::EvaluationContext {
  public:
    explicit TransformationEvaluationContext(const WorkingDocument& document)
        : document_(document) {}

    [[nodiscard]] titleformat::FormatContextKind kind() const noexcept override {
        return titleformat::FormatContextKind::metadata_transformation;
    }

    [[nodiscard]] std::optional<std::string>
    resolveField(const std::string_view name) const override {
        const auto values = resolveMetadata(name);
        if (!values) {
            return std::nullopt;
        }
        std::string joined;
        for (std::size_t index = 0U; index < values->size(); ++index) {
            if (index > 0U) {
                joined += "; ";
            }
            joined += (*values)[index];
        }
        return joined;
    }

    [[nodiscard]] std::optional<MetadataValues>
    resolveMetadata(const std::string_view name) const override {
        const auto found = document_.find(canonicalize_field_name(name));
        return found == document_.end() ? std::nullopt : std::optional{found->second};
    }

  private:
    const WorkingDocument& document_;
};

[[nodiscard]] core::Result<std::string> transform_value(std::string value,
                                                        const MetadataValueTransformKind transform,
                                                        const std::size_t action_index,
                                                        const std::size_t item_index) {
    switch (transform) {
    case MetadataValueTransformKind::trim_ascii:
        return trim_ascii(std::move(value));
    case MetadataValueTransformKind::lowercase: {
        auto lowered = core::unicodeSimpleLower(value);
        if (!lowered) {
            auto error = std::move(lowered.error());
            error.context.push_back({.key = "action", .value = std::to_string(action_index)});
            error.context.push_back({.key = "item", .value = std::to_string(item_index)});
            return std::unexpected(std::move(error));
        }
        return lowered;
    }
    case MetadataValueTransformKind::uppercase: {
        auto uppered = core::unicodeSimpleUpper(value);
        if (!uppered) {
            auto error = std::move(uppered.error());
            error.context.push_back({.key = "action", .value = std::to_string(action_index)});
            error.context.push_back({.key = "item", .value = std::to_string(item_index)});
            return std::unexpected(std::move(error));
        }
        return uppered;
    }
    case MetadataValueTransformKind::capitalize_first: {
        auto first_end = core::unicodeByteOffset(value, 1U);
        if (!first_end) {
            auto error = std::move(first_end.error());
            error.context.push_back({.key = "action", .value = std::to_string(action_index)});
            error.context.push_back({.key = "item", .value = std::to_string(item_index)});
            return std::unexpected(std::move(error));
        }
        auto capitalized = core::unicodeSimpleUpper(std::string_view{value}.substr(0U, *first_end));
        if (!capitalized) {
            auto error = std::move(capitalized.error());
            error.context.push_back({.key = "action", .value = std::to_string(action_index)});
            error.context.push_back({.key = "item", .value = std::to_string(item_index)});
            return std::unexpected(std::move(error));
        }
        capitalized->append(value.substr(*first_end));
        return capitalized;
    }
    }
    return std::unexpected(transformation_error(core::ErrorCode::invariant,
                                                "unknown metadata value transformation",
                                                action_index, item_index));
}

[[nodiscard]] core::Result<void>
apply_action(const PreparedAction& prepared, WorkingDocument& document,
             const std::size_t action_index, const std::size_t item_index,
             const std::size_t selection_position, const core::CancellationToken& cancellation,
             const MetadataTransformationLimits& limits) {
    return std::visit(
        [&](const auto& action) -> core::Result<void> {
            using Action = std::decay_t<decltype(action)>;
            if constexpr (std::is_same_v<Action, MetadataSetValuesAction>) {
                document[prepared.canonical_field] = action.values;
            } else if constexpr (std::is_same_v<Action, MetadataAddValuesAction>) {
                auto& values = document[prepared.canonical_field];
                if (values.size() > limits.values_per_cell ||
                    action.values.size() > limits.values_per_cell - values.size()) {
                    return std::unexpected(transformation_error(
                        core::ErrorCode::limit_exceeded,
                        "metadata add-values transformation exceeds the per-cell value limit",
                        action_index, item_index));
                }
                values.insert(values.end(), action.values.begin(), action.values.end());
            } else if constexpr (std::is_same_v<Action, MetadataRemoveFieldAction>) {
                document.erase(prepared.canonical_field);
            } else if constexpr (std::is_same_v<Action, MetadataTransformValuesAction>) {
                const auto found = document.find(prepared.canonical_field);
                if (found == document.end()) {
                    return {};
                }
                Values transformed;
                transformed.reserve(found->second.size());
                for (auto value : found->second) {
                    auto result = transform_value(std::move(value), action.transform, action_index,
                                                  item_index);
                    if (!result) {
                        return std::unexpected(std::move(result.error()));
                    }
                    transformed.push_back(std::move(*result));
                }
                found->second = std::move(transformed);
            } else if constexpr (std::is_same_v<Action, MetadataCopyFieldAction>) {
                const auto source = document.find(prepared.canonical_source_field);
                if (source == document.end()) {
                    document.erase(prepared.canonical_field);
                } else {
                    document[prepared.canonical_field] = source->second;
                }
            } else if constexpr (std::is_same_v<Action, MetadataSplitValuesAction>) {
                const auto found = document.find(prepared.canonical_field);
                if (found == document.end()) {
                    return {};
                }
                Values split_values;
                for (const auto& value : found->second) {
                    auto components = split_exact(value, action.separator);
                    if (split_values.size() > limits.values_per_cell ||
                        components.size() > limits.values_per_cell - split_values.size()) {
                        return std::unexpected(transformation_error(
                            core::ErrorCode::limit_exceeded,
                            "metadata split transformation exceeds the per-cell value limit",
                            action_index, item_index));
                    }
                    split_values.insert(split_values.end(),
                                        std::make_move_iterator(components.begin()),
                                        std::make_move_iterator(components.end()));
                }
                found->second = std::move(split_values);
            } else if constexpr (std::is_same_v<Action, MetadataJoinValuesAction>) {
                const auto found = document.find(prepared.canonical_field);
                if (found == document.end()) {
                    return {};
                }
                std::string joined;
                for (std::size_t index = 0U; index < found->second.size(); ++index) {
                    if (index > 0U) {
                        joined += action.separator;
                    }
                    joined += found->second[index];
                }
                found->second = {std::move(joined)};
            } else if constexpr (std::is_same_v<Action, MetadataRemoveMatchingValuesAction>) {
                const auto found = document.find(prepared.canonical_field);
                if (found == document.end()) {
                    return {};
                }
                std::erase(found->second, action.match);
                if (found->second.empty()) {
                    document.erase(found);
                }
            } else if constexpr (std::is_same_v<Action, MetadataReplaceMatchingValuesAction>) {
                const auto found = document.find(prepared.canonical_field);
                if (found == document.end()) {
                    return {};
                }
                Values replaced;
                for (const auto& value : found->second) {
                    const auto additions =
                        value == action.match ? action.replacement_values.size() : std::size_t{1U};
                    if (replaced.size() > limits.values_per_cell ||
                        additions > limits.values_per_cell - replaced.size()) {
                        return std::unexpected(transformation_error(
                            core::ErrorCode::limit_exceeded,
                            "metadata replace-matching transformation exceeds the per-cell "
                            "value limit",
                            action_index, item_index));
                    }
                    if (value == action.match) {
                        replaced.insert(replaced.end(), action.replacement_values.begin(),
                                        action.replacement_values.end());
                    } else {
                        replaced.push_back(value);
                    }
                }
                found->second = std::move(replaced);
            } else if constexpr (std::is_same_v<Action, MetadataNumberSelectedItemsAction>) {
                const auto number = static_cast<std::uint64_t>(action.start) + selection_position;
                if (number > std::numeric_limits<std::uint32_t>::max()) {
                    return std::unexpected(transformation_error(
                        core::ErrorCode::limit_exceeded,
                        "metadata numbering exceeds the supported integer range", action_index,
                        item_index));
                }
                auto value = std::to_string(number);
                if (value.size() < action.padding) {
                    value.insert(0U, static_cast<std::size_t>(action.padding) - value.size(), '0');
                }
                document[prepared.canonical_field] = {std::move(value)};
            } else if constexpr (std::is_same_v<Action, MetadataFormatValueAction>) {
                if (!prepared.program) {
                    return std::unexpected(transformation_error(
                        core::ErrorCode::invariant,
                        "compiled metadata transformation expression is missing", action_index,
                        item_index));
                }
                const TransformationEvaluationContext context{document};
                auto evaluated = titleformat::evaluate(
                    *prepared.program, context,
                    titleformat::EvaluationOptions{.maximum_steps = 100'000U,
                                                   .maximum_output_bytes = 1024U * 1024U,
                                                   .maximum_expanded_results = 1U,
                                                   .cancellation = cancellation});
                if (!evaluated) {
                    auto error = std::move(evaluated.error());
                    error.context.push_back(
                        {.key = "action", .value = std::to_string(action_index)});
                    error.context.push_back({.key = "item", .value = std::to_string(item_index)});
                    return std::unexpected(std::move(error));
                }
                if (auto valid = validate_utf8(
                        evaluated->text, "metadata transformation expression result", action_index);
                    !valid) {
                    return valid;
                }
                document[prepared.canonical_field] = {std::move(evaluated->text)};
            }
            return {};
        },
        *prepared.action);
}

[[nodiscard]] core::Result<PreparedChain>
prepare_chain(const MetadataTransformationChain& chain,
              const MetadataTransformationLimits& limits) {
    if (chain.schema_version != 1U || chain.actions.empty() ||
        chain.actions.size() > limits.actions || chain.name.size() > limits.chain_name_bytes) {
        return std::unexpected(transformation_error(
            core::ErrorCode::invalid_argument,
            "metadata transformation requires schema 1 and a bounded non-empty action chain"));
    }
    if (auto valid = validate_utf8(chain.name, "metadata transformation name"); !valid) {
        return std::unexpected(std::move(valid.error()));
    }

    PreparedChain result;
    result.actions.reserve(chain.actions.size());
    result.targets.reserve(chain.actions.size());
    for (std::size_t action_index = 0U; action_index < chain.actions.size(); ++action_index) {
        const auto& action = chain.actions[action_index];
        const auto& target = target_field(action);
        const auto canonical = canonicalize_field_name(target);
        if (target.empty() || target.size() > limits.field_name_bytes || canonical.empty()) {
            return std::unexpected(transformation_error(
                core::ErrorCode::invalid_argument,
                "metadata transformation target field is empty or exceeds its byte limit",
                action_index));
        }
        if (auto valid =
                validate_utf8(target, "metadata transformation target field", action_index);
            !valid) {
            return std::unexpected(std::move(valid.error()));
        }
        PreparedAction prepared_action{
            .action = &action,
            .canonical_field = canonical,
            .canonical_source_field = {},
            .display_field = target,
            .program = std::nullopt,
        };
        const auto validate_values = [&](const std::vector<std::string>& values,
                                         const std::string_view description) -> core::Result<void> {
            if (values.empty() || values.size() > limits.values_per_cell) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::invalid_argument,
                    std::string{description} + " requires a bounded non-empty value list",
                    action_index));
            }
            for (const auto& value : values) {
                if (value.size() > limits.action_text_bytes) {
                    return std::unexpected(transformation_error(
                        core::ErrorCode::limit_exceeded,
                        "metadata transformation literal value exceeds its byte limit",
                        action_index));
                }
                if (auto valid =
                        validate_utf8(value, "metadata transformation literal value", action_index);
                    !valid) {
                    return std::unexpected(std::move(valid.error()));
                }
            }
            return {};
        };
        if (const auto* set = std::get_if<MetadataSetValuesAction>(&action)) {
            if (auto valid = validate_values(set->values, "metadata set-values transformation");
                !valid) {
                return std::unexpected(std::move(valid.error()));
            }
        } else if (const auto* add = std::get_if<MetadataAddValuesAction>(&action)) {
            if (auto valid = validate_values(add->values, "metadata add-values transformation");
                !valid) {
                return std::unexpected(std::move(valid.error()));
            }
        } else if (const auto* copy = std::get_if<MetadataCopyFieldAction>(&action)) {
            prepared_action.canonical_source_field = canonicalize_field_name(copy->source_field);
            if (copy->source_field.empty() || copy->source_field.size() > limits.field_name_bytes ||
                prepared_action.canonical_source_field.empty()) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::invalid_argument,
                    "metadata copy transformation source field is empty or exceeds its byte "
                    "limit",
                    action_index));
            }
            if (auto valid = validate_utf8(copy->source_field,
                                           "metadata transformation source field", action_index);
                !valid) {
                return std::unexpected(std::move(valid.error()));
            }
        } else if (const auto* split = std::get_if<MetadataSplitValuesAction>(&action)) {
            if (split->separator.empty() || split->separator.size() > limits.action_text_bytes) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::invalid_argument,
                    "metadata split transformation requires a bounded non-empty separator",
                    action_index));
            }
            if (auto valid = validate_utf8(split->separator,
                                           "metadata transformation split separator", action_index);
                !valid) {
                return std::unexpected(std::move(valid.error()));
            }
        } else if (const auto* join = std::get_if<MetadataJoinValuesAction>(&action)) {
            if (join->separator.size() > limits.action_text_bytes) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::limit_exceeded,
                    "metadata join transformation separator exceeds its byte limit", action_index));
            }
            if (auto valid = validate_utf8(join->separator,
                                           "metadata transformation join separator", action_index);
                !valid) {
                return std::unexpected(std::move(valid.error()));
            }
        } else if (const auto* remove = std::get_if<MetadataRemoveMatchingValuesAction>(&action)) {
            if (remove->match.size() > limits.action_text_bytes) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::limit_exceeded,
                    "metadata remove-matching value exceeds its byte limit", action_index));
            }
            if (auto valid = validate_utf8(remove->match, "metadata remove-matching exact value",
                                           action_index);
                !valid) {
                return std::unexpected(std::move(valid.error()));
            }
        } else if (const auto* replace =
                       std::get_if<MetadataReplaceMatchingValuesAction>(&action)) {
            if (replace->match.size() > limits.action_text_bytes) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::limit_exceeded,
                    "metadata replace-matching value exceeds its byte limit", action_index));
            }
            if (auto valid = validate_utf8(replace->match, "metadata replace-matching exact value",
                                           action_index);
                !valid) {
                return std::unexpected(std::move(valid.error()));
            }
            if (auto valid = validate_values(replace->replacement_values,
                                             "metadata replace-matching transformation");
                !valid) {
                return std::unexpected(std::move(valid.error()));
            }
        } else if (const auto* number = std::get_if<MetadataNumberSelectedItemsAction>(&action)) {
            constexpr auto maximum_start = std::uint32_t{1'000'000'000U};
            constexpr auto maximum_padding = std::uint32_t{32U};
            if (number->start == 0U || number->start > maximum_start ||
                number->padding > maximum_padding) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::invalid_argument,
                    "metadata numbering requires a start from 1 to 1000000000 and padding from "
                    "0 to 32",
                    action_index));
            }
        } else if (const auto* format = std::get_if<MetadataFormatValueAction>(&action)) {
            const titleformat::DialectVersion current_dialect;
            if (format->dialect != current_dialect) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::invalid_argument,
                    "metadata transformation expression uses an unsupported dialect or compiler "
                    "schema",
                    action_index));
            }
            if (format->source.size() > limits.action_text_bytes) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::limit_exceeded,
                    "metadata transformation expression exceeds its byte limit", action_index));
            }
            if (auto valid = validate_utf8(
                    format->source, "metadata transformation expression source", action_index);
                !valid) {
                return std::unexpected(std::move(valid.error()));
            }
            titleformat::CompileOptions options;
            options.context = titleformat::FormatContextKind::metadata_transformation;
            options.dialect = format->dialect;
            auto compiled = titleformat::compile(format->source, options);
            if (!compiled.isValid()) {
                const auto message = !compiled.parse_diagnostics.empty()
                                         ? compiled.parse_diagnostics.front().message
                                         : compiled.diagnostics.front().message;
                return std::unexpected(transformation_error(
                    core::ErrorCode::invalid_argument,
                    "metadata transformation expression is invalid: " + message, action_index));
            }
            prepared_action.program = std::move(*compiled.program);
        }
        result.actions.push_back(std::move(prepared_action));
        const auto existing =
            std::ranges::find(result.targets, canonical, &Target::canonical_field);
        if (existing == result.targets.end()) {
            result.targets.push_back(Target{.canonical_field = canonical,
                                            .display_field = target,
                                            .last_action_index = action_index});
        } else {
            existing->last_action_index = action_index;
        }
    }
    return result;
}

} // namespace

core::Result<void>
validate_metadata_transformation_chain(const MetadataTransformationChain& chain,
                                       const MetadataTransformationLimits& limits) {
    auto prepared = prepare_chain(chain, limits);
    if (!prepared) {
        return std::unexpected(std::move(prepared.error()));
    }
    return {};
}

core::Result<MetadataTransformationPreview> plan_metadata_transformation(
    const StagedMetadataSelection& selection, const StagedMetadataPatchSet& current_draft,
    const std::span<const std::size_t> item_indexes, MetadataTransformationChain chain,
    const core::CancellationToken& cancellation, const MetadataTransformationLimits& limits) {
    auto prepared_chain = prepare_chain(chain, limits);
    if (!prepared_chain) {
        return std::unexpected(std::move(prepared_chain.error()));
    }
    if (item_indexes.empty() || item_indexes.size() > limits.items) {
        return std::unexpected(transformation_error(
            core::ErrorCode::invalid_argument,
            "metadata transformation requires a bounded non-empty item selection"));
    }
    for (std::size_t position = 0U; position < item_indexes.size(); ++position) {
        if (item_indexes[position] >= selection.item_count() ||
            (position > 0U && item_indexes[position - 1U] >= item_indexes[position])) {
            return std::unexpected(transformation_error(
                core::ErrorCode::invalid_argument,
                "metadata transformation items must be in-range, sorted, and unique"));
        }
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(transformation_error(core::ErrorCode::cancelled,
                                                    "metadata transformation was cancelled"));
    }

    const auto& prepared = prepared_chain->actions;
    const auto& targets = prepared_chain->targets;
    if (!targets.empty() && item_indexes.size() > limits.addressed_cells / targets.size()) {
        return std::unexpected(
            transformation_error(core::ErrorCode::limit_exceeded,
                                 "metadata transformation addressed-cell limit was exceeded"));
    }

    std::vector<MetadataTransformationCellPreview> cells;
    cells.reserve(item_indexes.size() * targets.size());
    const auto draft_patches = current_draft.patches();
    std::size_t draft_position = 0U;
    std::size_t changed_item_count = 0U;
    std::size_t unchanged_present_cell_count = 0U;
    std::size_t unchanged_missing_cell_count = 0U;
    std::size_t preview_text_bytes = 0U;
    for (std::size_t selection_position = 0U; selection_position < item_indexes.size();
         ++selection_position) {
        const auto item_index = item_indexes[selection_position];
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(transformation_error(core::ErrorCode::cancelled,
                                                        "metadata transformation was cancelled"));
        }
        while (draft_position < draft_patches.size() &&
               draft_patches[draft_position].item_index < item_index) {
            ++draft_position;
        }
        const auto item_draft_begin = draft_position;
        while (draft_position < draft_patches.size() &&
               draft_patches[draft_position].item_index == item_index) {
            ++draft_position;
        }
        const auto present_fields = selection.present_field_indexes(item_index);
        WorkingDocument document;
        document.reserve(present_fields.size() + (draft_position - item_draft_begin) +
                         targets.size());
        std::size_t present_position = 0U;
        auto item_draft_position = item_draft_begin;
        while (present_position < present_fields.size() || item_draft_position < draft_position) {
            const auto baseline_field = present_position < present_fields.size()
                                            ? present_fields[present_position]
                                            : std::numeric_limits<std::size_t>::max();
            const auto patch_field = item_draft_position < draft_position
                                         ? draft_patches[item_draft_position].field_index
                                         : std::numeric_limits<std::size_t>::max();
            if (patch_field <= baseline_field) {
                const auto& patch = draft_patches[item_draft_position++];
                if (patch.kind == StagedMetadataPatchKind::replace_values) {
                    document.emplace(selection.field(patch.field_index).canonical_name,
                                     patch.values);
                }
                if (patch_field == baseline_field) {
                    ++present_position;
                }
                continue;
            }
            const auto field_index = present_fields[present_position++];
            if (const auto* cell = selection.cell(item_index, field_index)) {
                document.emplace(selection.field(field_index).canonical_name, cell->values);
            }
        }
        std::vector<OptionalValues> before;
        before.reserve(targets.size());
        for (const auto& target : targets) {
            before.push_back(values_for(document, target.canonical_field));
        }
        for (std::size_t action_index = 0U; action_index < prepared.size(); ++action_index) {
            if (cancellation.is_cancellation_requested()) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::cancelled, "metadata transformation was cancelled"));
            }
            if (auto applied = apply_action(prepared[action_index], document, action_index,
                                            item_index, selection_position, cancellation, limits);
                !applied) {
                return std::unexpected(std::move(applied.error()));
            }
        }

        auto item_changed = false;
        for (std::size_t target_index = 0U; target_index < targets.size(); ++target_index) {
            auto after = values_for(document, targets[target_index].canonical_field);
            if (before[target_index] == after) {
                if (before[target_index]) {
                    ++unchanged_present_cell_count;
                } else {
                    ++unchanged_missing_cell_count;
                }
                continue;
            }
            const auto account = [&preview_text_bytes, &limits](const OptionalValues& values) {
                if (!values) {
                    return true;
                }
                for (const auto& value : *values) {
                    if (value.size() >
                        limits.total_preview_text_bytes -
                            std::min(preview_text_bytes, limits.total_preview_text_bytes)) {
                        return false;
                    }
                    preview_text_bytes += value.size();
                }
                return true;
            };
            if (!account(before[target_index]) || !account(after)) {
                return std::unexpected(transformation_error(
                    core::ErrorCode::limit_exceeded,
                    "metadata transformation preview text limit was exceeded"));
            }
            cells.push_back(MetadataTransformationCellPreview{
                .item_index = item_index,
                .last_action_index = targets[target_index].last_action_index,
                .canonical_field = targets[target_index].canonical_field,
                .display_field = targets[target_index].display_field,
                .before = std::move(before[target_index]),
                .after = std::move(after),
            });
            item_changed = true;
        }
        changed_item_count += item_changed ? 1U : 0U;
    }
    return MetadataTransformationPreview{
        .chain = std::move(chain),
        .item_indexes = {item_indexes.begin(), item_indexes.end()},
        .cells = std::move(cells),
        .changed_item_count = changed_item_count,
        .unchanged_present_cell_count = unchanged_present_cell_count,
        .unchanged_missing_cell_count = unchanged_missing_cell_count,
    };
}

} // namespace trackknife::metadata
