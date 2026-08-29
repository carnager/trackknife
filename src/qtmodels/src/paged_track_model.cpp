// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/qtmodels/paged_track_model.hpp"

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QtConcurrentRun>

#include <algorithm>
#include <limits>

namespace trackknife::qtmodels {
namespace {

constexpr std::array<const char*, PagedTrackModel::column_count> headers{
    "Artist", "Title", "Album", "Date", "Track", "Duration"};

[[nodiscard]] QString twoDigits(const qint64 value) {
    return QStringLiteral("%1").arg(value, 2, 10, QLatin1Char('0'));
}

} // namespace

PagedTrackModel::PagedTrackModel(const qint64 logical_rows, QObject* parent)
    : QAbstractTableModel(parent),
      logical_rows_(std::clamp(logical_rows, qint64{0}, qint64{std::numeric_limits<int>::max()})) {}

int PagedTrackModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(logical_rows_);
}

int PagedTrackModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(column_count);
}

QVariant PagedTrackModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || role != Qt::DisplayRole) {
        return {};
    }

    const auto logical_row = static_cast<qint64>(index.row());
    const auto page_index = logical_row / page_size;
    const auto page = pages_.constFind(page_index);
    if (page == pages_.cend()) {
        const_cast<PagedTrackModel*>(this)->requestPage(page_index, false);
        return index.column() == 0 ? QVariant{QStringLiteral("Loading\u2026")} : QVariant{};
    }

    const auto offset = logical_row - (*page)->first_row;
    if (offset < 0 || offset >= (*page)->rows.size()) {
        return {};
    }
    const_cast<PagedTrackModel*>(this)->touchPage(page_index);
    return (*page)
        ->rows.at(static_cast<qsizetype>(offset))
        .cells.at(static_cast<std::size_t>(index.column()));
}

QVariant PagedTrackModel::headerData(const int section, const Qt::Orientation orientation,
                                     const int role) const {
    if (role == Qt::DisplayRole && orientation == Qt::Horizontal && section >= 0 &&
        section < static_cast<int>(column_count)) {
        return QString::fromLatin1(headers.at(static_cast<std::size_t>(section)));
    }
    if (role == Qt::DisplayRole && orientation == Qt::Vertical) {
        return section + 1;
    }
    return {};
}

void PagedTrackModel::refreshPageContaining(const qint64 row) {
    if (row < 0 || row >= logical_rows_) {
        return;
    }
    requestPage(row / page_size, true);
}

PagedTrackModel::Page PagedTrackModel::generatePage(const qint64 page_index,
                                                    const qint64 logical_rows,
                                                    const quint64 revision) {
    Page page;
    page.first_row = page_index * page_size;
    page.revision = revision;
    const auto count = std::min(page_size, logical_rows - page.first_row);
    page.rows.reserve(static_cast<qsizetype>(count));

    for (qint64 offset = 0; offset < count; ++offset) {
        const auto row_number = page.first_row + offset;
        const auto album_number = row_number / 11;
        const auto track_number = row_number % 11 + 1;
        const auto duration_seconds = 145 + (row_number % 235);
        page.rows.push_back(TrackRow{{
            QStringLiteral("Artist %1").arg(album_number % 8192),
            QStringLiteral("Synthetic track %1 · r%2").arg(row_number + 1).arg(revision),
            QStringLiteral("Album %1").arg(album_number + 1),
            QString::number(1970 + album_number % 57),
            QString::number(track_number),
            QStringLiteral("%1:%2")
                .arg(duration_seconds / 60)
                .arg(twoDigits(duration_seconds % 60)),
        }});
    }
    return page;
}

void PagedTrackModel::requestPage(const qint64 page_index, const bool force_refresh) {
    if (page_index < 0 || page_index * page_size >= logical_rows_ ||
        loading_pages_.contains(page_index) || (!force_refresh && pages_.contains(page_index))) {
        return;
    }

    if (pending_refresh_.contains(page_index)) {
        pending_refresh_[page_index] = pending_refresh_.value(page_index) || force_refresh;
        pending_order_.removeAll(page_index);
        pending_order_.append(page_index);
        return;
    }

    if (loading_pages_.size() < maximum_active_loads) {
        startPageLoad(page_index);
        return;
    }

    pending_refresh_.insert(page_index, force_refresh);
    pending_order_.append(page_index);
    while (pending_order_.size() > maximum_pending_loads) {
        pending_refresh_.remove(pending_order_.takeFirst());
    }
}

void PagedTrackModel::startPageLoad(const qint64 page_index) {
    loading_pages_.insert(page_index);
    const auto revision = next_revision_++;
    auto* watcher = new QFutureWatcher<Page>(this);
    auto timer = QSharedPointer<QElapsedTimer>::create();
    timer->start();

    connect(watcher, &QFutureWatcher<Page>::finished, this, [this, watcher, timer, page_index]() {
        const auto page = QSharedPointer<const Page>::create(watcher->result());
        watcher->deleteLater();
        loading_pages_.remove(page_index);
        pages_.insert(page_index, page);
        touchPage(page_index);

        const auto first = page->first_row;
        const auto last = first + page->rows.size() - 1;
        if (last >= first) {
            emit dataChanged(index(static_cast<int>(first), 0),
                             index(static_cast<int>(last), static_cast<int>(column_count) - 1),
                             {Qt::DisplayRole});
        }
        emit pageLoaded(first, page->rows.size(), timer->nsecsElapsed() / 1000);
        startPendingLoads();
    });

    watcher->setFuture(
        QtConcurrent::run(&PagedTrackModel::generatePage, page_index, logical_rows_, revision));
}

void PagedTrackModel::startPendingLoads() {
    while (loading_pages_.size() < maximum_active_loads && !pending_order_.isEmpty()) {
        const auto page_index = pending_order_.takeLast();
        const auto force_refresh = pending_refresh_.take(page_index);
        if (!force_refresh && pages_.contains(page_index)) {
            continue;
        }
        startPageLoad(page_index);
    }
}

void PagedTrackModel::touchPage(const qint64 page_index) {
    recency_.removeAll(page_index);
    recency_.append(page_index);
    while (recency_.size() > maximum_resident_pages) {
        pages_.remove(recency_.takeFirst());
    }
}

} // namespace trackknife::qtmodels
