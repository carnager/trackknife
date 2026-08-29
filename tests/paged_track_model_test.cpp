// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/qtmodels/paged_track_model.hpp"

#include <QSignalSpy>
#include <QTest>

namespace trackknife::qtmodels {

class PagedTrackModelTest final : public QObject {
    Q_OBJECT

  private slots:
    void exposesMillionLogicalRowsWithoutAllocatingThem();
    void loadsPagesAndRefreshesWithoutReset();
    void boundsAdversarialPageRequests();
};

void PagedTrackModelTest::exposesMillionLogicalRowsWithoutAllocatingThem() {
    PagedTrackModel model(1'000'000);
    QCOMPARE(model.rowCount(), 1'000'000);
    QCOMPARE(model.columnCount(), static_cast<int>(PagedTrackModel::column_count));
    QCOMPARE(model.residentPageCount(), 0);
    QCOMPARE(model.data(model.index(999'999, 0), Qt::DisplayRole).toString(),
             QStringLiteral("Loading…"));

    QTRY_VERIFY_WITH_TIMEOUT(model.residentPageCount() == 1, 2'000);
    QVERIFY(model.data(model.index(999'999, 0), Qt::DisplayRole)
                .toString()
                .startsWith(QStringLiteral("Artist ")));
}

void PagedTrackModelTest::loadsPagesAndRefreshesWithoutReset() {
    PagedTrackModel model(10'000);
    QSignalSpy reset_spy(&model, &QAbstractItemModel::modelReset);
    QSignalSpy changed_spy(&model, &QAbstractItemModel::dataChanged);
    QSignalSpy page_spy(&model, &PagedTrackModel::pageLoaded);

    QCOMPARE(model.data(model.index(4'096, 1), Qt::DisplayRole).toString(), QString{});
    QTRY_VERIFY_WITH_TIMEOUT(page_spy.count() == 1, 2'000);
    QVERIFY(model.data(model.index(4'096, 1), Qt::DisplayRole)
                .toString()
                .contains(QStringLiteral("Synthetic track")));

    const auto before = changed_spy.count();
    model.refreshPageContaining(4'096);
    QTRY_VERIFY_WITH_TIMEOUT(page_spy.count() == 2, 2'000);
    QVERIFY(changed_spy.count() > before);
    QCOMPARE(reset_spy.count(), 0);
}

void PagedTrackModelTest::boundsAdversarialPageRequests() {
    PagedTrackModel model(1'000'000);
    for (int row = 0; row < 1'000'000; row += 257) {
        static_cast<void>(model.data(model.index(row, 0), Qt::DisplayRole));
        QVERIFY(model.activeLoadCount() <= PagedTrackModel::activeLoadLimit());
        QVERIFY(model.pendingLoadCount() <= PagedTrackModel::pendingLoadLimit());
    }

    QTRY_VERIFY_WITH_TIMEOUT(model.activeLoadCount() == 0 && model.pendingLoadCount() == 0, 5'000);
    QVERIFY(model.residentPageCount() <= PagedTrackModel::residentPageLimit());
}

} // namespace trackknife::qtmodels

QTEST_GUILESS_MAIN(trackknife::qtmodels::PagedTrackModelTest)

#include "paged_track_model_test.moc"
