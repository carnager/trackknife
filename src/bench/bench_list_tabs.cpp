// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "bench/metadata_properties_dialog.hpp"
#include "quick/mpd_probe_controller.hpp"
#include "quick/mpd_queue_model.hpp"
#include "trackknife/metadata/flac_mapping.hpp"
#include "ui/server_library_tree_view.hpp"
#include "uicommon/list_persistence_service.hpp"
#include "uicommon/local_folder_tree_model.hpp"
#include "uicommon/queue_item_delegate.hpp"
#include "uicommon/queue_table_view.hpp"
#include "uicommon/track_row_roles.hpp"
#include "uicommon/track_view_layout.hpp"

#include <QAbstractItemView>
#include <QAction>
#include <QDir>
#include <QFile>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QTreeView>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

namespace trackknife::bench {
namespace {

constexpr int persist_debounce_ms = 1'000;

} // namespace

void BenchMainWindow::initializePersistence() {
    const auto base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(base);
    database_path_ = std::filesystem::path{utf8Bytes(base + QStringLiteral("/lists.sqlite"))};
    persistence_ = new ui::ListPersistenceService(database_path_, this);
    persistence_timer_ = new QTimer(this);
    persistence_timer_->setSingleShot(true);
    persistence_timer_->setInterval(persist_debounce_ms);
    connect(persistence_timer_, &QTimer::timeout, this, [this] { persistNow(false); });
    persistence_->initialize([this](ui::PersistedWorkspace workspace, QString error) {
        if (!error.isEmpty()) {
            statusBar()->showMessage(QStringLiteral("List restore failed: %1").arg(error), 5'000);
        }
        restored_track_view_layouts_.clear();
        for (const auto& preset : workspace.view_presets) {
            restored_track_view_layouts_.insert(
                displayText(preset.binding),
                QByteArray{preset.header_state.data(),
                           static_cast<qsizetype>(preset.header_state.size())});
        }
        mpd_profiles_ = std::move(workspace.profiles);
        auto stored_mpd_layout = restored_track_view_layouts_.value(QStringLiteral("mpd:queue"));
        if (stored_mpd_layout.isEmpty()) {
            stored_mpd_layout = restored_track_view_layouts_.value(QStringLiteral("live-queue"));
        }
        if (!stored_mpd_layout.isEmpty()) {
            QString layout_error;
            if (auto decoded = ui::deserializeTrackViewLayout(stored_mpd_layout, trackColumnIds(),
                                                              &layout_error);
                decoded) {
                applyTrackViewLayout(mpd_queue_view_, mpd_view_layout_, *decoded);
            } else {
                mpd_view_layout_persistence_protected_ = true;
                preserved_mpd_view_layout_ = stored_mpd_layout;
                statusBar()->showMessage(
                    QStringLiteral("MPD track layout was not loaded (%1); the saved value was "
                                   "preserved")
                        .arg(layout_error),
                    7'000);
            }
        }
        restoreLists(std::move(workspace.lists));
        autoConnectMpd();
        startMetadataOperationRecovery();
    });
}

void BenchMainWindow::restoreLists(std::vector<persistence::ListDocument> documents) {
    lists_restored_ = true;
    for (auto& document : documents) {
        addListTab(std::move(document), false);
    }
    if (list_tabs_.empty()) {
        addListTab(
            persistence::ListDocument{
                .id = core::StableId::random(),
                .kind = persistence::ListKind::scratch,
                .name = "Local Queue",
                .pinned = false,
                .dirty = false,
                .items = {},
            },
            true);
    } else {
        tabs_->setCurrentWidget(list_tabs_.front()->view);
    }
    if (!pending_open_paths_.empty()) {
        auto pending = std::exchange(pending_open_paths_, std::vector<std::string>{});
        openLocalPaths(std::move(pending));
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
                .logical_reference = row.logical_reference,
                .segment = row.segment ? std::optional{persistence::ListItemSegment{
                                             .start_sample = row.segment->start_sample,
                                             .end_sample = row.segment->end_sample,
                                         }}
                                       : std::nullopt,
                .source_selection = row.selection.stream_index || row.selection.subsong_index
                                        ? std::optional{persistence::ListItemSourceSelection{
                                              .audio_stream_index = row.selection.stream_index,
                                              .subsong_index = row.selection.subsong_index,
                                          }}
                                        : std::nullopt,
                .duration_ms = row.duration_ms,
                .source_revision = row.source_revision,
                .fields = {},
            };
            if (!row.metadata.fields.empty()) {
                // This remains a presentation cache, but retaining layers is
                // necessary so a verified embedded refresh cannot erase CUE,
                // chapter, or sidecar projections for the same physical file.
                for (const auto& field : row.metadata.fields) {
                    if (field.canonical_name.empty()) {
                        continue;
                    }
                    for (const auto& value : field.values) {
                        item.fields.push_back({
                            .name = field.canonical_name,
                            .value = value,
                            .native_name = field.native_name,
                            .provenance = field.provenance,
                            .language = field.qualifier.language,
                            .description = field.qualifier.description,
                        });
                    }
                }
            } else {
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
            }
            document.items.push_back(std::move(item));
        }
        documents.push_back(std::move(document));
    }
    return documents;
}

std::vector<persistence::TrackViewPreset> BenchMainWindow::collectTrackViewLayouts() {
    std::vector<persistence::TrackViewPreset> layouts;
    layouts.reserve(list_tabs_.size() + 1U);
    const auto mpd_bytes = mpd_view_layout_persistence_protected_
                               ? preserved_mpd_view_layout_
                               : ui::serializeTrackViewLayout(
                                     captureTrackViewLayout(mpd_queue_view_, mpd_view_layout_));
    layouts.push_back(persistence::TrackViewPreset{
        .binding = "mpd:queue",
        .header_state =
            std::string{mpd_bytes.constData(), static_cast<std::size_t>(mpd_bytes.size())},
    });
    for (const auto& tab : list_tabs_) {
        const auto id = QString::fromStdString(tab->document.id.to_string());
        const auto bytes = tab->view_layout_persistence_protected
                               ? tab->preserved_view_layout
                               : ui::serializeTrackViewLayout(captureTrackViewLayout(*tab));
        layouts.push_back(persistence::TrackViewPreset{
            .binding = utf8Bytes(QStringLiteral("local:%1").arg(id)),
            .header_state = std::string{bytes.constData(), static_cast<std::size_t>(bytes.size())},
        });
    }
    return layouts;
}

void BenchMainWindow::persistNow(const bool wait) {
    if (persistence_ == nullptr) {
        return;
    }
    auto documents = collectDocuments();
    auto view_layouts = collectTrackViewLayouts();
    if (wait) {
        const auto error =
            persistence_->saveWorkspaceAndWait(std::move(documents), std::move(view_layouts));
        if (!error.isEmpty()) {
            statusBar()->showMessage(QStringLiteral("List save failed: %1").arg(error), 5'000);
        }
        return;
    }
    persistence_->saveWorkspace(
        std::move(documents), std::move(view_layouts), [this](QString error) {
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
        row.logical_reference = item.logical_reference;
        if (item.source_selection) {
            row.selection = formats::AudioSourceSelection{
                .stream_index = item.source_selection->audio_stream_index,
                .subsong_index = item.source_selection->subsong_index,
            };
        }
        if (item.segment) {
            row.segment = formats::SampleRange{.start_sample = item.segment->start_sample,
                                               .end_sample = item.segment->end_sample};
        }
        row.duration_ms = item.duration_ms;
        row.source_revision = item.source_revision;
        for (const auto& field : item.fields) {
            const auto canonical_name =
                field.name.empty()
                    ? metadata::resolve_text_property_identity(field.native_name).canonical_name
                    : field.name;
            if (!canonical_name.empty()) {
                row.metadata.fields.push_back(metadata::MetadataField{
                    .canonical_name = canonical_name,
                    .native_name = field.native_name.empty() ? field.name : field.native_name,
                    .values = {field.value},
                    .qualifier =
                        metadata::FieldQualifier{
                            .language = field.language,
                            .description = field.description,
                        },
                    .provenance = field.provenance,
                });
            }
        }
        remove_shadowed_probed_metadata(row.metadata);
        project_display_metadata(row);
        row.probed = row.selection.stream_index.has_value() ||
                     row.selection.subsong_index.has_value() || row.segment.has_value() ||
                     row.duration_ms.has_value() || !item.fields.empty();
        restored_rows.push_back(std::move(row));
    }
    model->replaceRows(std::move(restored_rows));

    auto* view = new ui::QueueTableView(tabs_);
    view->setObjectName(QStringLiteral("bench-list-%1").arg(id.left(8)));
    view->setProperty("bench-document-id", id);
    view->setModel(model);
    connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] { refreshSelectionStatus(); });
    connect(model, &QAbstractItemModel::dataChanged, this, [this] { refreshSelectionStatus(); });
    connect(model, &QAbstractItemModel::rowsInserted, this, [this] { refreshSelectionStatus(); });
    connect(model, &QAbstractItemModel::rowsRemoved, this, [this] { refreshSelectionStatus(); });
    connect(model, &QAbstractItemModel::modelReset, this, [this] { refreshSelectionStatus(); });
    view->setProperty("trackknife-hover-row", -1);
    view->setAlternatingRowColors(true);
    view->setShowGrid(false);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setWordWrap(false);
    view->setTextElideMode(Qt::ElideRight);
    view->verticalHeader()->setDefaultSectionSize(22);
    view->verticalHeader()->setMinimumSectionSize(18);
    view->verticalHeader()->hide();
    view->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    view->horizontalHeader()->setSectionsMovable(true);
    view->horizontalHeader()->setHighlightSections(false);
    view->horizontalHeader()->setStretchLastSection(false);
    view->horizontalHeader()->setMinimumSectionSize(24);
    view->horizontalHeader()->setMaximumSectionSize(4'096);
    view->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    view->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view->horizontalHeader(), &QWidget::customContextMenuRequested, this,
            [this, view](const QPoint& position) { showTrackViewHeaderMenu(view, position); });
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view, &QWidget::customContextMenuRequested, this,
            [this, view](const QPoint& position) { showTrackContextMenu(view, position); });
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
    view->setExternalDropCallback([this, id](QAbstractItemView* source, const QVariantList& rows,
                                             const int insertion_row, const Qt::DropAction action) {
        auto* table = qobject_cast<QTableView*>(source);
        return table != nullptr &&
               transferRows(table, rows, id, action == Qt::MoveAction, insertion_row);
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

    const auto index = tabs_->addTab(view, displayText(document.name));
    auto tab = std::make_unique<ListTab>();
    tab->document = std::move(document);
    tab->model = model;
    tab->view = view;
    auto* raw_tab = tab.get();
    list_tabs_.push_back(std::move(tab));
    view->setProperty("bench-tab-pointer", QVariant::fromValue<void*>(raw_tab));
    auto layout = defaultTrackViewLayout();
    const auto binding = QStringLiteral("local:%1").arg(id);
    if (const auto stored = restored_track_view_layouts_.value(binding); !stored.isEmpty()) {
        QString layout_error;
        if (auto decoded = ui::deserializeTrackViewLayout(stored, trackColumnIds(), &layout_error);
            decoded) {
            layout = std::move(*decoded);
        } else {
            raw_tab->view_layout_persistence_protected = true;
            raw_tab->preserved_view_layout = stored;
            statusBar()->showMessage(
                QStringLiteral("Track layout was not loaded (%1); the saved value was preserved")
                    .arg(layout_error),
                7'000);
        }
    }
    applyTrackViewLayout(*raw_tab, layout);
    connect(view->horizontalHeader(), &QHeaderView::sectionMoved, this,
            [this, id](const int, const int, const int) {
                if (applying_track_view_layout_) {
                    return;
                }
                auto* moved_tab = tabForDocument(id);
                if (moved_tab == nullptr) {
                    return;
                }
                moved_tab->view_layout = captureTrackViewLayout(*moved_tab);
                moved_tab->view_layout_persistence_protected = false;
                moved_tab->preserved_view_layout.clear();
                schedulePersist();
                refreshTrackViewActions();
            });
    connect(view->horizontalHeader(), &QHeaderView::sectionResized, this,
            [this, id](const int, const int, const int) {
                if (applying_track_view_layout_) {
                    return;
                }
                auto* resized_tab = tabForDocument(id);
                if (resized_tab == nullptr) {
                    return;
                }
                resized_tab->view_layout = captureTrackViewLayout(*resized_tab);
                resized_tab->view_layout_persistence_protected = false;
                resized_tab->preserved_view_layout.clear();
                schedulePersist();
            });
    refreshTabChrome(*raw_tab);
    if (select) {
        tabs_->setCurrentIndex(index);
        view->setFocus(Qt::ShortcutFocusReason);
    }
    enqueueUnprobedRows(*raw_tab);
    syncArtwork(*raw_tab);
    refreshTabActions();
    refreshSelectionStatus();
    return raw_tab;
}

BenchMainWindow::ListTab* BenchMainWindow::currentListTab() {
    auto* view = qobject_cast<QTableView*>(tabs_->currentWidget());
    if (view == nullptr) {
        return nullptr;
    }
    return static_cast<ListTab*>(view->property("bench-tab-pointer").value<void*>());
}

bool BenchMainWindow::isMpdContext() const {
    return tabs_ != nullptr && mpd_queue_view_ != nullptr &&
           tabs_->currentWidget() == mpd_queue_view_;
}

void BenchMainWindow::refreshActiveContext() {
    const auto mpd = isMpdContext();
    const auto authority = mpd ? QStringLiteral("mpd") : QStringLiteral("local");
    const auto context_changed = property("trackbench-active-authority").toString() != authority;
    setProperty("trackbench-active-authority", authority);
    if (source_stack_ != nullptr) {
        auto* source =
            mpd ? static_cast<QWidget*>(server_library_view_) : static_cast<QWidget*>(folder_view_);
        if (source != nullptr) {
            source_stack_->setCurrentWidget(source);
        }
    }
    if (source_heading_ != nullptr) {
        source_heading_->setText(mpd ? QStringLiteral("MPD Library") : QStringLiteral("Folders"));
    }
    if (connect_mpd_action_ != nullptr) {
        connect_mpd_action_->setEnabled(!mpd_controller_->busy());
    }
    if (disconnect_mpd_action_ != nullptr) {
        disconnect_mpd_action_->setEnabled(mpd_controller_->connected());
    }
    if (buffer_menu_ != nullptr) {
        buffer_menu_->setEnabled(!mpd);
    }
    if (context_changed && device_menu_ != nullptr) {
        rebuildDeviceMenu();
    }
    refreshMpdStatusControls();
    if (seek_ != nullptr) {
        refreshTransport();
    }
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
    syncArtwork(*target);
    if (move) {
        source_tab->model->removeRowIndexes(std::move(source_rows));
        markTabDirty(*source_tab);
    }
    return true;
}

void BenchMainWindow::markTabDirty(ListTab& tab) {
    tab.document.dirty = true;
    refreshTabChrome(tab);
    schedulePersist();
}

void BenchMainWindow::refreshTabChrome(ListTab& tab) {
    const auto index = tabs_->indexOf(tab.view);
    if (index < 0) {
        return;
    }
    const auto name = displayText(tab.document.name);
    tabs_->setTabText(index, name + (tab.document.dirty ? QStringLiteral(" *") : QString{}));
    const auto kind = tab.document.kind == persistence::ListKind::scratch
                          ? QStringLiteral("Persistent scratch list")
                          : QStringLiteral("Named Trackbench working list");
    tabs_->setTabToolTip(index,
                         QStringLiteral("%1%2%3").arg(
                             kind, tab.document.pinned ? QStringLiteral(" · pinned") : QString{},
                             tab.document.dirty ? QStringLiteral(" · modified") : QString{}));
    tab.view->setAccessibleName(QStringLiteral("%1 track list").arg(name));
    if (auto* close = tabs_->tabBar()->tabButton(index, QTabBar::RightSide)) {
        close->setVisible(!tab.document.pinned);
    }
}

void BenchMainWindow::refreshTabActions() {
    const auto* tab = currentListTab();
    const bool available = tab != nullptr;
    for (auto* action :
         {duplicate_tab_action_, pin_tab_action_, save_tab_action_, rename_tab_action_}) {
        if (action != nullptr) {
            action->setEnabled(available);
        }
    }
    if (pin_tab_action_ != nullptr) {
        const QSignalBlocker blocker{pin_tab_action_};
        pin_tab_action_->setChecked(available && tab->document.pinned);
    }
    if (close_tab_action_ != nullptr) {
        const auto properties_tab =
            qobject_cast<MetadataPropertiesDialog*>(tabs_->currentWidget()) != nullptr;
        close_tab_action_->setEnabled(properties_tab || (available && !tab->document.pinned));
    }
}

void BenchMainWindow::closeTabAt(const int index) {
    if (auto* properties = qobject_cast<MetadataPropertiesDialog*>(tabs_->widget(index))) {
        properties->close();
        return;
    }
    auto* view = qobject_cast<QTableView*>(tabs_->widget(index));
    if (view == nullptr) {
        return;
    }
    if (view == mpd_queue_view_) {
        statusBar()->showMessage(QStringLiteral("MPD Queue is a permanent authority tab"), 3'000);
        return;
    }
    auto* tab = static_cast<ListTab*>(view->property("bench-tab-pointer").value<void*>());
    if (tab == nullptr || tab->document.pinned) {
        statusBar()->showMessage(QStringLiteral("Unpin this list before closing it"), 3'000);
        return;
    }
    if (tab->document.dirty) {
        QMessageBox confirmation{
            QMessageBox::Question,
            QStringLiteral("Close unsaved list"),
            QStringLiteral("Discard the unsaved contents of “%1”?")
                .arg(displayText(tab->document.name)),
            QMessageBox::Yes | QMessageBox::No,
            this,
        };
        confirmation.setOption(QMessageBox::Option::DontUseNativeDialog);
        confirmation.setDefaultButton(QMessageBox::No);
        if (confirmation.exec() != QMessageBox::Yes) {
            return;
        }
    }
    tabs_->removeTab(index);
    view->deleteLater();
    std::erase_if(list_tabs_,
                  [tab](const std::unique_ptr<ListTab>& owned) { return owned.get() == tab; });
    if (list_tabs_.empty()) {
        addListTab(
            persistence::ListDocument{
                .id = core::StableId::random(),
                .kind = persistence::ListKind::scratch,
                .name = "Local Queue",
                .pinned = false,
                .dirty = false,
                .items = {},
            },
            true);
    }
    schedulePersist();
    refreshTabActions();
}

void BenchMainWindow::closeCurrentTab() { closeTabAt(tabs_->currentIndex()); }

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

void BenchMainWindow::duplicateCurrentTab() {
    auto* tab = currentListTab();
    if (tab == nullptr) {
        return;
    }
    auto documents = collectDocuments();
    const auto found =
        std::ranges::find(documents, tab->document.id, &persistence::ListDocument::id);
    if (found == documents.end()) {
        return;
    }
    auto duplicate = *found;
    duplicate.id = core::StableId::random();
    duplicate.name = utf8Bytes(QStringLiteral("%1 copy").arg(displayText(found->name)));
    duplicate.pinned = false;
    duplicate.dirty = true;
    auto* duplicated_tab = addListTab(std::move(duplicate), true);
    if (duplicated_tab != nullptr) {
        applyTrackViewLayout(*duplicated_tab, captureTrackViewLayout(*tab));
    }
    schedulePersist();
}

void BenchMainWindow::toggleCurrentTabPinned() {
    auto* tab = currentListTab();
    if (tab == nullptr) {
        return;
    }
    tab->document.pinned = !tab->document.pinned;
    refreshTabChrome(*tab);
    refreshTabActions();
    schedulePersist();
}

void BenchMainWindow::saveCurrentList() {
    auto* tab = currentListTab();
    if (tab == nullptr) {
        return;
    }
    if (tab->document.kind == persistence::ListKind::scratch) {
        bool accepted = false;
        const auto name = QInputDialog::getText(this, QStringLiteral("Save working list"),
                                                QStringLiteral("Name:"), QLineEdit::Normal,
                                                displayText(tab->document.name), &accepted)
                              .trimmed();
        if (!accepted || name.isEmpty()) {
            return;
        }
        tab->document.name = utf8Bytes(name);
        tab->document.kind = persistence::ListKind::saved;
    }
    tab->document.dirty = false;
    refreshTabChrome(*tab);
    schedulePersist();
}

void BenchMainWindow::renameCurrentList() {
    auto* tab = currentListTab();
    if (tab == nullptr) {
        return;
    }
    const auto current_name = displayText(tab->document.name);
    bool accepted = false;
    const auto name =
        QInputDialog::getText(this, QStringLiteral("Rename list"), QStringLiteral("Name:"),
                              QLineEdit::Normal, current_name, &accepted)
            .trimmed();
    if (!accepted || name.isEmpty() || name == current_name) {
        return;
    }
    tab->document.name = utf8Bytes(name);
    markTabDirty(*tab);
}

void BenchMainWindow::showTabContextMenu(const QPoint& position) {
    const auto index = tabs_->tabBar()->tabAt(position);
    if (index < 0) {
        return;
    }
    tabs_->setCurrentIndex(index);
    refreshTabActions();
    tab_context_menu_->popup(tabs_->tabBar()->mapToGlobal(position));
}

void BenchMainWindow::showTrackContextMenu(QTableView* view, const QPoint& position) {
    if (view == nullptr || view->selectionModel() == nullptr || track_context_menu_ == nullptr) {
        return;
    }
    const auto target = view->indexAt(position);
    if (!target.isValid()) {
        return;
    }
    tabs_->setCurrentWidget(view);

    const auto* grouped_delegate = qobject_cast<const ui::QueueItemDelegate*>(view->itemDelegate());
    const auto relative_y = position.y() - view->visualRect(target).top();
    if (grouped_delegate != nullptr && grouped_delegate->isAlbumHeaderHit(target, relative_y)) {
        const auto [first, last] = grouped_delegate->albumRowRange(target);
        const QItemSelection album{view->model()->index(first, 0),
                                   view->model()->index(last, view->model()->columnCount() - 1)};
        view->selectionModel()->select(album, QItemSelectionModel::ClearAndSelect |
                                                  QItemSelectionModel::Rows);
        view->selectionModel()->setCurrentIndex(view->model()->index(first, local_title_column),
                                                QItemSelectionModel::NoUpdate);
    } else {
        if (!view->selectionModel()->isRowSelected(target.row(), target.parent())) {
            view->selectionModel()->select(target, QItemSelectionModel::ClearAndSelect |
                                                       QItemSelectionModel::Rows);
        }
        view->selectionModel()->setCurrentIndex(target, QItemSelectionModel::NoUpdate);
    }
    refreshSelectionStatus();

    const auto mpd_queue = view == mpd_queue_view_;
    const auto has_selection = !view->selectionModel()->selectedRows().isEmpty();
    const auto command_ready =
        !mpd_queue || (mpd_controller_->connected() && !mpd_controller_->commandBusy());
    track_context_menu_->clear();
    play_selected_action_->setEnabled(view->currentIndex().isValid() && command_ready);
    remove_selected_action_->setEnabled(has_selection && command_ready);
    track_context_menu_->addAction(play_selected_action_);
    if (mpd_queue) {
        const auto has_uris = !selectedMpdQueueUris().isEmpty();
        mpd_add_next_selection_action_->setEnabled(command_ready && has_uris);
        mpd_append_selection_action_->setEnabled(command_ready && has_uris);
        mpd_crop_selection_action_->setEnabled(command_ready && has_selection);
        track_context_menu_->addAction(mpd_add_next_selection_action_);
        track_context_menu_->addAction(mpd_append_selection_action_);
        track_context_menu_->addSeparator();
        track_context_menu_->addAction(remove_selected_action_);
        track_context_menu_->addAction(mpd_crop_selection_action_);
        refreshMpdPriorityMenu();
        track_context_menu_->addMenu(mpd_priority_menu_);
        track_context_menu_->popup(view->viewport()->mapToGlobal(position));
        return;
    }

    track_context_menu_->addAction(properties_action_);

    auto* source_tab = static_cast<ListTab*>(view->property("bench-tab-pointer").value<void*>());
    if (source_tab != nullptr && list_tabs_.size() > 1U) {
        auto* copy_menu = track_context_menu_->addMenu(QStringLiteral("Copy to list"));
        copy_menu->setObjectName(QStringLiteral("bench-track-copy-menu"));
        auto* move_menu = track_context_menu_->addMenu(QStringLiteral("Move to list"));
        move_menu->setObjectName(QStringLiteral("bench-track-move-menu"));
        for (const auto& destination : list_tabs_) {
            if (destination.get() == source_tab) {
                continue;
            }
            const auto target_id = QString::fromStdString(destination->document.id.to_string());
            const auto label = displayText(destination->document.name);
            auto* copy = copy_menu->addAction(label);
            connect(copy, &QAction::triggered, this,
                    [this, view, target_id] { transferSelectedRows(view, target_id, false); });
            auto* move = move_menu->addAction(label);
            connect(move, &QAction::triggered, this,
                    [this, view, target_id] { transferSelectedRows(view, target_id, true); });
        }
    }
    track_context_menu_->addSeparator();
    track_context_menu_->addAction(remove_selected_action_);
    track_context_menu_->popup(view->viewport()->mapToGlobal(position));
}

void BenchMainWindow::showFolderContextMenu(const QPoint& position) {
    if (folder_context_menu_ == nullptr) {
        return;
    }
    const auto target = folder_view_->indexAt(position);
    if (!target.isValid()) {
        return;
    }
    folder_view_->selectionModel()->setCurrentIndex(target, QItemSelectionModel::ClearAndSelect |
                                                                QItemSelectionModel::Rows);
    const auto directory = folder_model_->isDirectory(target);
    folder_add_to_list_action_->setText(directory ? QStringLiteral("Add folder to current list")
                                                  : QStringLiteral("Add file to current list"));
    folder_toggle_expanded_action_->setText(
        folder_view_->isExpanded(target) ? QStringLiteral("Collapse") : QStringLiteral("Expand"));
    folder_toggle_expanded_action_->setEnabled(directory);
    folder_context_menu_->clear();
    folder_context_menu_->addAction(folder_add_to_list_action_);
    if (directory) {
        folder_context_menu_->addAction(folder_toggle_expanded_action_);
    }
    folder_context_menu_->popup(folder_view_->viewport()->mapToGlobal(position));
}

void BenchMainWindow::playCurrentRow() {
    if (isMpdContext()) {
        if (mpd_queue_view_->currentIndex().isValid()) {
            mpd_controller_->playQueueItem(mpd_queue_view_->currentIndex().row());
        }
        return;
    }
    auto* tab = currentListTab();
    if (tab != nullptr && tab->view->currentIndex().isValid()) {
        playRow(*tab, tab->view->currentIndex().row());
    }
}

void BenchMainWindow::removeSelectedRows() {
    if (isMpdContext()) {
        if (mpd_queue_view_->selectionModel() == nullptr) {
            return;
        }
        QVariantList rows;
        for (const auto& index : mpd_queue_view_->selectionModel()->selectedRows()) {
            rows.push_back(index.row());
        }
        if (!rows.isEmpty()) {
            mpd_controller_->removeQueueItems(rows);
        }
        return;
    }
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

void BenchMainWindow::transferSelectedRows(QTableView* source, const QString& target_id,
                                           const bool move) {
    if (source == nullptr || source->selectionModel() == nullptr) {
        return;
    }
    auto selected = source->selectionModel()->selectedRows(0);
    std::ranges::sort(selected, {}, &QModelIndex::row);
    QVariantList rows;
    rows.reserve(selected.size());
    for (const auto& index : selected) {
        rows.push_back(index.row());
    }
    if (!rows.isEmpty()) {
        static_cast<void>(transferRows(source, rows, target_id, move, -1));
    }
}

} // namespace trackknife::bench
