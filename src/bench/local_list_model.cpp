// SPDX-License-Identifier: GPL-3.0-only

#include "bench/local_list_model.hpp"

#include "trackknife/core/local_sources.hpp"
#include "uicommon/track_row_roles.hpp"

#include <QImage>

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

[[nodiscard]] std::vector<int> normalized_rows(std::vector<int> rows, const int row_count) {
    std::ranges::sort(rows);
    rows.erase(std::ranges::unique(rows).begin(), rows.end());
    std::erase_if(rows, [row_count](const int row) { return row < 0 || row >= row_count; });
    return rows;
}

} // namespace

LocalListModel::LocalListModel(QObject* parent) : QAbstractTableModel(parent) {}

void LocalListModel::replaceRows(std::vector<LocalTrackRow> rows) {
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
    refreshCurrentRow();
}

void LocalListModel::appendPaths(std::vector<std::string> raw_paths, const int insertion_row) {
    std::vector<LocalTrackRow> rows;
    rows.reserve(raw_paths.size());
    for (auto& raw : raw_paths) {
        LocalTrackRow row;
        row.raw_path = std::move(raw);
        rows.push_back(std::move(row));
    }
    appendRows(std::move(rows), insertion_row);
}

void LocalListModel::appendRows(std::vector<LocalTrackRow> rows, const int insertion_row) {
    if (rows.empty()) {
        return;
    }
    const auto row_count = static_cast<int>(rows_.size());
    const auto target = insertion_row < 0 || insertion_row > row_count ? row_count : insertion_row;
    beginInsertRows({}, target, target + static_cast<int>(rows.size()) - 1);
    rows_.insert(rows_.begin() + target, std::make_move_iterator(rows.begin()),
                 std::make_move_iterator(rows.end()));
    endInsertRows();
    refreshCurrentRow();
}

void LocalListModel::removeRowIndexes(std::vector<int> rows) {
    rows = normalized_rows(std::move(rows), static_cast<int>(rows_.size()));
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        beginRemoveRows({}, *it, *it);
        rows_.erase(rows_.begin() + *it);
        endRemoveRows();
    }
    refreshCurrentRow();
}

void LocalListModel::reorderRows(std::vector<int> rows, const int insertion_row) {
    rows = normalized_rows(std::move(rows), static_cast<int>(rows_.size()));
    if (rows.empty()) {
        return;
    }
    std::vector<LocalTrackRow> moved;
    moved.reserve(rows.size());
    auto target = insertion_row < 0 ? static_cast<int>(rows_.size()) : insertion_row;
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        moved.push_back(std::move(rows_[static_cast<std::size_t>(*it)]));
        rows_.erase(rows_.begin() + *it);
        if (*it < target) {
            --target;
        }
    }
    std::ranges::reverse(moved);
    target = std::clamp(target, 0, static_cast<int>(rows_.size()));
    beginResetModel();
    rows_.insert(rows_.begin() + target, std::make_move_iterator(moved.begin()),
                 std::make_move_iterator(moved.end()));
    endResetModel();
    refreshCurrentRow();
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
    emitRowChanged(row);
    return true;
}

void LocalListModel::setCurrentPath(std::string raw_path, const int hint_row) {
    const auto previous = current_row_;
    current_path_ = std::move(raw_path);
    current_row_ = current_path_.empty() ? -1 : rowOfPath(current_path_, hint_row);
    if (previous >= 0 && previous < static_cast<int>(rows_.size())) {
        emitRowChanged(previous);
    }
    if (current_row_ >= 0) {
        emitRowChanged(current_row_);
    }
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
    return parent.isValid() ? 0 : ui::track_column_count;
}

QVariant LocalListModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) {
        return {};
    }
    const auto& row = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case ui::track_source_role:
        return QByteArray(row.raw_path.data(), static_cast<qsizetype>(row.raw_path.size()));
    case ui::track_id_role:
    case ui::track_position_role:
        return index.row();
    case ui::track_duration_ms_role:
        return static_cast<qlonglong>(row.duration_ms.value_or(0));
    case ui::track_current_role:
        return index.row() == current_row_;
    case ui::track_album_artist_role:
        return display_utf8(row.album_artist.empty() ? row.artist : row.album_artist);
    case ui::track_priority_role:
        return {};
    case ui::track_album_artwork_role:
        return QVariant::fromValue(artwork_.value(groupKey(index.row())));
    case ui::track_album_artwork_key_role:
        return groupKey(index.row());
    default:
        break;
    }
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case ui::track_marker_column:
            return display_utf8(row.artist);
        case ui::track_title_column:
            return row.title.empty() ? escaped(file_name_of(row.raw_path))
                                     : display_utf8(row.title);
        case ui::track_album_column:
            return display_utf8(row.album);
        case ui::track_date_column:
            return display_utf8(row.date);
        case ui::track_number_column:
            return display_utf8(row.track_number);
        case ui::track_length_column:
            return row.duration_ms ? format_duration(*row.duration_ms) : QString{};
        default:
            return {};
        }
    }
    if (role == Qt::ToolTipRole) {
        return escaped(row.raw_path);
    }
    return {};
}

QVariant LocalListModel::headerData(const int section, const Qt::Orientation orientation,
                                    const int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    switch (section) {
    case ui::track_marker_column:
        return QStringLiteral("Artist");
    case ui::track_title_column:
        return QStringLiteral("Title");
    case ui::track_album_column:
        return QStringLiteral("Album");
    case ui::track_date_column:
        return QStringLiteral("Date");
    case ui::track_number_column:
        return QStringLiteral("#");
    case ui::track_length_column:
        return QStringLiteral("Length");
    default:
        return {};
    }
}

Qt::ItemFlags LocalListModel::flags(const QModelIndex& index) const {
    auto item_flags = QAbstractTableModel::flags(index);
    if (index.isValid()) {
        item_flags |= Qt::ItemIsDragEnabled;
    }
    return item_flags;
}

Qt::DropActions LocalListModel::supportedDropActions() const {
    // Drops are executed by the view's typed callbacks, never by the model,
    // but Qt only tracks and paints the drop indicator for actions the target
    // model advertises.
    return Qt::MoveAction | Qt::CopyAction;
}

QString LocalListModel::groupKey(const int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return {};
    }
    // Must mirror the shared delegate's grouping: album artist (with artist
    // fallback), album, and date, null-separated.
    const auto& track = rows_[static_cast<std::size_t>(row)];
    const auto& artist = track.album_artist.empty() ? track.artist : track.album_artist;
    return display_utf8(artist) + QChar::Null + display_utf8(track.album) + QChar::Null +
           display_utf8(track.date);
}

void LocalListModel::setArtwork(const QString& key, QImage image) {
    artwork_.insert(key, std::move(image));
    for (int row = 0; row < static_cast<int>(rows_.size()); ++row) {
        if (groupKey(row) == key) {
            emitRowChanged(row);
        }
    }
}

void LocalListModel::refreshCurrentRow() {
    const auto previous = current_row_;
    current_row_ = current_path_.empty() ? -1 : rowOfPath(current_path_, current_row_);
    if (previous != current_row_) {
        if (previous >= 0 && previous < static_cast<int>(rows_.size())) {
            emitRowChanged(previous);
        }
        if (current_row_ >= 0) {
            emitRowChanged(current_row_);
        }
    }
}

void LocalListModel::emitRowChanged(const int row) {
    emit dataChanged(index(row, 0), index(row, ui::track_column_count - 1));
}

} // namespace trackknife::bench
