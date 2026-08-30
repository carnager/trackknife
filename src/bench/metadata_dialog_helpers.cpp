// SPDX-License-Identifier: GPL-3.0-only

#include "bench/metadata_dialog_helpers.hpp"

#include "trackknife/metadata/write_plan.hpp"
#include "trackknife/operations/file_publication_apply.hpp"
#include "trackknife/operations/metadata_apply.hpp"
#include "trackknife/operations/output_path_preflight.hpp"

#include <QStringList>

#include <algorithm>
#include <ranges>
#include <utility>

namespace trackknife::bench {

QString display_utf8(const std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

std::string encode_utf8(const QString& value) {
    const auto encoded = value.toUtf8();
    return {encoded.constData(), static_cast<std::size_t>(encoded.size())};
}

QString pluralized(const std::size_t count, const QString& singular, const QString& plural) {
    return count == 1U ? singular : plural;
}

QString display_plan_values(const std::vector<std::string>& values) {
    constexpr auto maximum_visible_values = std::size_t{8U};
    constexpr auto maximum_visible_characters = qsizetype{512};
    QStringList visible;
    visible.reserve(static_cast<qsizetype>(std::min(values.size(), maximum_visible_values) +
                                           (values.size() > maximum_visible_values)));
    for (const auto& value : values | std::views::take(maximum_visible_values)) {
        auto display = value.empty() ? QStringLiteral("(empty value)") : display_utf8(value);
        if (display.size() > maximum_visible_characters) {
            display = display.left(maximum_visible_characters) + QChar{0x2026};
        }
        visible.push_back(std::move(display));
    }
    if (values.size() > maximum_visible_values) {
        visible.push_back(
            QStringLiteral("… +%1 values").arg(values.size() - maximum_visible_values));
    }
    return visible.join(QStringLiteral("  ·  "));
}

bool has_conflict(const metadata::MetadataWritePlanSource& source) {
    return std::ranges::any_of(source.issues, [](const auto& issue) {
        return issue.blocking && issue.error.code == core::ErrorCode::conflict;
    });
}

QString apply_state_text(const operations::MetadataApplySourceState state) {
    using State = operations::MetadataApplySourceState;
    switch (state) {
    case State::pending:
        return QStringLiteral("Waiting");
    case State::running:
        return QStringLiteral("Applying");
    case State::committed:
        return QStringLiteral("Applied");
    case State::failed:
        return QStringLiteral("Failed");
    case State::cancelled:
        return QStringLiteral("Cancelled");
    }
    return QStringLiteral("Unknown");
}

QString file_apply_state_text(const operations::FilePublicationApplySourceState state) {
    using State = operations::FilePublicationApplySourceState;
    switch (state) {
    case State::pending:
        return QStringLiteral("Waiting");
    case State::running:
        return QStringLiteral("Publishing");
    case State::unchanged:
        return QStringLiteral("Unchanged");
    case State::committed:
        return QStringLiteral("Published");
    case State::failed:
        return QStringLiteral("Failed");
    case State::cancelled:
        return QStringLiteral("Cancelled");
    }
    return QStringLiteral("Unknown");
}

QString publication_kind_text(const operations::OutputPathPublicationKind publication) {
    using Kind = operations::OutputPathPublicationKind;
    switch (publication) {
    case Kind::no_change:
        return QStringLiteral("No path change");
    case Kind::same_filesystem_rename:
        return QStringLiteral("Atomic same-filesystem rename");
    case Kind::cross_filesystem_copy:
        return QStringLiteral("Verified cross-filesystem move");
    }
    return QStringLiteral("Unknown publication");
}

} // namespace trackknife::bench
