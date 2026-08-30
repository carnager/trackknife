// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::metadata {
struct MetadataWritePlanSource;
}

namespace trackknife::operations {
enum class FilePublicationApplySourceState : std::uint8_t;
enum class MetadataApplySourceState : std::uint8_t;
enum class OutputPathPublicationKind : std::uint8_t;
} // namespace trackknife::operations

namespace trackknife::bench {

[[nodiscard]] QString display_utf8(std::string_view value);
[[nodiscard]] std::string encode_utf8(const QString& value);
[[nodiscard]] QString pluralized(std::size_t count, const QString& singular, const QString& plural);
[[nodiscard]] QString display_plan_values(const std::vector<std::string>& values);
[[nodiscard]] bool has_conflict(const metadata::MetadataWritePlanSource& source);
[[nodiscard]] QString apply_state_text(operations::MetadataApplySourceState state);
[[nodiscard]] QString file_apply_state_text(operations::FilePublicationApplySourceState state);
[[nodiscard]] QString publication_kind_text(operations::OutputPathPublicationKind publication);

} // namespace trackknife::bench
