// SPDX-License-Identifier: GPL-3.0-only

#include "bench/local_list_model.hpp"

#include "trackknife/core/local_sources.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <utility>

namespace trackknife::bench {

namespace {

[[nodiscard]] std::string file_name_of(const std::string& raw_path) {
    const auto slash = raw_path.find_last_of('/');
    if (slash == std::string::npos || slash + 1U >= raw_path.size()) {
        return raw_path;
    }
    return raw_path.substr(slash + 1U);
}

[[nodiscard]] QString escaped(const std::string& raw) {
    return QString::fromStdString(core::escape_raw_path(raw));
}

[[nodiscard]] QString display_utf8(const std::string& utf8) {
    return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
}

[[nodiscard]] QString format_duration(const std::int64_t milliseconds) {
    const auto total_seconds = std::max<std::int64_t>(milliseconds, 0) / 1'000;
    return QStringLiteral("%1:%2")
        .arg(total_seconds / 60)
        .arg(total_seconds % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

LocalListModel::LocalListModel(QObject* parent) : QAbstractTableModel(parent) {}

void LocalListModel::replaceRows(std::vector<LocalTrackRow> rows) {
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
}

void LocalListModel::appendPaths(std::vector<std::string> raw_paths, const int insertion_row) {
    if (raw_paths.empty()) {
        return;
    }
    const auto row_count = static_cast<int>(rows_.size());
    const auto target = insertion_row < 0 || insertion_row > row_count ? row_count : insertion_row;
    beginInsertRows({}, target, target + static_cast<int>(raw_paths.size()) - 1);
    auto position = rows_.begin() + target;
    for (auto& raw : raw_paths) {
        LocalTrackRow row;
        row.raw_path = std::move(raw);
        position = rows_.insert(position, std::move(row));
        ++position;
    }
    endInsertRows();
}

void LocalListModel::removeRowIndexes(std::vector<int> rows) {
    std::ranges::sort(rows);
    rows.erase(std::ranges::unique(rows).begin(), rows.end());
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        const auto row = *it;
        if (row < 0 || row >= static_cast<int>(rows_.size())) {
            continue;
        }
        beginRemoveRows({}, row, row);
        rows_.erase(rows_.begin() + row);
        endRemoveRows();
    }
}

bool LocalListModel::applyMetadata(const std::string& raw_path, const int hint_row,
                                   LocalTrackRow metadata) {
    const auto row = rowOfPath(raw_path, hint_row);
    if (row < 0) {
        return false;
    }
    metadata.raw_path = raw_path;
    metadata.probed = true;
    rows_[static_cast<std::size_t>(row)] = std::move(metadata);
    emit dataChanged(index(row, 0), index(row, column_count - 1));
    return true;
}

std::string LocalListModel::rawPath(const int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return {};
    }
    return rows_[static_cast<std::size_t>(row)].raw_path;
}

int LocalListModel::rowOfPath(const std::string& raw_path, const int hint_row) const {
    if (hint_row >= 0 && hint_row < static_cast<int>(rows_.size()) &&
        rows_[static_cast<std::size_t>(hint_row)].raw_path == raw_path) {
        return hint_row;
    }
    const auto found = std::ranges::find(rows_, raw_path, &LocalTrackRow::raw_path);
    if (found == rows_.end()) {
        return -1;
    }
    return static_cast<int>(std::distance(rows_.begin(), found));
}

int LocalListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int LocalListModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : column_count;
}

QVariant LocalListModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) {
        return {};
    }
    const auto& row = rows_[static_cast<std::size_t>(index.row())];
    if (role == raw_path_role) {
        return QByteArray(row.raw_path.data(), static_cast<qsizetype>(row.raw_path.size()));
    }
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case title_column:
            return row.title.empty() ? escaped(file_name_of(row.raw_path))
                                     : display_utf8(row.title);
        case artist_column:
            return display_utf8(row.artist);
        case album_column:
            return display_utf8(row.album);
        case duration_column:
            return row.duration_ms ? format_duration(*row.duration_ms) : QString{};
        case path_column:
            return escaped(row.raw_path);
        default:
            return {};
        }
    }
    if (role == Qt::ToolTipRole) {
        return escaped(row.raw_path);
    }
    if (role == Qt::TextAlignmentRole && index.column() == duration_column) {
        return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
    }
    return {};
}

QVariant LocalListModel::headerData(const int section, const Qt::Orientation orientation,
                                    const int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    switch (section) {
    case title_column:
        return QStringLiteral("Title");
    case artist_column:
        return QStringLiteral("Artist");
    case album_column:
        return QStringLiteral("Album");
    case duration_column:
        return QStringLiteral("Length");
    case path_column:
        return QStringLiteral("Path");
    default:
        return {};
    }
}

Qt::ItemFlags LocalListModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

} // namespace trackknife::bench
