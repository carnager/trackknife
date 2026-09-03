// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/local_list_model.hpp"
#include "bench/musicbrainz_identify_dialog.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/operations/file_publication.hpp"
#include "trackknife/operations/metadata_commit.hpp"
#include "trackknife/persistence/list_repository.hpp"
#include "uicommon/panel_layout.hpp"
#include "uicommon/track_view_layout.hpp"

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QHash>
#include <QImage>
#include <QMainWindow>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QSet>
#include <QStringList>

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class QActionGroup;
class QDialog;
class QLabel;
class QListWidget;
class QLineEdit;
class QMenu;
class QPoint;
class QResizeEvent;
class QSlider;
class QSplitter;
class QStackedWidget;
class QTabWidget;
class QTableView;
class QTimer;
class QToolButton;
class QTreeView;
class QVBoxLayout;

namespace trackknife::audio {
class LocalAuditionService;
}

namespace trackknife::ui {
class ListPersistenceService;
class LocalFolderTreeModel;
class ServerLibraryTreeModel;
class ServerLibraryTreeView;
} // namespace trackknife::ui

namespace trackknife::quick {
class MpdProbeController;
class MpdSearchResultModel;
} // namespace trackknife::quick

namespace trackknife::bench {

struct MetadataOperationJobOutcome;
class MusicBrainzFetchService;

// Trackbench main window: composed Folders/Track Lists panels, configurable
// local working-list views, and one transport over the serialized playback worker
// (ADR-0021/0023/0024, promoted to first-class playback by ADR-0025).
class BenchMainWindow final : public QMainWindow {
    Q_OBJECT

  public:
    explicit BenchMainWindow(QWidget* parent = nullptr);
    ~BenchMainWindow() override;

    BenchMainWindow(const BenchMainWindow&) = delete;
    BenchMainWindow& operator=(const BenchMainWindow&) = delete;

    void openLocalPaths(std::vector<std::string> raw_paths);
    void loadMpdUrisAsLocalFiles(const QStringList& uris);

  protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    struct ListTab {
        persistence::ListDocument document;
        LocalListModel* model{nullptr};
        QTableView* view{nullptr};
        ui::TrackViewLayout view_layout;
        QByteArray preserved_view_layout;
        bool view_layout_persistence_protected{false};
    };

    void buildWorkspace();
    void buildMpdWorkspace();
    void buildMpdSearch();
    void buildMpdStatusControls();
    void buildTransport();
    [[nodiscard]] ui::PanelLayout defaultPanelLayout() const;
    void loadPanelLayout();
    void applyPanelLayout(const ui::PanelLayout& layout);
    [[nodiscard]] QWidget* renderPanelLayoutNode(const ui::PanelLayoutNode& node, QWidget* parent);
    [[nodiscard]] ui::PanelLayoutNode capturePanelLayoutNode(QWidget* widget) const;
    void persistPanelLayout();
    void setLayoutEditMode(bool editing);
    void arrangePanelLayout(ui::PanelLayoutNodeKind kind, Qt::Orientation orientation);
    void swapPanelLayout();
    void resetPanelLayout();
    void refreshPanelLayoutActions();
    void initializePersistence();
    void restoreLists(std::vector<persistence::ListDocument> documents);
    void schedulePersist();
    void persistNow(bool wait);
    [[nodiscard]] std::vector<persistence::ListDocument> collectDocuments();
    [[nodiscard]] std::vector<persistence::TrackViewPreset> collectTrackViewLayouts();
    void openMpdConnectionDialog();
    void autoConnectMpd();
    void refreshActiveContext();
    [[nodiscard]] bool isMpdContext() const;
    void previewMpdSearch();
    void finishMpdSearch(const QString& query, bool success);
    void syncMpdSearchView();
    void closeMpdSearch(bool restore_queue_focus = true);
    void positionMpdSearchSurface();
    void resizeMpdSearchField();
    void activateMpdSearchResult(int row, int action);
    void refreshMpdStatusControls();
    void activateMpdLibraryAction(const QModelIndex& index, int action);
    void completePendingMpdLibraryAction();
    void showMpdLibraryContextMenu(const QPoint& position);
    [[nodiscard]] QVariantList selectedMpdQueueRows() const;
    [[nodiscard]] QStringList selectedMpdQueueUris() const;
    void refreshMpdPriorityMenu();

