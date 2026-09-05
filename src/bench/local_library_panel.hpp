// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/persistence/local_library.hpp"

#include <QFutureWatcher>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QSet>
#include <QThreadPool>
#include <QWidget>

#include <deque>
#include <functional>

class QDialog;
class QLabel;
class QLineEdit;
class QListWidget;
class QStandardItem;
class QStandardItemModel;
class QTimer;
class QToolButton;
class QTreeView;

namespace trackknife::bench {

class LocalLibraryPanel final : public QWidget {
    Q_OBJECT
  public:
    explicit LocalLibraryPanel(std::filesystem::path database_path, QWidget* parent = nullptr);
    ~LocalLibraryPanel() override;
    void addRoot(std::string raw_path);
    void refreshLibrary();
    void stop();

  signals:
    void pathsRequested(std::vector<std::string> paths);

  private:
    struct Outcome {
        persistence::LibraryPage page;
        std::vector<persistence::LibraryRoot> roots;
        std::vector<std::string> paths;
        QString error;
    };
    struct Task {
        std::function<Outcome(persistence::LocalLibrary&)> work;
        std::function<void(Outcome)> done;
        bool view_query{false};
    };
    struct ScanOutcome {
        persistence::LibraryScanResult result;
        QString error;
    };

    void enqueue(Task task);
    void pump();
    void reloadTree();
    void loadChildren(const QPersistentModelIndex& parent, persistence::LibraryQuery query);
    void activate(const QModelIndex& index);
    void showFolders();
    void loadRoots();
    void startScan();
    void updateProgress();

    std::filesystem::path database_path_;
    QThreadPool pool_;
    QFutureWatcher<Outcome> query_watcher_;
    QFutureWatcher<ScanOutcome> scan_watcher_;
    std::deque<Task> tasks_;
    std::function<void(Outcome)> completion_;
    core::CancellationSource lifetime_cancellation_;
    core::CancellationSource view_cancellation_;
    core::CancellationSource scan_cancellation_;
    std::shared_ptr<persistence::LibraryScanProgress> progress_;
    QLineEdit* search_{nullptr};
    QTreeView* tree_{nullptr};
    QStandardItemModel* model_{nullptr};
    QLabel* status_{nullptr};
    QToolButton* scan_button_{nullptr};
    QTimer* search_timer_{nullptr};
    QTimer* refresh_timer_{nullptr};
    QTimer* poll_timer_{nullptr};
    QTimer* change_timer_{nullptr};
    QPointer<QDialog> folders_dialog_;
    QListWidget* roots_list_{nullptr};
    QLabel* roots_error_{nullptr};
    std::size_t generation_{0};
    QSet<QByteArray> expanded_entries_;
    QByteArray current_entry_;
    QString previous_search_;
    bool querying_{false};
    bool scanning_{false};
    bool rescan_pending_{false};
    bool stopped_{false};
};

} // namespace trackknife::bench
