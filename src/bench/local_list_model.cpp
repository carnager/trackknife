// SPDX-License-Identifier: GPL-3.0-only

#include "bench/local_list_model.hpp"

#include "trackknife/core/local_sources.hpp"
#include "uicommon/track_row_roles.hpp"

#include <QImage>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string_view>
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

[[nodiscard]] bool same_album(const LocalTrackRow& left, const LocalTrackRow& right) {
    const auto& left_artist = left.album_artist.empty() ? left.artist : left.album_artist;
    const auto& right_artist = right.album_artist.empty() ? right.artist : right.album_artist;
    return left_artist == right_artist && left.album == right.album && left.date == right.date;
}

[[nodiscard]] std::string
metadata_value(const metadata::MetadataDocument& document,
               const std::initializer_list<std::string_view> candidate_names) {
    for (const auto name : candidate_names) {
        if (auto value = document.first_effective_value(name)) {
            return std::move(*value);
        }
    }
    return {};
}

void project_display_metadata(LocalTrackRow& row) {
    row.title = metadata_value(row.metadata, {"title"});
    row.artist = metadata_value(row.metadata, {"artist"});
    row.album = metadata_value(row.metadata, {"album"});
    row.album_artist = metadata_value(row.metadata, {"albumartist"});
    row.date = metadata_value(row.metadata, {"date", "year"});
    row.track_number = metadata_value(row.metadata, {"tracknumber", "track"});
}

[[nodiscard]] bool replaceable_source_field(const metadata::MetadataField& field) {
    return field.provenance == metadata::FieldProvenance::cached_snapshot ||
           field.provenance == metadata::FieldProvenance::embedded ||
           field.provenance == metadata::FieldProvenance::stream;
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
    const auto contiguous =
        std::adjacent_find(rows.begin(), rows.end(), [](const int left, const int right) {
            return right != left + 1;
        }) == rows.end();
    if (contiguous) {
        const auto first = rows.front();
        const auto last = rows.back();
        const auto count = last - first + 1;
        auto target = insertion_row < 0
                          ? static_cast<int>(rows_.size())
                          : std::clamp(insertion_row, 0, static_cast<int>(rows_.size()));
        if (target >= first && target <= last + 1) {
            return;
        }
        if (!beginMoveRows({}, first, last, {}, target)) {
            return;
        }
        std::vector<LocalTrackRow> moved;
        moved.reserve(static_cast<std::size_t>(count));
        auto first_iterator = rows_.begin() + first;
        auto last_iterator = first_iterator + count;
        std::move(first_iterator, last_iterator, std::back_inserter(moved));
        rows_.erase(first_iterator, last_iterator);
        if (target > last) {
            target -= count;
        }
        rows_.insert(rows_.begin() + target, std::make_move_iterator(moved.begin()),
                     std::make_move_iterator(moved.end()));
        endMoveRows();
        refreshCurrentRow();
        return;
    }
    // The reset boundary must begin before touching rows_. Views retain drag
    // indexes while the callback runs. Noncontiguous selections cannot be
    // represented by one move transaction; mutating before the reset begins
    // lets Qt observe storage and persistent indexes that disagree.
    beginResetModel();
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
    metadata.logical_reference = rows_[static_cast<std::size_t>(row)].logical_reference;
    metadata.selection = rows_[static_cast<std::size_t>(row)].selection;
    metadata.segment = rows_[static_cast<std::size_t>(row)].segment;
    metadata.raw_path = raw_path;
    metadata.probed = true;
    rows_[static_cast<std::size_t>(row)] = std::move(metadata);
    emitRowChanged(row);
    return true;
}