    ListTab* addListTab(persistence::ListDocument document, bool select);
    [[nodiscard]] ListTab* currentListTab();
    [[nodiscard]] ListTab* tabForDocument(const QString& document_id);
    bool transferRows(QTableView* source, const QVariantList& rows, const QString& target_id,
                      bool move, int insertion_row);
    void refreshTabChrome(ListTab& tab);
    void refreshTabActions();
    void markTabDirty(ListTab& tab);
    void closeTabAt(int index);
    void closeCurrentTab();
    void createList();
    void duplicateCurrentTab();
    void toggleCurrentTabPinned();
    void saveCurrentList();
    void renameCurrentList();
    void showTabContextMenu(const QPoint& position);
    void showTrackContextMenu(QTableView* view, const QPoint& position);
    void showFolderContextMenu(const QPoint& position);
    void showFolderBookmarkMenu(const QPoint& position);
    void loadFolderBookmarks();
    void persistFolderBookmarks() const;
    void addFolderBookmark(const std::string& raw_path);
    void revealFolderPath(const std::string& raw_path);
    void revealFolderStep(const QPersistentModelIndex& parent_index, const std::string& raw_path);
    void goToMpdLibraryEntry(const QString& artist, const QString& album);
    void completeMpdLibraryGoTo(const QString& artist, const QString& album);
    void playCurrentRow();
    void showMetadataProperties();
    void showConvertDialog();
    void showSettingsDialog();
    void startMetadataOperationRecovery();
    [[nodiscard]] MusicBrainzLookupService musicBrainzLookupService();
    void finishMetadataOperationJob();
    void presentInterruptedOperations();
    void applyCommittedMetadata(const operations::MetadataCommitResult& result);
    void applyCommittedRelocation(const operations::FilePublicationCommitResult& result);
    void applyCommittedPublicationMetadata(const operations::FilePublicationCommitResult& result,
                                           const metadata::MetadataDocument& document);
    void removeSelectedRows();
    void transferSelectedRows(QTableView* source, const QString& target_id, bool move);
    [[nodiscard]] ui::TrackViewLayout
    defaultTrackViewLayout(ui::TrackViewPresentation presentation =
                               ui::TrackViewPresentation::albums_side_artwork) const;
    void applyTrackViewLayout(ListTab& tab, const ui::TrackViewLayout& layout);
    [[nodiscard]] ui::TrackViewLayout captureTrackViewLayout(const ListTab& tab) const;
    void applyTrackViewLayout(QTableView* view, ui::TrackViewLayout& state,
                              const ui::TrackViewLayout& layout);
    [[nodiscard]] ui::TrackViewLayout
    captureTrackViewLayout(const QTableView* view, const ui::TrackViewLayout& state) const;
    void setTrackViewPresentation(ui::TrackViewPresentation presentation);
    void setTrackColumnVisible(const QString& column_id, bool visible);
    void resetTrackViewLayout();
    void copyTrackViewLayoutToAllTabs();
    void refreshTrackViewActions();
    void stopBackgroundWork();
    void refreshSelectionStatus();
    void showTrackViewHeaderMenu(QTableView* view, const QPoint& position);

    void openFilesDialog();
    void openFolderDialog();
    void addFolderRoot();
    void startDiscovery(std::vector<std::string> raw_paths, QString target_document_id,
                        int insertion_row);
    void finishDiscovery();

    struct DiscoveryOutcome {
        std::vector<LocalTrackRow> rows;
        std::vector<core::LocalSourceIssue> issues;
        bool cancelled{false};
        bool truncated{false};
    };

    struct ProbeJob {
        QString document_id;
        std::string raw_path;
        int hint_row{-1};
    };
    struct ProbeOutcome {
        ProbeJob job;
        std::vector<LocalTrackRow> rows;
        LocalTrackRow whole_file_fallback;
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
    [[nodiscard]] std::optional<std::pair<int, LocalTrackSource>>
    adjacentPlaybackRow(int direction);
    void refreshTransport();
    void refreshMpdTransport();
    void rebuildDeviceMenu();
    void configurePlaybackBuffer(const QString& profile, int capacity_ms, int start_threshold_ms);
    void showCustomPlaybackBufferDialog();
    void refreshPlaybackBufferChecks();
    void togglePlayPause();
    void seekToMs(qint64 position_ms);

