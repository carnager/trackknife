// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/titleformat/compiler.hpp"
#include "trackknife/titleformat/evaluator.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace {

class EmptyContext final : public trackknife::titleformat::EvaluationContext {
  public:
    explicit EmptyContext(const trackknife::titleformat::FormatContextKind kind) : kind_(kind) {}

    [[nodiscard]] trackknife::titleformat::FormatContextKind kind() const noexcept override {
        return kind_;
    }

    [[nodiscard]] std::optional<std::string> resolveField(const std::string_view) const override {
        return std::nullopt;
    }

  private:
    trackknife::titleformat::FormatContextKind kind_;
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) {
    trackknife::titleformat::CompileOptions compile_options;
    compile_options.parse_options.maximum_source_bytes = 64U * 1024U;
    compile_options.parse_options.maximum_nesting_depth = 64U;
    const auto compiled = trackknife::titleformat::compile(
        std::string{reinterpret_cast<const char*>(data), size}, compile_options);
    if (!compiled.program) {
        return 0;
    }

    trackknife::titleformat::EvaluationOptions evaluation_options;
    evaluation_options.maximum_steps = 10'000U;
    evaluation_options.maximum_output_bytes = 64U * 1024U;
    const EmptyContext context(trackknife::titleformat::FormatContextKind::track_display);
    static_cast<void>(
        trackknife::titleformat::evaluate(*compiled.program, context, evaluation_options));

    compile_options.context = trackknife::titleformat::FormatContextKind::tree_level;
    const auto tree_compiled = trackknife::titleformat::compile(
        std::string{reinterpret_cast<const char*>(data), size}, compile_options);
    if (tree_compiled.program) {
        const EmptyContext tree_context(trackknife::titleformat::FormatContextKind::tree_level);
        static_cast<void>(trackknife::titleformat::evaluateExpanded(
            *tree_compiled.program, tree_context, evaluation_options));
    }
    return 0;
}