bool LocalListModel::applyProbeRows(const std::string& raw_path, const int hint_row,
                                    std::vector<LocalTrackRow> rows) {
    if (rows.empty()) {
        return false;
    }
    const auto is_provisional = [&raw_path](const LocalTrackRow& row) {
        return row.raw_path == raw_path && !row.probed;
    };
    auto target = -1;
    if (hint_row >= 0 && hint_row < static_cast<int>(rows_.size()) &&
        is_provisional(rows_[static_cast<std::size_t>(hint_row)])) {
        target = hint_row;
    } else {
        const auto found = std::ranges::find_if(rows_, is_provisional);
        if (found != rows_.end()) {
            target = static_cast<int>(std::distance(rows_.begin(), found));
        }
    }
    if (target < 0) {
        return false;
    }

    for (auto& row : rows) {
        row.raw_path = raw_path;
        row.probed = true;
    }
    rows_[static_cast<std::size_t>(target)] = std::move(rows.front());
    emitRowChanged(target);
    if (rows.size() > 1U) {
        const auto first_inserted = target + 1;
        const auto last_inserted = target + static_cast<int>(rows.size()) - 1;
        beginInsertRows({}, first_inserted, last_inserted);
        rows_.insert(rows_.begin() + first_inserted, std::make_move_iterator(rows.begin() + 1),
                     std::make_move_iterator(rows.end()));
        endInsertRows();
    }
    refreshCurrentRow();
    return true;
}

core::Result<std::size_t>
LocalListModel::applyCommittedMetadata(const std::string& raw_path,
                                       const metadata::MetadataDocument& document,
                                       const core::LocalSourceRevision& published_revision) {
    const auto matches = [&raw_path](const LocalTrackRow& row) { return row.raw_path == raw_path; };
    const auto affected = static_cast<std::size_t>(std::ranges::count_if(rows_, matches));
    if (affected == 0U) {
        return std::size_t{0U};
    }
    const auto ambiguous = std::ranges::any_of(rows_, [&](const LocalTrackRow& row) {
        return matches(row) && row.logical_reference &&
               std::ranges::any_of(row.metadata.fields, [](const metadata::MetadataField& field) {
                   return field.provenance == metadata::FieldProvenance::cached_snapshot;
               });
    });
    if (ambiguous) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = "Logical tracks must be freshly probed before metadata commit",
            .context = {{"source_path", raw_path}},
        });
    }

    beginResetModel();
    for (auto& row : rows_) {
        if (!matches(row)) {
            continue;
        }
        std::vector<metadata::MetadataField> retained;
        retained.reserve(row.metadata.fields.size());
        std::ranges::copy_if(
            row.metadata.fields, std::back_inserter(retained),
            [](const metadata::MetadataField& field) { return !replaceable_source_field(field); });
        row.metadata = document;
        row.metadata.fields.insert(row.metadata.fields.end(),
                                   std::make_move_iterator(retained.begin()),
                                   std::make_move_iterator(retained.end()));
        row.source_revision = published_revision;
        row.probed = true;
        project_display_metadata(row);
    }
    endResetModel();
    refreshCurrentRow();
    return affected;
}

core::Result<std::size_t>
LocalListModel::applyCommittedRelocation(const std::string& source_raw_path,
                                         const std::string& target_raw_path,
                                         const core::LocalSourceRevision& previous_revision,
                                         const core::LocalSourceRevision& published_revision) {
    if (source_raw_path.empty() || target_raw_path.empty() || source_raw_path == target_raw_path ||
        previous_revision.inode == 0U || published_revision.inode == 0U) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "Committed relocation requires distinct paths and valid revisions",
            .context = {},
        });
    }
    const auto matches_source = [&source_raw_path](const LocalTrackRow& row) {
        return row.raw_path == source_raw_path;
    };
    const auto affected = static_cast<std::size_t>(std::ranges::count_if(rows_, matches_source));
    if (affected == 0U) {
        return std::size_t{0U};
    }
    if (std::ranges::any_of(rows_, [&target_raw_path](const LocalTrackRow& row) {
            return row.raw_path == target_raw_path;
        })) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = "Relocation target already exists in the local list",
            .context = {{"target_path", target_raw_path}},
        });
    }
    if (std::ranges::any_of(rows_, [&](const LocalTrackRow& row) {
            return matches_source(row) && row.source_revision != previous_revision;
        })) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = "Relocated rows no longer identify the published source",
            .context = {{"source_path", source_raw_path}},
        });
    }

    beginResetModel();
    for (auto& row : rows_) {
        if (!matches_source(row)) {
            continue;
        }
        row.raw_path = target_raw_path;
        row.source_revision = published_revision;
    }
    if (current_source_.raw_path == source_raw_path) {
        current_source_.raw_path = target_raw_path;
    }
    endResetModel();
    refreshCurrentRow();
    return affected;
}

