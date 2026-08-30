// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/metadata/transformation.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::metadata {

enum class MetadataRuleScriptDiagnosticSeverity : std::uint8_t {
    warning,
    error,
};

struct MetadataRuleScriptDiagnostic {
    MetadataRuleScriptDiagnosticSeverity severity{MetadataRuleScriptDiagnosticSeverity::error};
    std::size_t byte_offset{0U};
    std::size_t line{1U};
    std::size_t column{1U};
    std::string message;

    friend bool operator==(const MetadataRuleScriptDiagnostic&,
                           const MetadataRuleScriptDiagnostic&) = default;
};

struct MetadataRuleScriptImportLimits {
    std::size_t source_bytes{1U * 1'024U * 1'024U};
    std::size_t syntax_nodes{4'096U};
    std::size_t nesting_depth{64U};
    std::size_t actions{256U};
};

struct MetadataRuleScriptImportResult {
    std::vector<MetadataTransformationAction> actions;
    std::vector<MetadataRuleScriptDiagnostic> diagnostics;

    [[nodiscard]] bool has_errors() const noexcept;
};

// Translates a deliberately bounded Picard-style cleanup subset into
// Trackbench-owned declarative actions. This is an import convenience, not a
// Picard scripting runtime or compatibility contract. Supported mutation calls
// are $unset/$delete, $set, and $if; supported pure expressions are fields plus
// $if/$if2/$and/$or/$not/$eq/$ne/$left.
[[nodiscard]] MetadataRuleScriptImportResult
import_metadata_rule_script(std::string_view source,
                            const MetadataRuleScriptImportLimits& limits = {});

// Produces canonical editable cleanup source only for actions represented by
// the bounded import subset. The result is required to import back to the
// exact same typed actions; typed-only actions return an unsupported error.
[[nodiscard]] core::Result<std::string>
export_metadata_rule_script(std::span<const MetadataTransformationAction> actions);

} // namespace trackknife::metadata