    audio::LocalAuditionService* player_{nullptr};
    std::unique_ptr<audio::LocalAuditionService> player_storage_;

    ui::LocalFolderTreeModel* folder_model_{nullptr};
    QTreeView* folder_view_{nullptr};
    quick::MpdProbeController* mpd_controller_{nullptr};
    ui::ServerLibraryTreeModel* server_library_model_{nullptr};
    ui::ServerLibraryTreeView* server_library_view_{nullptr};
    QTableView* mpd_queue_view_{nullptr};
    QLineEdit* mpd_search_field_{nullptr};
    QWidget* mpd_search_surface_{nullptr};
    quick::MpdSearchResultModel* mpd_search_model_{nullptr};
    QTableView* mpd_search_view_{nullptr};
    QLabel* mpd_search_status_{nullptr};
    QToolButton* mpd_search_more_button_{nullptr};
    QTimer* mpd_search_timer_{nullptr};
    ui::TrackViewLayout mpd_view_layout_;
    QByteArray preserved_mpd_view_layout_;
    bool mpd_view_layout_persistence_protected_{false};
    std::vector<persistence::ConnectionProfile> mpd_profiles_;
    bool mpd_was_connected_{false};
    enum class MpdLibraryAction : std::uint8_t { append, next, replace, insert, load_local };
    std::optional<MpdLibraryAction> pending_mpd_library_action_;
    QPersistentModelIndex pending_mpd_library_index_;
    int pending_mpd_library_insertion_row_{-1};
    QTabWidget* tabs_{nullptr};
    std::vector<std::unique_ptr<ListTab>> list_tabs_;

    QAction* previous_action_{nullptr};
    QAction* play_pause_action_{nullptr};
    QAction* stop_action_{nullptr};
    QAction* next_action_{nullptr};
    QAction* connect_mpd_action_{nullptr};
    QAction* disconnect_mpd_action_{nullptr};
    QAction* duplicate_tab_action_{nullptr};
    QAction* pin_tab_action_{nullptr};
    QAction* save_tab_action_{nullptr};
    QAction* rename_tab_action_{nullptr};
    QAction* close_tab_action_{nullptr};
    QAction* play_selected_action_{nullptr};
    QAction* properties_action_{nullptr};
    QAction* convert_action_{nullptr};
    QAction* mpd_load_local_action_{nullptr};
    QAction* remove_selected_action_{nullptr};
    QAction* folder_add_to_list_action_{nullptr};
    QAction* folder_toggle_expanded_action_{nullptr};
    QAction* layout_edit_action_{nullptr};
    QAction* layout_side_by_side_action_{nullptr};
    QAction* layout_top_bottom_action_{nullptr};
    QAction* layout_tabbed_action_{nullptr};
    QAction* layout_swap_action_{nullptr};
    QAction* layout_reset_action_{nullptr};
    QAction* track_albums_side_action_{nullptr};
    QAction* track_albums_header_action_{nullptr};
    QAction* track_plain_columns_action_{nullptr};
    QAction* track_compact_queue_action_{nullptr};
    QAction* track_layout_reset_action_{nullptr};
    QAction* track_layout_copy_action_{nullptr};
    QSlider* seek_{nullptr};
    QLabel* elapsed_{nullptr};
    QLabel* duration_{nullptr};
    QLabel* now_playing_{nullptr};
    QLabel* now_playing_context_{nullptr};
    QLabel* selection_status_{nullptr};
    QWidget* mpd_status_separator_{nullptr};
    QAction* mpd_repeat_action_{nullptr};
    QAction* mpd_random_action_{nullptr};
    QAction* mpd_single_action_{nullptr};
    QAction* mpd_consume_action_{nullptr};
    QAction* mpd_append_selection_action_{nullptr};
    QAction* mpd_add_next_selection_action_{nullptr};
    QAction* mpd_crop_selection_action_{nullptr};
    QToolButton* mpd_repeat_button_{nullptr};
    QToolButton* mpd_random_button_{nullptr};
    QToolButton* mpd_single_button_{nullptr};
    QToolButton* mpd_consume_button_{nullptr};
    QToolButton* mpd_replaygain_button_{nullptr};
    QActionGroup* mpd_replaygain_group_{nullptr};
    QMenu* mpd_priority_menu_{nullptr};
    QSlider* volume_{nullptr};
    QToolButton* device_button_{nullptr};
    QMenu* device_menu_{nullptr};
    QActionGroup* device_group_{nullptr};
    QMenu* buffer_menu_{nullptr};
    QActionGroup* buffer_group_{nullptr};
    QMenu* tab_context_menu_{nullptr};
    QMenu* track_context_menu_{nullptr};
    QMenu* folder_context_menu_{nullptr};
    QMenu* mpd_library_context_menu_{nullptr};
    QActionGroup* layout_arrangement_group_{nullptr};
    QActionGroup* track_presentation_group_{nullptr};
    QMenu* track_columns_menu_{nullptr};
    QHash<QString, QAction*> track_column_actions_;

