// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QAbstractTableModel>
#include <QHash>
#include <QList>
#include <QSet>
#include <QSharedPointer>
#include <QString>
#include <QVector>

#include <array>

namespace trackknife::qtmodels {

class PagedTrackModel final : public QAbstractTableModel {
    Q_OBJECT

  public:
    static constexpr qsizetype column_count = 6;

    explicit PagedTrackModel(qint64 logical_rows, QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role) const override;

    [[nodiscard]] qint64 logicalRowCount() const noexcept { return logical_rows_; }
    [[nodiscard]] qsizetype residentPageCount() const noexcept { return pages_.size(); }
    [[nodiscard]] qsizetype activeLoadCount() const noexcept { return loading_pages_.size(); }
    [[nodiscard]] qsizetype pendingLoadCount() const noexcept { return pending_order_.size(); }
    [[nodiscard]] static constexpr qsizetype residentPageLimit() noexcept {
        return maximum_resident_pages;
    }
    [[nodiscard]] static constexpr qsizetype activeLoadLimit() noexcept {
        return maximum_active_loads;
    }
    [[nodiscard]] static constexpr qsizetype pendingLoadLimit() noexcept {
        return maximum_pending_loads;
    }

  public slots:
    void refreshPageContaining(qint64 row);

  signals:
    void pageLoaded(qint64 first_row, qint64 row_count, qint64 elapsed_microseconds);

  private:
    struct TrackRow {
        std::array<QString, column_count> cells;
    };

    struct Page {
        qint64 first_row{0};
        QVector<TrackRow> rows;
        quint64 revision{0};
    };

    static constexpr qint64 page_size = 256;
    static constexpr qsizetype maximum_resident_pages = 48;
    static constexpr qsizetype maximum_active_loads = 4;
    static constexpr qsizetype maximum_pending_loads = 16;

    [[nodiscard]] static Page generatePage(qint64 page_index, qint64 logical_rows,
                                           quint64 revision);
    void requestPage(qint64 page_index, bool force_refresh);
    void startPageLoad(qint64 page_index);
    void startPendingLoads();
    void touchPage(qint64 page_index);

    qint64 logical_rows_;
    QHash<qint64, QSharedPointer<const Page>> pages_;
    QSet<qint64> loading_pages_;
    QHash<qint64, bool> pending_refresh_;
    QList<qint64> pending_order_;
    QList<qint64> recency_;
    quint64 next_revision_{1};
};

} // namespace trackknife::qtmodels
