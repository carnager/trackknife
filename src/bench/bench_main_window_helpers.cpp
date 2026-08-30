// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window_helpers.hpp"

#include <algorithm>
#include <ranges>

namespace trackknife::bench {

QString trackColumnId(const int logical) {
    const auto found = std::ranges::find(track_column_specs, logical, &TrackColumnSpec::logical);
    return found == track_column_specs.end() ? QString{} : QString::fromLatin1(found->id);
}

int trackColumnLogical(const QString& id) {
    const auto found = std::ranges::find_if(
        track_column_specs, [&id](const auto& spec) { return id == QString::fromLatin1(spec.id); });
    return found == track_column_specs.end() ? -1 : found->logical;
}

QStringList trackColumnIds() {
    QStringList ids;
    ids.reserve(static_cast<qsizetype>(track_column_specs.size()));
    for (const auto& spec : track_column_specs) {
        ids.push_back(QString::fromLatin1(spec.id));
    }
    return ids;
}

QString displayText(const std::string& utf8) {
    return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
}

std::string utf8Bytes(const QString& text) {
    const auto encoded = text.toUtf8();
    return {encoded.constData(), static_cast<std::size_t>(encoded.size())};
}

QString formatTime(const qint64 milliseconds) {
    const auto total_seconds = std::max<qint64>(0, milliseconds) / 1'000;
    const auto minutes = total_seconds / 60;
    const auto seconds = total_seconds % 60;
    return QStringLiteral("%1:%2").arg(minutes).arg(seconds, 2, 10, QChar{'0'});
}

} // namespace trackknife::bench