    QWidget* layout_host_{nullptr};
    QVBoxLayout* layout_host_layout_{nullptr};
    QWidget* layout_root_{nullptr};
    QWidget* folders_panel_{nullptr};
    QLabel* source_heading_{nullptr};
    QListWidget* folder_bookmarks_{nullptr};
    QLabel* folder_bookmarks_heading_{nullptr};
    QMenu* folder_bookmark_menu_{nullptr};
    QAction* folder_bookmark_add_action_{nullptr};
    QAction* folder_bookmark_remove_action_{nullptr};
    QAction* mpd_go_to_artist_action_{nullptr};
    QAction* mpd_go_to_album_action_{nullptr};
    QStackedWidget* source_stack_{nullptr};
    QHash<QString, QWidget*> panel_widgets_;
    bool applying_panel_layout_{false};
    bool panel_layout_persistence_protected_{false};
    bool applying_track_view_layout_{false};
    QHash<QString, QByteArray> restored_track_view_layouts_;

    ui::ListPersistenceService* persistence_{nullptr};
    std::filesystem::path database_path_;
    MusicBrainzFetchService* musicbrainz_service_{nullptr};
    QTimer* persistence_timer_{nullptr};
    QTimer* transport_timer_{nullptr};

    QFutureWatcher<DiscoveryOutcome> discovery_watcher_;
    QString discovery_target_document_;
    int discovery_insertion_row_{-1};
    bool discovery_running_{false};

    QFutureWatcher<std::vector<ProbeOutcome>> probe_watcher_;
    std::deque<ProbeJob> probe_queue_;
    bool probe_running_{false};
    core::CancellationSource probe_cancellation_;

    QFutureWatcher<void> artwork_watcher_;
    std::shared_ptr<ArtworkOutcome> artwork_outcome_;
    std::deque<ArtworkJob> artwork_queue_;
    QHash<QString, QImage> artwork_cache_;
    QSet<QString> artwork_pending_;
    bool artwork_running_{false};

    QFutureWatcher<std::shared_ptr<MetadataOperationJobOutcome>> metadata_operation_watcher_;
    std::shared_ptr<MetadataOperationJobOutcome> metadata_operation_snapshot_;
    core::CancellationSource metadata_operation_cancellation_;
    QPointer<QDialog> interrupted_operations_dialog_;
    bool metadata_operation_running_{false};
    bool metadata_recovery_started_{false};

    // Paths opened before the asynchronous list restore finishes are queued
    // and flushed into the initial tab once it exists.
    std::vector<std::string> pending_open_paths_;
    bool lists_restored_{false};

    QString playback_document_id_;
    int playback_row_{-1};
    LocalTrackSource playback_source_;
    bool advance_pending_{false};
    // Gapless continuation upkeep: the last takeover count seen, the last
    // requested next path, and a throttle for re-requests after the engine
    // dropped or rejected a queue.
    quint64 last_chain_transitions_{0U};
    std::optional<LocalTrackSource> last_requested_next_;
    QElapsedTimer next_request_timer_;
    bool seeking_{false};
    bool changing_volume_{false};
    QString last_player_error_;
    QString last_device_monitor_error_;
    QString last_output_recovery_error_;
    QString selected_buffer_profile_{QStringLiteral("balanced")};
    std::vector<std::pair<std::string, std::string>> device_choices_;
    std::optional<std::string> selected_device_;
    std::optional<std::string> default_device_;
    bool selected_device_available_{true};
    quint64 last_device_generation_{0U};
};

} // namespace trackknife::bench
