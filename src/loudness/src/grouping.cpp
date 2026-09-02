// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/loudness/grouping.hpp"

#include "trackknife/titleformat/compiler.hpp"
#include "trackknife/titleformat/evaluator.hpp"

#include <string_view>
#include <utility>

namespace trackknife::loudness {
namespace {

[[nodiscard]] core::Error grouping_error(const core::ErrorCode code, std::string message) {
    return core::Error{.code = code, .message = std::move(message), .context = {}};
}

// Effective document values through the same lens the transformation
// dialect uses: canonical names, multi-values joined for field lookups.
class GroupingEvaluationContext final : public titleformat::EvaluationContext {
  public:
    explicit GroupingEvaluationContext(const metadata::MetadataDocument& document)
        : document_(document) {}

    [[nodiscard]] titleformat::FormatContextKind kind() const noexcept override {
        return titleformat::FormatContextKind::grouping;
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
        auto values = document_.effective_values(metadata::canonicalize_field_name(name));
        return values.empty() ? std::nullopt : std::optional{std::move(values)};
    }

  private:
    const metadata::MetadataDocument& document_;
};

[[nodiscard]] std::optional<std::string> release_key(const metadata::MetadataDocument& document) {
    if (const auto release_id = document.first_effective_value("musicbrainzalbumid");
        release_id && !release_id->empty()) {
        return "mbid:" + *release_id;
    }
    const auto album = document.first_effective_value("album");
    if (!album || album->empty()) {
        return std::nullopt;
    }
    auto artist = document.first_effective_value("albumartist");
    if (!artist || artist->empty()) {
        artist = document.first_effective_value("artist");
    }
    return "tag:" + *album + '\x1F' + artist.value_or(std::string{});
}

} // namespace

core::Result<std::vector<std::optional<std::string>>>
assign_loudness_groups(const LoudnessGrouping& grouping,
                       const std::span<const metadata::MetadataDocument* const> documents,
                       const core::CancellationToken& cancellation) {
    for (const auto* document : documents) {
        if (document == nullptr) {
            return std::unexpected(grouping_error(core::ErrorCode::invalid_argument,
                                                  "loudness grouping received a null document"));
        }
    }
    std::vector<std::optional<std::string>> keys(documents.size());
    switch (grouping.mode) {
    case LoudnessGroupingMode::track:
        return keys;
    case LoudnessGroupingMode::selection_album:
        for (auto& key : keys) {
            key = "selection";
        }
        return keys;
    case LoudnessGroupingMode::release:
        for (std::size_t index = 0U; index < documents.size(); ++index) {
            keys[index] = release_key(*documents[index]);
        }
        return keys;
    case LoudnessGroupingMode::format_expression: {
        if (grouping.expression.empty()) {
            return std::unexpected(
                grouping_error(core::ErrorCode::invalid_argument,
                               "format-expression grouping needs a tkfmt-1 expression"));
        }
        titleformat::CompileOptions options;
        options.context = titleformat::FormatContextKind::grouping;
        options.dialect = titleformat::DialectVersion{};
        auto compiled = titleformat::compile(grouping.expression, options);
        if (!compiled.isValid()) {
            return std::unexpected(
                grouping_error(core::ErrorCode::invalid_argument,
                               "the loudness grouping expression does not compile as tkfmt-1"));
        }
        for (std::size_t index = 0U; index < documents.size(); ++index) {
            if (cancellation.is_cancellation_requested()) {
                return std::unexpected(
                    grouping_error(core::ErrorCode::cancelled, "loudness grouping was cancelled"));
            }
            const GroupingEvaluationContext context{*documents[index]};
            auto evaluated = titleformat::evaluate(
                *compiled.program, context,
                titleformat::EvaluationOptions{.maximum_steps = 100'000U,
                                               .maximum_output_bytes = 64U * 1024U,
                                               .maximum_expanded_results = 1U,
                                               .cancellation = cancellation});
            if (!evaluated) {
                return std::unexpected(std::move(evaluated.error()));
            }
            if (!evaluated->text.empty()) {
                keys[index] = "fmt:" + evaluated->text;
            }
        }
        return keys;
    }
    }
    return std::unexpected(
        grouping_error(core::ErrorCode::invariant, "unknown loudness grouping mode"));
}

} // namespace trackknife::loudness
