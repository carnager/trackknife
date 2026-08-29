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

} // namespace

LocalListModel::LocalListModel(QObject* parent) : QAbstractTableModel(parent) {}

void LocalListModel::replacePaths(std::vector<std::string> raw_paths) {
    beginResetModel();
    raw_paths_ = std::move(raw_paths);
    endResetModel();
}

void LocalListModel::appendPaths(std::vector<std::string> raw_paths, const int insertion_row) {
    if (raw_paths.empty()) {
        return;
    }
    const auto row_count = static_cast<int>(raw_paths_.size());
    const auto target = insertion_row < 0 || insertion_row > row_count ? row_count : insertion_row;
    beginInsertRows({}, target, target + static_cast<int>(raw_paths.size()) - 1);
    raw_paths_.insert(raw_paths_.begin() + target, std::make_move_iterator(raw_paths.begin()),
                      std::make_move_iterator(raw_paths.end()));
    endInsertRows();
}

void LocalListModel::removeRowIndexes(std::vector<int> rows) {
    std::ranges::sort(rows);
    rows.erase(std::ranges::unique(rows).begin(), rows.end());
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        const auto row = *it;
        if (row < 0 || row >= static_cast<int>(raw_paths_.size())) {
            continue;
        }
        beginRemoveRows({}, row, row);
        raw_paths_.erase(raw_paths_.begin() + row);
        endRemoveRows();
    }
}

std::string LocalListModel::rawPath(const int row) const {
    if (row < 0 || row >= static_cast<int>(raw_paths_.size())) {
        return {};
    }
    return raw_paths_[static_cast<std::size_t>(row)];
}

int LocalListModel::rowOfPath(const std::string& raw_path, const int hint_row) const {
    if (hint_row >= 0 && hint_row < static_cast<int>(raw_paths_.size()) &&
        raw_paths_[static_cast<std::size_t>(hint_row)] == raw_path) {
        return hint_row;
    }
    const auto found = std::ranges::find(raw_paths_, raw_path);
    if (found == raw_paths_.end()) {
        return -1;
    }
    return static_cast<int>(std::distance(raw_paths_.begin(), found));
}

int LocalListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(raw_paths_.size());
}

int LocalListModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : column_count;
}

QVariant LocalListModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(raw_paths_.size())) {
        return {};
    }
    const auto& raw = raw_paths_[static_cast<std::size_t>(index.row())];
    if (role == raw_path_role) {
        return QByteArray(raw.data(), static_cast<qsizetype>(raw.size()));
    }
    if (role == Qt::DisplayRole) {
        if (index.column() == title_column) {
            return QString::fromStdString(core::escape_raw_path(file_name_of(raw)));
        }
        if (index.column() == path_column) {
            return QString::fromStdString(core::escape_raw_path(raw));
        }
    }
    if (role == Qt::ToolTipRole) {
        return QString::fromStdString(core::escape_raw_path(raw));
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
