// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/local_list_model.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/persistence/list_repository.hpp"

#include <QFutureWatcher>
#include <QHash>
#include <QImage>
#include <QMainWindow>
#include <QSet>

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class QComboBox;
class QLabel;
class QSlider;
class QTabWidget;
class QTableView;
class QTreeView;

namespace trackknife::audio {
class LocalAuditionService;
}

namespace trackknife::ui {
class ListPersistenceService;
class LocalFolderTreeModel;
} // namespace trackknife::ui

namespace trackknife::bench {

// Trackbench main window: tabbed local working lists, a folder library dock,
// and one local-domain transport over the serialized playback worker
// (ADR-0021/0023/0024, promoted to first-class playback by ADR-0025).
class BenchMainWindow final : public QMainWindow {
    Q_OBJECT

  public:
    explicit BenchMainWindow(QWidget* parent = nullptr);
    ~BenchMainWindow() override;

    BenchMainWindow(const BenchMainWindow&) = delete;
    BenchMainWindow& operator=(const BenchMainWindow&) = delete;

    void openLocalPaths(std::vector<std::string> raw_paths);

  protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

  private:
    struct ListTab {
        persistence::ListDocument document;
        LocalListModel* model{nullptr};
        QTableView* view{nullptr};
    };

    void buildWorkspace();
    void buildTransport();
    void initializePersistence();
    void restoreLists(std::vector<persistence::ListDocument> documents);
    void schedulePersist();
    void persistNow(bool wait);
    [[nodiscard]] std::vector<persistence::ListDocument> collectDocuments();

    ListTab* addListTab(persistence::ListDocument document, bool select);
    [[nodiscard]] ListTab* currentListTab();
    [[nodiscard]] ListTab* tabForDocument(const QString& document_id);
    bool transferRows(QTableView* source, const QVariantList& rows, const QString& target_id,
                      bool move, int insertion_row);
    void markTabDirty(ListTab& tab);
    void closeTabAt(int index);
    void createList();
    void removeSelectedRows();

    void openFilesDialog();
    void openFolderDialog();
    void addFolderRoot();
    void startDiscovery(std::vector<std::string> raw_paths, QString target_document_id,
                        int insertion_row);
    void finishDiscovery();

    struct ProbeJob {
        QString document_id;
        std::string raw_path;
        int hint_row{-1};
    };
    struct ProbeOutcome {
        ProbeJob job;
        LocalTrackRow metadata;
    };
    void enqueueUnprobedRows(ListTab& tab);
    void pumpProbeQueue();
    void finishProbeBatch();

    struct ArtworkJob {
        QString key;
        std::string raw_path;
    };
    struct ArtworkOutcome {
        QString key;
        QImage image;
    };
    void syncArtwork(ListTab& tab);
    void pumpArtworkQueue();
    void finishArtworkLoad();

    void playRow(ListTab& tab, int row);
    void playAdjacent(int direction);
    [[nodiscard]] std::optional<std::pair<int, std::string>> adjacentPlaybackRow(int direction);
    void refreshTransport();
    void togglePlayPause();
    void seekToMs(qint64 position_ms);

    audio::LocalAuditionService* player_{nullptr};
    std::unique_ptr<audio::LocalAuditionService> player_storage_;

    ui::LocalFolderTreeModel* folder_model_{nullptr};
    QTreeView* folder_view_{nullptr};
    QTabWidget* tabs_{nullptr};
    std::vector<std::unique_ptr<ListTab>> list_tabs_;

    QAction* previous_action_{nullptr};
    QAction* play_pause_action_{nullptr};
    QAction* stop_action_{nullptr};
    QAction* next_action_{nullptr};
    QSlider* seek_{nullptr};
    QLabel* elapsed_{nullptr};
    QLabel* duration_{nullptr};
    QLabel* now_playing_{nullptr};
    QSlider* volume_{nullptr};
    QComboBox* device_box_{nullptr};

    ui::ListPersistenceService* persistence_{nullptr};
    QTimer* persistence_timer_{nullptr};
    QTimer* transport_timer_{nullptr};

    QFutureWatcher<core::LocalSourceDiscovery> discovery_watcher_;
    QString discovery_target_document_;
    int discovery_insertion_row_{-1};
    bool discovery_running_{false};

    QFutureWatcher<std::vector<ProbeOutcome>> probe_watcher_;
    std::deque<ProbeJob> probe_queue_;
    bool probe_running_{false};
    core::CancellationSource probe_cancellation_;

    QFutureWatcher<ArtworkOutcome> artwork_watcher_;
    std::deque<ArtworkJob> artwork_queue_;
    QHash<QString, QImage> artwork_cache_;
    QSet<QString> artwork_pending_;
    bool artwork_running_{false};

    // Paths opened before the asynchronous list restore finishes are queued
    // and flushed into the initial tab once it exists.
    std::vector<std::string> pending_open_paths_;
    bool lists_restored_{false};

    QString playback_document_id_;
    int playback_row_{-1};
    std::string playback_path_;
    bool advance_pending_{false};
    bool seeking_{false};
    bool changing_volume_{false};
    QString last_player_error_;
    std::vector<std::string> device_names_;
};

} // namespace trackknife::bench
