// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "bench/local_list_model.hpp"
#include "trackknife/lists/m3u8.hpp"

#include <QFutureWatcher>
#include <QPointer>
#include <QThreadPool>
#include <QToolBar>

class QLabel;
class QTimer;
namespace trackknife::bench {
// One bounded worker; imports publish a complete detached list, exports capture
// the selected local model in short slices before serializing or touching disk.
class PlaylistTransferBar final : public QToolBar {
    Q_OBJECT
  public:
    explicit PlaylistTransferBar(QWidget* parent = nullptr);
    ~PlaylistTransferBar() override;
    void importFile(std::string raw_path);
    void exportFile(std::string raw_path, LocalListModel* model);
    void cancel();
    void stop();
    [[nodiscard]] bool busy() const noexcept { return active_ || worker_busy_; }
  signals:
    void imported(std::shared_ptr<std::vector<LocalTrackRow>> rows, const QString& name);
    void completed(bool success, const QString& message);

  private:
    struct Outcome {
        std::shared_ptr<std::vector<LocalTrackRow>> rows;
        std::string name;
        std::optional<core::Error> error;
    };
    bool begin(std::string raw_path);
    void capture();
    void finish();
    void clearCapture();
    void report(bool success, const QString& message);
    QLabel* status_{};
    QAction* cancel_action_{};
    QTimer* capture_timer_{};
    QTimer* progress_timer_{};
    QThreadPool pool_;
    QFutureWatcher<Outcome> watcher_;
    core::CancellationSource cancellation_;
    std::shared_ptr<std::atomic<std::size_t>> progress_;
    std::shared_ptr<std::vector<lists::PlaylistEntry>> snapshot_;
    QPointer<LocalListModel> model_;
    std::vector<QMetaObject::Connection> connections_;
    std::string path_;
    QString operation_;
    std::size_t bytes_{0};
    int total_{0};
    bool active_{false}, worker_busy_{false};
};
} // namespace trackknife::bench
