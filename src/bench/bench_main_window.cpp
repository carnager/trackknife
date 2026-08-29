// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"

#include "bench/local_list_model.hpp"
#include "trackknife/audio/local_audition.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/formats/probe.hpp"
#include "uicommon/list_persistence_service.hpp"
#include "uicommon/local_folder_tree_model.hpp"
#include "uicommon/queue_item_delegate.hpp"
#include "uicommon/queue_table_view.hpp"
#include "uicommon/track_row_roles.hpp"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMenuBar>
#include <QMimeData>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QTreeView>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <iterator>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

namespace trackknife::bench {

namespace {

constexpr int transport_refresh_ms = 33;
constexpr int persist_debounce_ms = 1'000;

[[nodiscard]] QString displayText(const std::string& utf8) {
    return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
}

[[nodiscard]] std::string utf8Bytes(const QString& text) {
    const auto encoded = text.toUtf8();
    return {encoded.constData(), static_cast<std::size_t>(encoded.size())};
}

[[nodiscard]] QString formatTime(const qint64 milliseconds) {
    const auto total_seconds = std::max<qint64>(milliseconds, 0) / 1'000;
    return QStringLiteral("%1:%2")
        .arg(total_seconds / 60)
        .arg(total_seconds % 60, 2, 10, QLatin1Char('0'));
}

[[nodiscard]] bool playerActive(const audio::LocalAuditionState state) {
    return state == audio::LocalAuditionState::buffering ||
           state == audio::LocalAuditionState::playing ||
           state == audio::LocalAuditionState::draining;
}

constexpr std::size_t probe_batch_size = 8U;

[[nodiscard]] std::string lowercased_ascii(std::string name) {
    for (auto& character : name) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return name;
}

// Case-insensitive first-value lookup over the probe's ordered tags.
[[nodiscard]] std::string probed_tag(const formats::MediaProbe& probe,
                                     const std::string_view name) {
    for (const auto& tag : probe.tags) {
        if (lowercased_ascii(tag.name) == name) {
            return tag.value;
        }
    }
    return {};
}

} // namespace

BenchMainWindow::BenchMainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("Trackbench"));
    resize(1100, 720);
    setAcceptDrops(true);

    if (auto player = audio::LocalAuditionService::create(); player) {
        player_storage_ = std::move(*player);
        player_ = player_storage_.get();
    }

    buildWorkspace();
    buildTransport();
    initializePersistence();

    if (player_ != nullptr) {
        static_cast<void>(player_->refresh_output_devices());
        transport_timer_ = new QTimer(this);
        transport_timer_->setInterval(transport_refresh_ms);
        connect(transport_timer_, &QTimer::timeout, this, &BenchMainWindow::refreshTransport);
        transport_timer_->start();
    } else {
        statusBar()->showMessage(
            QStringLiteral("Local playback unavailable: the audio worker failed to start"));
    }
    refreshTransport();
}

BenchMainWindow::~BenchMainWindow() = default;

void BenchMainWindow::buildWorkspace() {
    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("bench-tabs"));
    tabs_->setDocumentMode(true);
    tabs_->setMovable(true);
    tabs_->setTabsClosable(true);
    connect(tabs_, &QTabWidget::tabCloseRequested, this, &BenchMainWindow::closeTabAt);
    setCentralWidget(tabs_);

    folder_model_ = new ui::LocalFolderTreeModel(this);
    folder_view_ = new QTreeView(this);
    folder_view_->setObjectName(QStringLiteral("bench-folder-tree"));
    folder_view_->setModel(folder_model_);
    folder_view_->setHeaderHidden(true);
    folder_view_->setUniformRowHeights(true);
    connect(folder_view_, &QTreeView::activated, this, [this](const QModelIndex& index) {
        if (!index.isValid() || folder_model_->isDirectory(index)) {
            return;
        }
        openLocalPaths({folder_model_->rawPath(index)});
    });
    auto* dock = new QDockWidget(QStringLiteral("Folders"), this);
    dock->setObjectName(QStringLiteral("bench-folders-dock"));
    dock->setWidget(folder_view_);
    addDockWidget(Qt::LeftDockWidgetArea, dock);

    QSettings settings;
    const auto roots = settings.value(QStringLiteral("library/roots")).toList();
    for (const auto& root : roots) {
        const auto bytes = root.toByteArray();
        if (!bytes.isEmpty()) {
            folder_model_->addRoot(
                std::string{bytes.constData(), static_cast<std::size_t>(bytes.size())});
        }
    }

    auto* file_menu = menuBar()->addMenu(QStringLiteral("&File"));
    auto* new_list = file_menu->addAction(QStringLiteral("New list…"));
    new_list->setShortcut(QKeySequence::New);
    connect(new_list, &QAction::triggered, this, &BenchMainWindow::createList);
    auto* open_files = file_menu->addAction(QStringLiteral("Open files…"));
    open_files->setShortcut(QKeySequence::Open);
    connect(open_files, &QAction::triggered, this, &BenchMainWindow::openFilesDialog);
    auto* open_folder = file_menu->addAction(QStringLiteral("Open folder…"));
    open_folder->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+O")));
    connect(open_folder, &QAction::triggered, this, &BenchMainWindow::openFolderDialog);
    auto* add_root = file_menu->addAction(QStringLiteral("Add library folder…"));
    connect(add_root, &QAction::triggered, this, &BenchMainWindow::addFolderRoot);
    file_menu->addSeparator();
    auto* quit = file_menu->addAction(QStringLiteral("Quit"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);

    auto* edit_menu = menuBar()->addMenu(QStringLiteral("&Edit"));
    auto* remove_rows = edit_menu->addAction(QStringLiteral("Remove selected"));
    remove_rows->setShortcut(QKeySequence::Delete);
    connect(remove_rows, &QAction::triggered, this, &BenchMainWindow::removeSelectedRows);
}

