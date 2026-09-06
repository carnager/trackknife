// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "trackknife/lists/edit_plan.hpp"

#include <QFutureWatcher>
#include <QPointer>
#include <QThreadPool>
#include <QToolBar>

class QComboBox;
class QLabel;
class QLineEdit;
class QTableView;
class QTimer;
namespace trackknife::bench {
class LocalListModel;
class LocalListEditBar final : public QToolBar {
    Q_OBJECT
  public:
    explicit LocalListEditBar(QWidget* parent = nullptr);
    ~LocalListEditBar() override;
    void setView(QTableView* view);
    void openSort();
    void start(lists::EditRequest request);
    void cancel();
    [[nodiscard]] bool busy() const noexcept { return active_; }
  signals:
    void edited(LocalListModel* model);

  private:
    void capture();
    void finish();
    void invalidate();
    void stopWithMessage(const QString& message);
    QPointer<QTableView> view_;
    QPointer<LocalListModel> model_;
    std::vector<QMetaObject::Connection> connections_;
    QLineEdit* expression_{};
    QComboBox* direction_{};
    QLabel* status_{};
    QAction* apply_{};
    QAction* expression_action_{};
    QAction* direction_action_{};
    QAction* close_{};
    QTimer* capture_timer_{};
    QTimer* progress_timer_{};
    QThreadPool pool_;
    QFutureWatcher<core::Result<lists::EditPlan>> watcher_;
    core::CancellationSource cancellation_;
    std::shared_ptr<std::vector<lists::Entry>> snapshot_;
    std::shared_ptr<std::atomic<std::size_t>> progress_;
    lists::EditRequest request_;
    std::uint64_t generation_{0}, job_generation_{0};
    std::size_t bytes_{0};
    int total_{0};
    bool active_{false}, worker_busy_{false};
};
} // namespace trackknife::bench