void LocalListModel::setCurrentPath(std::string raw_path, const int hint_row) {
    setCurrentSource(
        LocalTrackSource{.raw_path = std::move(raw_path), .selection = {}, .segment = std::nullopt},
        hint_row);
}

void LocalListModel::setCurrentSource(LocalTrackSource source, const int hint_row) {
    const auto previous = current_row_;
    current_source_ = std::move(source);
    current_row_ = current_source_.raw_path.empty() ? -1 : rowOfSource(current_source_, hint_row);
    if (previous >= 0 && previous < static_cast<int>(rows_.size())) {
        emitCurrentRowChanged(previous);
    }
    if (current_row_ >= 0) {
        emitCurrentRowChanged(current_row_);
    }
}

LocalTrackSource LocalListModel::source(const int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return {};
    }
    const auto& track = rows_[static_cast<std::size_t>(row)];
    return LocalTrackSource{
        .raw_path = track.raw_path, .selection = track.selection, .segment = track.segment};
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

int LocalListModel::rowOfSource(const LocalTrackSource& source, const int hint_row) const {
    const auto matches = [&source](const LocalTrackRow& row) {
        return row.raw_path == source.raw_path && row.selection == source.selection &&
               row.segment == source.segment;
    };
    if (hint_row >= 0 && hint_row < static_cast<int>(rows_.size()) &&
        matches(rows_[static_cast<std::size_t>(hint_row)])) {
        return hint_row;
    }
    const auto found = std::ranges::find_if(rows_, matches);
    return found == rows_.end() ? -1 : static_cast<int>(std::distance(rows_.begin(), found));
}

int LocalListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int LocalListModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : local_column_count;
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
    case ui::track_album_group_start_role: {
        const auto row_index = static_cast<std::size_t>(index.row());
        const auto begins_group = (row_index == 0U || !same_album(rows_[row_index - 1U], row)) &&
                                  row_index + 1U < rows_.size() &&
                                  same_album(row, rows_[row_index + 1U]);
        return begins_group;
    }
    default:
        break;
    }
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case local_artwork_column:
            return {};
        case local_artist_column:
            return display_utf8(row.artist);
        case local_track_number_column:
            return display_utf8(row.track_number);
        case local_title_column:
            return row.title.empty() ? escaped(file_name_of(row.raw_path))
                                     : display_utf8(row.title);
        case local_album_column:
            return display_utf8(row.album);
        case local_date_column:
            return display_utf8(row.date);
        case local_length_column:
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
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole || section < 0 ||
        section >= local_column_count) {
        return {};
    }
    return QString::fromLatin1(ui::track_column_headers.at(static_cast<std::size_t>(section)));
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
    if (!rows_.empty()) {
        emit dataChanged(index(0, local_artwork_column),
                         index(static_cast<int>(rows_.size()) - 1, local_artwork_column),
                         {ui::track_album_artwork_role});
    }
}

void LocalListModel::refreshCurrentRow() {
    const auto previous = current_row_;
    current_row_ =
        current_source_.raw_path.empty() ? -1 : rowOfSource(current_source_, current_row_);
    if (previous != current_row_) {
        if (previous >= 0 && previous < static_cast<int>(rows_.size())) {
            emitCurrentRowChanged(previous);
        }
        if (current_row_ >= 0) {
            emitCurrentRowChanged(current_row_);
        }
    }
}

void LocalListModel::emitRowChanged(const int row) {
    emit dataChanged(index(row, 0), index(row, local_column_count - 1));
}

void LocalListModel::emitCurrentRowChanged(const int row) {
    emit dataChanged(index(row, 0), index(row, local_column_count - 1), {ui::track_current_role});
}

} // namespace trackknife::bench