void BenchMainWindow::buildTransport() {
    auto* bar = addToolBar(QStringLiteral("Transport"));
    bar->setObjectName(QStringLiteral("bench-transport"));
    bar->setMovable(false);

    previous_action_ = bar->addAction(style()->standardIcon(QStyle::SP_MediaSkipBackward),
                                      QStringLiteral("Previous"));
    connect(previous_action_, &QAction::triggered, this, [this] { playAdjacent(-1); });
    play_pause_action_ =
        bar->addAction(style()->standardIcon(QStyle::SP_MediaPlay), QStringLiteral("Play"));
    play_pause_action_->setShortcut(Qt::Key_Space);
    play_pause_action_->setShortcutContext(Qt::ApplicationShortcut);
    connect(play_pause_action_, &QAction::triggered, this, &BenchMainWindow::togglePlayPause);
    stop_action_ =
        bar->addAction(style()->standardIcon(QStyle::SP_MediaStop), QStringLiteral("Stop"));
    connect(stop_action_, &QAction::triggered, this, [this] {
        if (player_ != nullptr) {
            static_cast<void>(player_->stop());
        }
    });
    next_action_ =
        bar->addAction(style()->standardIcon(QStyle::SP_MediaSkipForward), QStringLiteral("Next"));
    connect(next_action_, &QAction::triggered, this, [this] { playAdjacent(1); });

    elapsed_ = new QLabel(QStringLiteral("0:00"), bar);
    bar->addWidget(elapsed_);
    seek_ = new QSlider(Qt::Horizontal, bar);
    seek_->setObjectName(QStringLiteral("bench-seek"));
    seek_->setMinimumWidth(240);
    connect(seek_, &QSlider::sliderPressed, this, [this] { seeking_ = true; });
    connect(seek_, &QSlider::sliderReleased, this, [this] {
        seeking_ = false;
        seekToMs(seek_->value());
    });
    bar->addWidget(seek_);
    duration_ = new QLabel(QStringLiteral("0:00"), bar);
    bar->addWidget(duration_);

    now_playing_ = new QLabel(bar);
    now_playing_->setObjectName(QStringLiteral("bench-now-playing"));
    now_playing_->setTextFormat(Qt::PlainText);
    now_playing_->setMinimumWidth(160);
    bar->addWidget(now_playing_);

    volume_ = new QSlider(Qt::Horizontal, bar);
    volume_->setObjectName(QStringLiteral("bench-volume"));
    volume_->setRange(0, 100);
    volume_->setValue(100);
    volume_->setMaximumWidth(120);
    volume_->setToolTip(QStringLiteral("Volume"));
    connect(volume_, &QSlider::sliderPressed, this, [this] { changing_volume_ = true; });
    connect(volume_, &QSlider::sliderReleased, this, [this] { changing_volume_ = false; });
    connect(volume_, &QSlider::valueChanged, this, [this](const int value) {
        if (player_ != nullptr) {
            static_cast<void>(player_->set_volume_percent(value));
        }
    });
    bar->addWidget(volume_);

    device_box_ = new QComboBox(bar);
    device_box_->setObjectName(QStringLiteral("bench-device"));
    device_box_->addItem(QStringLiteral("System default"));
    device_box_->setToolTip(QStringLiteral("PipeWire output device"));
    connect(device_box_, &QComboBox::activated, this, [this](const int index) {
        if (player_ == nullptr) {
            return;
        }
        std::optional<std::string> target;
        if (index > 0 && index <= static_cast<int>(device_names_.size())) {
            target = device_names_[static_cast<std::size_t>(index - 1)];
        }
        static_cast<void>(player_->set_output_target(std::move(target)));
    });
    bar->addWidget(device_box_);

    auto* playback_menu = menuBar()->addMenu(QStringLiteral("&Playback"));
    playback_menu->addAction(play_pause_action_);
    playback_menu->addAction(stop_action_);
    playback_menu->addAction(previous_action_);
    playback_menu->addAction(next_action_);
    playback_menu->addSeparator();
    auto* refresh_devices = playback_menu->addAction(QStringLiteral("Refresh audio devices"));
    connect(refresh_devices, &QAction::triggered, this, [this] {
        if (player_ != nullptr) {
            static_cast<void>(player_->refresh_output_devices());
        }
    });
}

