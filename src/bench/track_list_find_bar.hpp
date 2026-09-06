// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"

#include <QFutureWatcher>
#include <QPointer>
#include <QThreadPool>
#include <QToolBar>

#include <cstdint>
#include <vector>

class QLabel;
class QLineEdit;
class QTableView;
class QTimer;
class QAbstractItemModel;

namespace trackknife::bench {

// One cancellable, bounded find traversal over either authority's track view.
// Workers receive detached text batches and never access a Qt item model.
class TrackListFindBar final : public QToolBar {
    Q_OBJECT

  public:
    explicit TrackListFindBar(QWidget* parent = nullptr);
    ~TrackListFindBar() override;
    void setView(QTableView* view);
    void open();
    void findNext(bool backwards = false);
    void dismiss();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    struct BatchResult {
        std::uint64_t generation{};
        int row{-1};
        QString error;
    };
    void cancel();
    void start(bool backwards, bool include_current);
    void pump();
    void finish();
    void invalidate();

    QPointer<QTableView> view_;
    QPointer<QAbstractItemModel> model_;
    std::vector<QMetaObject::Connection> connections_;
    QLineEdit* query_{nullptr};
    QLabel* status_{nullptr};
    QTimer* debounce_{nullptr};
    QThreadPool pool_;
    QFutureWatcher<BatchResult> watcher_;
    core::CancellationSource cancellation_;
    std::uint64_t generation_{0};
    int total_{0};
    int visited_{0};
    int cursor_{0};
    int direction_{1};
    bool wrapped_{false};
    bool searching_{false};
    bool worker_busy_{false};
};

} // namespace trackknife::bench