void BenchMainWindow::initializePersistence() {
    const auto base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(base);
    persistence_ = new ui::ListPersistenceService(
        std::filesystem::path{utf8Bytes(base + QStringLiteral("/lists.sqlite"))}, this);
    persistence_timer_ = new QTimer(this);
    persistence_timer_->setSingleShot(true);
    persistence_timer_->setInterval(persist_debounce_ms);
    connect(persistence_timer_, &QTimer::timeout, this, [this] { persistNow(false); });
    persistence_->initialize([this](ui::PersistedWorkspace workspace, QString error) {
        if (!error.isEmpty()) {
            statusBar()->showMessage(QStringLiteral("List restore failed: %1").arg(error), 5'000);
        }
        restoreLists(std::move(workspace.lists));
    });
}

void BenchMainWindow::restoreLists(std::vector<persistence::ListDocument> documents) {
    lists_restored_ = true;
    for (auto& document : documents) {
        addListTab(std::move(document), false);
    }
    if (tabs_->count() == 0) {
        addListTab(
            persistence::ListDocument{
                .id = core::StableId::random(),
                .kind = persistence::ListKind::scratch,
                .name = "Scratch",
                .pinned = false,
                .dirty = false,
                .items = {},
            },
            true);
    } else {
        tabs_->setCurrentIndex(0);
    }
    if (!pending_open_paths_.empty()) {
        openLocalPaths(std::move(pending_open_paths_));
        pending_open_paths_.clear();
    }
}

void BenchMainWindow::schedulePersist() {
    if (persistence_timer_ != nullptr) {
        persistence_timer_->start();
    }
}

std::vector<persistence::ListDocument> BenchMainWindow::collectDocuments() {
    std::vector<persistence::ListDocument> documents;
    documents.reserve(static_cast<std::size_t>(tabs_->count()));
    for (int index = 0; index < tabs_->count(); ++index) {
        auto* view = qobject_cast<QTableView*>(tabs_->widget(index));
        if (view == nullptr) {
            continue;
        }
        const auto id = view->property("bench-document-id").toString();
        auto* tab = tabForDocument(id);
        if (tab == nullptr) {
            continue;
        }
        auto document = tab->document;
        document.items.clear();
        document.items.reserve(tab->model->rows().size());
        for (const auto& row : tab->model->rows()) {
            persistence::ListItem item{
                .source = persistence::ListSource::local,
                .profile_id = std::nullopt,
                .source_reference = row.raw_path,
                .duration_ms = row.duration_ms,
                .fields = {},
            };
            if (!row.title.empty()) {
                item.fields.push_back({.name = "title", .value = row.title});
            }
            if (!row.artist.empty()) {
                item.fields.push_back({.name = "artist", .value = row.artist});
            }
            if (!row.album.empty()) {
                item.fields.push_back({.name = "album", .value = row.album});
            }
            if (!row.album_artist.empty()) {
                item.fields.push_back({.name = "albumartist", .value = row.album_artist});
            }
            if (!row.date.empty()) {
                item.fields.push_back({.name = "date", .value = row.date});
            }
            if (!row.track_number.empty()) {
                item.fields.push_back({.name = "track", .value = row.track_number});
            }
            document.items.push_back(std::move(item));
        }
        documents.push_back(std::move(document));
    }
    return documents;
}

void BenchMainWindow::persistNow(const bool wait) {
    if (persistence_ == nullptr) {
        return;
    }
    auto documents = collectDocuments();
    if (wait) {
        const auto error = persistence_->saveWorkspaceAndWait(std::move(documents), {});
        if (!error.isEmpty()) {
            statusBar()->showMessage(QStringLiteral("List save failed: %1").arg(error), 5'000);
        }
        return;
    }
    persistence_->saveWorkspace(std::move(documents), {}, [this](QString error) {
        if (!error.isEmpty()) {
            statusBar()->showMessage(QStringLiteral("List save failed: %1").arg(error), 5'000);
        }
    });
}

BenchMainWindow::ListTab* BenchMainWindow::addListTab(persistence::ListDocument document,
                                                      const bool select) {
    const auto id = QString::fromStdString(document.id.to_string());
    auto* model = new LocalListModel(tabs_);
    std::vector<LocalTrackRow> restored_rows;
    restored_rows.reserve(document.items.size());
    for (const auto& item : document.items) {
        if (item.source != persistence::ListSource::local) {
            continue;
        }
        LocalTrackRow row;
        row.raw_path = item.source_reference;
        row.duration_ms = item.duration_ms;
        for (const auto& field : item.fields) {
            if (field.name == "title") {
                row.title = field.value;
            } else if (field.name == "artist") {
                row.artist = field.value;
            } else if (field.name == "album") {
                row.album = field.value;
            } else if (field.name == "albumartist") {
                row.album_artist = field.value;
            } else if (field.name == "date") {
                row.date = field.value;
            } else if (field.name == "track") {
                row.track_number = field.value;
            }
        }
        row.probed = row.duration_ms.has_value() || !row.title.empty() || !row.artist.empty() ||
                     !row.album.empty();
        restored_rows.push_back(std::move(row));
    }
    model->replaceRows(std::move(restored_rows));

    auto* view = new ui::QueueTableView(tabs_);
    view->setObjectName(QStringLiteral("bench-list-%1").arg(id.left(8)));
    view->setProperty("bench-document-id", id);
    view->setModel(model);
    view->setItemDelegate(new ui::QueueItemDelegate(view));
    view->setProperty("trackknife-hover-row", -1);
    view->setAlternatingRowColors(true);
    view->setShowGrid(false);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->verticalHeader()->setDefaultSectionSize(22);
    view->verticalHeader()->setMinimumSectionSize(18);
    view->verticalHeader()->hide();
    view->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    view->horizontalHeader()->setSectionsMovable(true);
    view->horizontalHeader()->setHighlightSections(false);
    view->horizontalHeader()->setStretchLastSection(false);
    view->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    view->horizontalHeader()->setSectionResizeMode(ui::track_title_column, QHeaderView::Stretch);
    view->setColumnWidth(ui::track_marker_column, 180);
    view->setColumnWidth(ui::track_album_column, 220);
    view->setColumnWidth(ui::track_date_column, 72);
    view->setColumnWidth(ui::track_number_column, 60);
    view->setColumnWidth(ui::track_length_column, 72);
    view->setDragEnabled(true);
    view->setAcceptDrops(true);
    view->setDropIndicatorShown(true);
    view->setDragDropOverwriteMode(false);
    view->setDragDropMode(QAbstractItemView::DragDrop);
    view->setDefaultDropAction(Qt::MoveAction);
    view->setActivateCallback([this, id](const QModelIndex& index) {
        auto* tab = tabForDocument(id);
        if (tab != nullptr && index.isValid()) {
            playRow(*tab, index.row());
        }
    });
    connect(view, &QTableView::doubleClicked, this, [this, id](const QModelIndex& index) {
        auto* tab = tabForDocument(id);
        if (tab != nullptr && index.isValid()) {
            playRow(*tab, index.row());
        }
    });
    view->setReorderCallback([this, id](const QVariantList& rows, const int insertion_row) {
        auto* tab = tabForDocument(id);
        if (tab == nullptr) {
            return;
        }
        std::vector<int> row_indexes;
        row_indexes.reserve(static_cast<std::size_t>(rows.size()));
        for (const auto& row : rows) {
            row_indexes.push_back(row.toInt());
        }
        tab->model->reorderRows(std::move(row_indexes), insertion_row);
        markTabDirty(*tab);
    });
    view->setExternalDropCallback([this, id](QTableView* source, const QVariantList& rows,
                                             const int insertion_row, const Qt::DropAction action) {
        return transferRows(source, rows, id, action == Qt::MoveAction, insertion_row);
    });
    view->setLocalUrlDropCallback([this, id](const QList<QUrl>& urls, const int insertion_row) {
        std::vector<std::string> raw_paths;
        raw_paths.reserve(static_cast<std::size_t>(urls.size()));
        for (const auto& url : urls) {
            if (!url.isLocalFile()) {
                continue;
            }
            const auto encoded = QFile::encodeName(url.toLocalFile());
            raw_paths.emplace_back(encoded.constData(), static_cast<std::size_t>(encoded.size()));
        }
        if (raw_paths.empty()) {
            return false;
        }
        startDiscovery(std::move(raw_paths), id, insertion_row);
        return true;
    });

    const auto index = tabs_->addTab(view, displayText(document.name) +
                                               (document.dirty ? QStringLiteral(" *") : QString{}));
    auto tab = std::make_unique<ListTab>();
    tab->document = std::move(document);
    tab->model = model;
    tab->view = view;
    auto* raw_tab = tab.release();
    view->setProperty("bench-tab-pointer", QVariant::fromValue<void*>(raw_tab));
    if (select) {
        tabs_->setCurrentIndex(index);
        view->setFocus(Qt::ShortcutFocusReason);
    }
    enqueueUnprobedRows(*raw_tab);
    return raw_tab;
}

void BenchMainWindow::enqueueUnprobedRows(ListTab& tab) {
    const auto id = QString::fromStdString(tab.document.id.to_string());
    const auto& rows = tab.model->rows();
    for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
        if (!rows[static_cast<std::size_t>(row)].probed) {
            probe_queue_.push_back(ProbeJob{
                .document_id = id,
                .raw_path = rows[static_cast<std::size_t>(row)].raw_path,
                .hint_row = row,
            });
        }
    }
    pumpProbeQueue();
}

void BenchMainWindow::pumpProbeQueue() {
    if (probe_running_ || probe_queue_.empty()) {
        return;
    }
    std::vector<ProbeJob> batch;
    batch.reserve(probe_batch_size);
    while (!probe_queue_.empty() && batch.size() < probe_batch_size) {
        batch.push_back(std::move(probe_queue_.front()));
        probe_queue_.pop_front();
    }
    probe_running_ = true;
    connect(&probe_watcher_, &QFutureWatcher<std::vector<ProbeOutcome>>::finished, this,
            &BenchMainWindow::finishProbeBatch, Qt::SingleShotConnection);
    probe_watcher_.setFuture(QtConcurrent::run(
        [jobs = std::move(batch), cancellation = probe_cancellation_.token()]() mutable {
            std::vector<ProbeOutcome> outcomes;
            outcomes.reserve(jobs.size());
            for (auto& job : jobs) {
                if (cancellation.is_cancellation_requested()) {
                    break;
                }
                LocalTrackRow metadata;
                if (auto probe = formats::probe_local_media(job.raw_path, cancellation); probe) {
                    metadata.title = probed_tag(*probe, "title");
                    metadata.artist = probed_tag(*probe, "artist");
                    metadata.album = probed_tag(*probe, "album");
                    metadata.album_artist = probed_tag(*probe, "album_artist");
                    metadata.date = probed_tag(*probe, "date");
                    metadata.track_number = probed_tag(*probe, "track");
                    metadata.duration_ms = probe->duration_ms;
                }
                // A failed probe still marks the row probed so it is not
                // retried in a loop; the file-name fallback stays visible.
                outcomes.push_back(
                    ProbeOutcome{.job = std::move(job), .metadata = std::move(metadata)});
            }
            return outcomes;
        }));
}

void BenchMainWindow::finishProbeBatch() {
    probe_running_ = false;
    auto outcomes = probe_watcher_.result();
    bool applied = false;
    for (auto& outcome : outcomes) {
        auto* tab = tabForDocument(outcome.job.document_id);
        if (tab == nullptr) {
            continue;
        }
        applied = tab->model->applyMetadata(outcome.job.raw_path, outcome.job.hint_row,
                                            std::move(outcome.metadata)) ||
                  applied;
    }
    if (applied) {
        schedulePersist();
    }
    pumpProbeQueue();
}

BenchMainWindow::ListTab* BenchMainWindow::currentListTab() {
    auto* view = qobject_cast<QTableView*>(tabs_->currentWidget());
    if (view == nullptr) {
        return nullptr;
    }
    return static_cast<ListTab*>(view->property("bench-tab-pointer").value<void*>());
}

BenchMainWindow::ListTab* BenchMainWindow::tabForDocument(const QString& document_id) {
    for (int index = 0; index < tabs_->count(); ++index) {
        auto* view = qobject_cast<QTableView*>(tabs_->widget(index));
        if (view != nullptr && view->property("bench-document-id").toString() == document_id) {
            return static_cast<ListTab*>(view->property("bench-tab-pointer").value<void*>());
        }
    }
    return nullptr;
}

bool BenchMainWindow::transferRows(QTableView* source, const QVariantList& rows,
                                   const QString& target_id, const bool move,
                                   const int insertion_row) {
    auto* target = tabForDocument(target_id);
    if (target == nullptr || source == nullptr) {
        return false;
    }
    auto* source_tab = static_cast<ListTab*>(source->property("bench-tab-pointer").value<void*>());
    if (source_tab == nullptr || source_tab == target) {
        return false;
    }
    std::vector<LocalTrackRow> transferred;
    std::vector<int> source_rows;
    transferred.reserve(static_cast<std::size_t>(rows.size()));
    source_rows.reserve(static_cast<std::size_t>(rows.size()));
    for (const auto& row : rows) {
        const auto row_index = row.toInt();
        if (row_index < 0 || row_index >= static_cast<int>(source_tab->model->rows().size())) {
            continue;
        }
        transferred.push_back(source_tab->model->rows()[static_cast<std::size_t>(row_index)]);
        source_rows.push_back(row_index);
    }
    if (transferred.empty()) {
        return false;
    }
    target->model->appendRows(std::move(transferred), insertion_row);
    markTabDirty(*target);
    if (move) {
        source_tab->model->removeRowIndexes(std::move(source_rows));
        markTabDirty(*source_tab);
    }
    return true;
}

void BenchMainWindow::markTabDirty(ListTab& tab) {
    tab.document.dirty = true;
    const auto index = tabs_->indexOf(tab.view);
    if (index >= 0) {
        tabs_->setTabText(index, displayText(tab.document.name) + QStringLiteral(" *"));
    }
    schedulePersist();
}

void BenchMainWindow::closeTabAt(const int index) {
    auto* view = qobject_cast<QTableView*>(tabs_->widget(index));
    if (view == nullptr) {
        return;
    }
    auto* tab = static_cast<ListTab*>(view->property("bench-tab-pointer").value<void*>());
    tabs_->removeTab(index);
    view->deleteLater();
    delete tab;
    if (tabs_->count() == 0) {
        addListTab(
            persistence::ListDocument{
                .id = core::StableId::random(),
                .kind = persistence::ListKind::scratch,
                .name = "Scratch",
                .pinned = false,
                .dirty = false,
                .items = {},
            },
            true);
    }
    schedulePersist();
}

void BenchMainWindow::createList() {
    bool accepted = false;
    const auto name =
        QInputDialog::getText(this, QStringLiteral("New list"), QStringLiteral("Name:"),
                              QLineEdit::Normal, QString{}, &accepted)
            .trimmed();
    if (!accepted || name.isEmpty()) {
        return;
    }
    addListTab(
        persistence::ListDocument{
            .id = core::StableId::random(),
            .kind = persistence::ListKind::saved,
            .name = utf8Bytes(name),
            .pinned = false,
            .dirty = false,
            .items = {},
        },
        true);
    schedulePersist();
}

void BenchMainWindow::removeSelectedRows() {
    auto* tab = currentListTab();
    if (tab == nullptr || tab->view->selectionModel() == nullptr) {
        return;
    }
    std::vector<int> rows;
    const auto selection = tab->view->selectionModel()->selectedRows();
    rows.reserve(static_cast<std::size_t>(selection.size()));
    for (const auto& index : selection) {
        rows.push_back(index.row());
    }
    if (rows.empty()) {
        return;
    }
    tab->model->removeRowIndexes(std::move(rows));
    markTabDirty(*tab);
}

void BenchMainWindow::openFilesDialog() {
    const auto files = QFileDialog::getOpenFileNames(this, QStringLiteral("Open files"));
    std::vector<std::string> raw_paths;
    raw_paths.reserve(static_cast<std::size_t>(files.size()));
    for (const auto& file : files) {
        const auto encoded = QFile::encodeName(file);
        raw_paths.emplace_back(encoded.constData(), static_cast<std::size_t>(encoded.size()));
    }
    openLocalPaths(std::move(raw_paths));
}

void BenchMainWindow::openFolderDialog() {
    const auto folder = QFileDialog::getExistingDirectory(this, QStringLiteral("Open folder"));
    if (folder.isEmpty()) {
        return;
    }
    const auto encoded = QFile::encodeName(folder);
    openLocalPaths({{encoded.constData(), static_cast<std::size_t>(encoded.size())}});
}

void BenchMainWindow::addFolderRoot() {
    const auto folder =
        QFileDialog::getExistingDirectory(this, QStringLiteral("Add library folder"));
    if (folder.isEmpty()) {
        return;
    }
    const auto encoded = QFile::encodeName(folder);
    folder_model_->addRoot({encoded.constData(), static_cast<std::size_t>(encoded.size())});
    QSettings settings;
    auto roots = settings.value(QStringLiteral("library/roots")).toList();
    roots.push_back(QByteArray{encoded.constData(), encoded.size()});
    settings.setValue(QStringLiteral("library/roots"), roots);
}

void BenchMainWindow::openLocalPaths(std::vector<std::string> raw_paths) {
    if (raw_paths.empty()) {
        return;
    }
    if (!lists_restored_) {
        pending_open_paths_.insert(pending_open_paths_.end(),
                                   std::make_move_iterator(raw_paths.begin()),
                                   std::make_move_iterator(raw_paths.end()));
        return;
    }
    auto* tab = currentListTab();
    if (tab == nullptr) {
        return;
    }
    startDiscovery(std::move(raw_paths), QString::fromStdString(tab->document.id.to_string()), -1);
}

void BenchMainWindow::startDiscovery(std::vector<std::string> raw_paths, QString target_document_id,
                                     const int insertion_row) {
    if (discovery_running_) {
        statusBar()->showMessage(QStringLiteral("A folder scan is already running"), 3'000);
        return;
    }
    discovery_running_ = true;
    discovery_target_document_ = std::move(target_document_id);
    discovery_insertion_row_ = insertion_row;
    connect(&discovery_watcher_, &QFutureWatcher<core::LocalSourceDiscovery>::finished, this,
            &BenchMainWindow::finishDiscovery, Qt::SingleShotConnection);
    discovery_watcher_.setFuture(QtConcurrent::run([paths = std::move(raw_paths),
                                                    cancellation = probe_cancellation_.token()] {
        return core::discover_local_sources(std::span{paths.data(), paths.size()}, cancellation);
    }));
}

void BenchMainWindow::finishDiscovery() {
    discovery_running_ = false;
    auto result = discovery_watcher_.result();
    auto* tab = tabForDocument(discovery_target_document_);
    if (tab == nullptr) {
        tab = currentListTab();
    }
    if (tab == nullptr) {
        return;
    }
    if (!result.raw_files.empty()) {
        tab->model->appendPaths(std::move(result.raw_files), discovery_insertion_row_);
        markTabDirty(*tab);
        enqueueUnprobedRows(*tab);
    }
    if (!result.issues.empty()) {
        statusBar()->showMessage(
            QStringLiteral("%1 entr%2 could not be opened")
                .arg(result.issues.size())
                .arg(result.issues.size() == 1U ? QStringLiteral("y") : QStringLiteral("ies")),
            5'000);
    }
    if (result.truncated) {
        statusBar()->showMessage(QStringLiteral("Folder scan hit the file limit"), 5'000);
    }
}

void BenchMainWindow::playRow(ListTab& tab, const int row) {
    if (player_ == nullptr) {
        return;
    }
    const auto raw = tab.model->rawPath(row);
    if (raw.empty()) {
        return;
    }
    if (auto result = player_->load_and_play(raw); !result) {
        statusBar()->showMessage(
            QStringLiteral("Playback failed: %1").arg(displayText(result.error().message)), 5'000);
        return;
    }
    const auto id = QString::fromStdString(tab.document.id.to_string());
    if (playback_document_id_ != id) {
        if (auto* previous = tabForDocument(playback_document_id_); previous != nullptr) {
            previous->model->setCurrentPath({}, -1);
        }
    }
    playback_document_id_ = id;
    playback_row_ = row;
    playback_path_ = raw;
    advance_pending_ = false;
    tab.model->setCurrentPath(raw, row);
}

std::optional<std::pair<int, std::string>>
BenchMainWindow::adjacentPlaybackRow(const int direction) {
    auto* tab = tabForDocument(playback_document_id_);
    if (tab == nullptr || playback_path_.empty()) {
        return std::nullopt;
    }
    const auto row = tab->model->rowOfPath(playback_path_, playback_row_);
    if (row < 0) {
        return std::nullopt;
    }
    const auto adjacent = row + direction;
    if (adjacent < 0 || adjacent >= tab->model->rowCount()) {
        return std::nullopt;
    }
    return std::make_pair(adjacent, tab->model->rawPath(adjacent));
}

void BenchMainWindow::playAdjacent(const int direction) {
    auto* tab = tabForDocument(playback_document_id_);
    const auto next = adjacentPlaybackRow(direction);
    if (tab == nullptr || !next || player_ == nullptr) {
        return;
    }
    if (auto result = player_->load_and_play(next->second); result) {
        playback_row_ = next->first;
        playback_path_ = next->second;
        advance_pending_ = false;
        tab->model->setCurrentPath(next->second, next->first);
    } else {
        statusBar()->showMessage(
            QStringLiteral("Playback failed: %1").arg(displayText(result.error().message)), 5'000);
    }
}

void BenchMainWindow::togglePlayPause() {
    if (player_ == nullptr) {
        return;
    }
    const auto snapshot = player_->snapshot();
    if (playerActive(snapshot.state)) {
        static_cast<void>(player_->pause());
    } else {
        static_cast<void>(player_->play());
    }
}

void BenchMainWindow::seekToMs(const qint64 position_ms) {
    if (player_ == nullptr) {
        return;
    }
    const auto snapshot = player_->snapshot();
    if (!snapshot.format || snapshot.format->sample_rate <= 0) {
        return;
    }
    static_cast<void>(player_->seek_to_sample(position_ms * snapshot.format->sample_rate / 1'000));
}

void BenchMainWindow::refreshTransport() {
    if (player_ == nullptr) {
        for (auto* action : {previous_action_, play_pause_action_, stop_action_, next_action_}) {
            action->setEnabled(false);
        }
        seek_->setEnabled(false);
        volume_->setEnabled(false);
        device_box_->setEnabled(false);
        return;
    }
    const auto snapshot = player_->snapshot();

    // List progression (ADR-0023): a finished track advances once to the next
    // row of its originating list; the end of the list stays "Ended".
    if (snapshot.state == audio::LocalAuditionState::ended) {
        if (!advance_pending_) {
            advance_pending_ = true;
            if (const auto next = adjacentPlaybackRow(1)) {
                if (auto result = player_->load_and_play(next->second); result) {
                    playback_row_ = next->first;
                    playback_path_ = next->second;
                    advance_pending_ = false;
                    if (auto* tab = tabForDocument(playback_document_id_); tab != nullptr) {
                        tab->model->setCurrentPath(next->second, next->first);
                    }
                }
            }
        }
    } else if (snapshot.state == audio::LocalAuditionState::empty) {
        if (!playback_document_id_.isEmpty()) {
            if (auto* tab = tabForDocument(playback_document_id_); tab != nullptr) {
                tab->model->setCurrentPath({}, -1);
            }
            playback_document_id_.clear();
            playback_row_ = -1;
            playback_path_.clear();
        }
        advance_pending_ = false;
    } else if (snapshot.state != audio::LocalAuditionState::loading) {
        advance_pending_ = false;
    }

    const auto error = snapshot.error ? displayText(snapshot.error->message) : QString{};
    if (!error.isEmpty() && error != last_player_error_) {
        last_player_error_ = error;
        statusBar()->showMessage(QStringLiteral("Playback failed: %1").arg(error), 5'000);
    } else if (error.isEmpty()) {
        last_player_error_.clear();
    }

    const bool active = playerActive(snapshot.state);
    const bool source_ready = snapshot.format.has_value() &&
                              snapshot.state != audio::LocalAuditionState::loading &&
                              snapshot.state != audio::LocalAuditionState::failed;
    previous_action_->setEnabled(adjacentPlaybackRow(-1).has_value());
    next_action_->setEnabled(adjacentPlaybackRow(1).has_value());
    play_pause_action_->setEnabled(source_ready);
    play_pause_action_->setText(active ? QStringLiteral("Pause") : QStringLiteral("Play"));
    play_pause_action_->setIcon(
        style()->standardIcon(active ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
    stop_action_->setEnabled(source_ready);

    if (snapshot.state == audio::LocalAuditionState::empty) {
        now_playing_->clear();
        now_playing_->setToolTip({});
    } else {
        const auto slash = snapshot.raw_path.find_last_of('/');
        const auto name = slash == std::string::npos || slash + 1U >= snapshot.raw_path.size()
                              ? snapshot.raw_path
                              : snapshot.raw_path.substr(slash + 1U);
        now_playing_->setText(QString::fromStdString(core::escape_raw_path(name)));
        now_playing_->setToolTip(QString::fromStdString(core::escape_raw_path(snapshot.raw_path)));
    }

    qint64 position_ms = 0;
    qint64 duration_ms = 0;
    if (snapshot.format && snapshot.format->sample_rate > 0) {
        position_ms = snapshot.position_sample * 1'000 / snapshot.format->sample_rate;
        if (snapshot.end_sample) {
            duration_ms = *snapshot.end_sample * 1'000 / snapshot.format->sample_rate;
        }
    }
    elapsed_->setText(formatTime(position_ms));
    duration_->setText(formatTime(duration_ms));
    const auto bounded = std::clamp<qint64>(duration_ms, 0, std::numeric_limits<int>::max());
    seek_->setEnabled(source_ready && bounded > 0);
    seek_->setRange(0, static_cast<int>(bounded));
    if (!seeking_) {
        const QSignalBlocker blocker{seek_};
        seek_->setValue(
            static_cast<int>(std::clamp<qint64>(position_ms, 0, std::numeric_limits<int>::max())));
    }
    volume_->setEnabled(true);
    if (!changing_volume_) {
        const QSignalBlocker blocker{volume_};
        volume_->setValue(snapshot.volume_percent);
    }

    std::vector<std::string> names;
    names.reserve(snapshot.devices.size());
    for (const auto& device : snapshot.devices) {
        names.push_back(device.name);
    }
    if (names != device_names_) {
        device_names_ = std::move(names);
        const QSignalBlocker blocker{device_box_};
        device_box_->clear();
        device_box_->addItem(QStringLiteral("System default"));
        for (const auto& device : snapshot.devices) {
            device_box_->addItem(
                displayText(device.description.empty() ? device.name : device.description));
        }
    }
    const QSignalBlocker blocker{device_box_};
    if (snapshot.output_target) {
        const auto found = std::ranges::find(device_names_, *snapshot.output_target);
        device_box_->setCurrentIndex(
            found == device_names_.end()
                ? 0
                : static_cast<int>(std::distance(device_names_.begin(), found)) + 1);
    } else {
        device_box_->setCurrentIndex(0);
    }
}

void BenchMainWindow::closeEvent(QCloseEvent* event) {
    probe_cancellation_.request_cancellation();
    probe_queue_.clear();
    if (probe_running_) {
        probe_watcher_.waitForFinished();
    }
    if (discovery_running_) {
        discovery_watcher_.waitForFinished();
    }
    persistNow(true);
    if (transport_timer_ != nullptr) {
        transport_timer_->stop();
    }
    player_ = nullptr;
    player_storage_.reset();
    event->accept();
}

void BenchMainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void BenchMainWindow::dropEvent(QDropEvent* event) {
    std::vector<std::string> raw_paths;
    const auto urls = event->mimeData()->urls();
    raw_paths.reserve(static_cast<std::size_t>(urls.size()));
    for (const auto& url : urls) {
        if (!url.isLocalFile()) {
            continue;
        }
        const auto encoded = QFile::encodeName(url.toLocalFile());
        raw_paths.emplace_back(encoded.constData(), static_cast<std::size_t>(encoded.size()));
    }
    if (raw_paths.empty()) {
        return;
    }
    event->acceptProposedAction();
    openLocalPaths(std::move(raw_paths));
}

} // namespace trackknife::bench
