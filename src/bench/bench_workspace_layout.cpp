// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "quick/mpd_probe_controller.hpp"
#include "quick/mpd_queue_model.hpp"
#include "ui/server_library_tree_view.hpp"
#include "uicommon/local_folder_tree_model.hpp"
#include "uicommon/panel_layout.hpp"
#include "uicommon/queue_item_delegate.hpp"
#include "uicommon/queue_table_view.hpp"
#include "uicommon/track_view_layout.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <ranges>
#include <utility>
#include <vector>

namespace trackknife::bench {
namespace {

constexpr auto panel_layout_settings_key = "workspace/panel-layout-v1";
constexpr auto folders_panel_id = "folders";
constexpr auto track_lists_panel_id = "track-lists";
constexpr auto layout_panel_id_property = "trackknife-layout-panel-id";
constexpr auto layout_panel_title_property = "trackknife-layout-panel-title";
constexpr auto layout_container_kind_property = "trackknife-layout-container-kind";

} // namespace

void BenchMainWindow::buildWorkspace() {
    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("bench-tabs"));
    tabs_->setDocumentMode(true);
    tabs_->setMovable(true);
    tabs_->setTabsClosable(true);
    tabs_->installEventFilter(this);
    tabs_->tabBar()->installEventFilter(this);
    connect(tabs_, &QTabWidget::tabCloseRequested, this, &BenchMainWindow::closeTabAt);
    connect(tabs_, &QTabWidget::currentChanged, this, [this](const int) {
        refreshTabActions();
        refreshTrackViewActions();
        refreshSelectionStatus();
        refreshActiveContext();
    });
    connect(tabs_->tabBar(), &QTabBar::tabMoved, this,
            [this](const int, const int) { schedulePersist(); });
    tabs_->setProperty(layout_panel_id_property, QString::fromLatin1(track_lists_panel_id));
    tabs_->setProperty(layout_panel_title_property, QStringLiteral("Track Lists"));
    tabs_->setProperty("trackknifeLayoutPanel", true);

    folder_model_ = new ui::LocalFolderTreeModel(this);
    folders_panel_ = new QWidget(this);
    folders_panel_->setObjectName(QStringLiteral("bench-panel-folders"));
    folders_panel_->setProperty(layout_panel_id_property, QString::fromLatin1(folders_panel_id));
    folders_panel_->setProperty(layout_panel_title_property, QStringLiteral("Sources"));
    folders_panel_->setProperty("trackknifeLayoutPanel", true);
    folders_panel_->setMinimumWidth(160);
    auto* folders_layout = new QVBoxLayout(folders_panel_);
    folders_layout->setContentsMargins(0, 0, 0, 0);
    folders_layout->setSpacing(0);
    source_heading_ = new QLabel(QStringLiteral("Folders"), folders_panel_);
    source_heading_->setObjectName(QStringLiteral("bench-folders-heading"));
    source_heading_->setAlignment(Qt::AlignCenter);
    source_heading_->setContentsMargins(8, 4, 8, 4);
    folders_layout->addWidget(source_heading_);
    source_stack_ = new QStackedWidget(folders_panel_);
    source_stack_->setObjectName(QStringLiteral("bench-source-stack"));
    folder_view_ = new QTreeView(source_stack_);
    folder_view_->setObjectName(QStringLiteral("bench-folder-tree"));
    folder_view_->setModel(folder_model_);
    folder_view_->setHeaderHidden(true);
    folder_view_->setUniformRowHeights(true);
    folder_view_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(folder_view_, &QWidget::customContextMenuRequested, this,
            &BenchMainWindow::showFolderContextMenu);
    connect(folder_view_, &QTreeView::activated, this, [this](const QModelIndex& index) {
        if (!index.isValid() || folder_model_->isDirectory(index)) {
            return;
        }
        openLocalPaths({folder_model_->rawPath(index)});
    });
    source_stack_->addWidget(folder_view_);
    folders_layout->addWidget(source_stack_, 1);

    buildMpdWorkspace();

    panel_widgets_.insert(QString::fromLatin1(folders_panel_id), folders_panel_);
    panel_widgets_.insert(QString::fromLatin1(track_lists_panel_id), tabs_);
    layout_host_ = new QWidget(this);
    layout_host_->setObjectName(QStringLiteral("bench-panel-layout-host"));
    layout_host_layout_ = new QVBoxLayout(layout_host_);
    layout_host_layout_->setContentsMargins(0, 0, 0, 0);
    layout_host_layout_->setSpacing(0);
    setCentralWidget(layout_host_);
    loadPanelLayout();

    selection_status_ = new QLabel(statusBar());
    selection_status_->setObjectName(QStringLiteral("bench-selection-status"));
    selection_status_->setAccessibleName(QStringLiteral("Selected track information"));
    selection_status_->setContentsMargins(6, 0, 6, 0);
    selection_status_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    statusBar()->addWidget(selection_status_, 1);
    buildMpdStatusControls();
    refreshSelectionStatus();

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
    connect_mpd_action_ = file_menu->addAction(QStringLiteral("Connect to MPD…"));
    connect_mpd_action_->setObjectName(QStringLiteral("action-connect-mpd"));
    connect_mpd_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+K")));
    connect(connect_mpd_action_, &QAction::triggered, this,
            &BenchMainWindow::openMpdConnectionDialog);
    disconnect_mpd_action_ = file_menu->addAction(QStringLiteral("Disconnect MPD"));
    disconnect_mpd_action_->setObjectName(QStringLiteral("action-disconnect-mpd"));
    connect(disconnect_mpd_action_, &QAction::triggered, mpd_controller_,
            &quick::MpdProbeController::disconnectFromServer);
    file_menu->addSeparator();
    auto* quit = file_menu->addAction(QStringLiteral("Quit"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);

    auto* edit_menu = menuBar()->addMenu(QStringLiteral("&Edit"));
    play_selected_action_ = new QAction(QStringLiteral("Play"), this);
    play_selected_action_->setObjectName(QStringLiteral("action-play-selected-track"));
    connect(play_selected_action_, &QAction::triggered, this, &BenchMainWindow::playCurrentRow);
    properties_action_ = edit_menu->addAction(QStringLiteral("Properties…"));
    properties_action_->setObjectName(QStringLiteral("action-track-properties"));
    properties_action_->setShortcut(QKeySequence(QStringLiteral("Alt+Return")));
    properties_action_->setEnabled(false);
    connect(properties_action_, &QAction::triggered, this,
            &BenchMainWindow::showMetadataProperties);
    edit_menu->addSeparator();
    remove_selected_action_ = edit_menu->addAction(QStringLiteral("Remove selected"));
    remove_selected_action_->setObjectName(QStringLiteral("action-remove-selected-tracks"));
    remove_selected_action_->setShortcut(QKeySequence::Delete);
    connect(remove_selected_action_, &QAction::triggered, this,
            &BenchMainWindow::removeSelectedRows);

    folder_add_to_list_action_ = new QAction(QStringLiteral("Add to current list"), this);
    folder_add_to_list_action_->setObjectName(QStringLiteral("action-folder-add-to-list"));
    connect(folder_add_to_list_action_, &QAction::triggered, this, [this] {
        const auto index = folder_view_->currentIndex();
        if (index.isValid()) {
            openLocalPaths({folder_model_->rawPath(index)});
        }
    });
    folder_toggle_expanded_action_ = new QAction(QStringLiteral("Expand"), this);
    folder_toggle_expanded_action_->setObjectName(QStringLiteral("action-folder-toggle-expanded"));
    connect(folder_toggle_expanded_action_, &QAction::triggered, this, [this] {
        const auto index = folder_view_->currentIndex();
        if (!index.isValid() || !folder_model_->isDirectory(index)) {
            return;
        }
        folder_view_->setExpanded(index, !folder_view_->isExpanded(index));
    });

    auto* workspace_menu = menuBar()->addMenu(QStringLiteral("&Workspace"));
    duplicate_tab_action_ = workspace_menu->addAction(QStringLiteral("Duplicate tab"));
    duplicate_tab_action_->setObjectName(QStringLiteral("action-duplicate-tab"));
    duplicate_tab_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+D")));
    connect(duplicate_tab_action_, &QAction::triggered, this,
            &BenchMainWindow::duplicateCurrentTab);
    pin_tab_action_ = workspace_menu->addAction(QStringLiteral("Pin tab"));
    pin_tab_action_->setObjectName(QStringLiteral("action-pin-tab"));
    pin_tab_action_->setCheckable(true);
    pin_tab_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+P")));
    connect(pin_tab_action_, &QAction::triggered, this, &BenchMainWindow::toggleCurrentTabPinned);
    save_tab_action_ = workspace_menu->addAction(QStringLiteral("Save list"));
    save_tab_action_->setObjectName(QStringLiteral("action-save-list"));
    save_tab_action_->setShortcut(QKeySequence::Save);
    connect(save_tab_action_, &QAction::triggered, this, &BenchMainWindow::saveCurrentList);
    rename_tab_action_ = workspace_menu->addAction(QStringLiteral("Rename tab…"));
    rename_tab_action_->setObjectName(QStringLiteral("action-rename-tab"));
    rename_tab_action_->setShortcut(QKeySequence(Qt::Key_F2));
    connect(rename_tab_action_, &QAction::triggered, this, &BenchMainWindow::renameCurrentList);
    workspace_menu->addSeparator();
    close_tab_action_ = workspace_menu->addAction(QStringLiteral("Close tab"));
    close_tab_action_->setObjectName(QStringLiteral("action-close-tab"));
    close_tab_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+W")));
    connect(close_tab_action_, &QAction::triggered, this, &BenchMainWindow::closeCurrentTab);

    workspace_menu->addSeparator();
    auto* track_layout_menu = workspace_menu->addMenu(QStringLiteral("Track list layout"));
    track_layout_menu->setObjectName(QStringLiteral("menu-track-list-layout"));
    track_presentation_group_ = new QActionGroup(track_layout_menu);
    track_presentation_group_->setExclusive(true);
    track_albums_side_action_ =
        track_layout_menu->addAction(QStringLiteral("Albums with side artwork"));
    track_albums_side_action_->setObjectName(QStringLiteral("action-track-layout-albums-side"));
    track_albums_side_action_->setCheckable(true);
    track_presentation_group_->addAction(track_albums_side_action_);
    connect(track_albums_side_action_, &QAction::triggered, this,
            [this] { setTrackViewPresentation(ui::TrackViewPresentation::albums_side_artwork); });
    track_albums_header_action_ =
        track_layout_menu->addAction(QStringLiteral("Albums with header artwork"));
    track_albums_header_action_->setObjectName(QStringLiteral("action-track-layout-albums-header"));
    track_albums_header_action_->setCheckable(true);
    track_presentation_group_->addAction(track_albums_header_action_);
    connect(track_albums_header_action_, &QAction::triggered, this,
            [this] { setTrackViewPresentation(ui::TrackViewPresentation::albums_header_artwork); });
    track_plain_columns_action_ = track_layout_menu->addAction(QStringLiteral("Plain columns"));
    track_plain_columns_action_->setObjectName(QStringLiteral("action-track-layout-plain"));
    track_plain_columns_action_->setCheckable(true);
    track_presentation_group_->addAction(track_plain_columns_action_);
    connect(track_plain_columns_action_, &QAction::triggered, this,
            [this] { setTrackViewPresentation(ui::TrackViewPresentation::plain_columns); });
    track_compact_queue_action_ = track_layout_menu->addAction(QStringLiteral("Compact queue"));
    track_compact_queue_action_->setObjectName(QStringLiteral("action-track-layout-compact"));
    track_compact_queue_action_->setCheckable(true);
    track_presentation_group_->addAction(track_compact_queue_action_);
    connect(track_compact_queue_action_, &QAction::triggered, this,
            [this] { setTrackViewPresentation(ui::TrackViewPresentation::compact_queue); });
    track_layout_menu->addSeparator();
    track_columns_menu_ = track_layout_menu->addMenu(QStringLiteral("Columns"));
    track_columns_menu_->setObjectName(QStringLiteral("menu-track-columns"));
    for (const auto& spec : track_column_specs) {
        const auto id = QString::fromLatin1(spec.id);
        auto* action = track_columns_menu_->addAction(QString::fromLatin1(spec.label));
        action->setObjectName(QStringLiteral("action-track-column-%1").arg(id));
        action->setCheckable(true);
        connect(action, &QAction::toggled, this,
                [this, id](const bool visible) { setTrackColumnVisible(id, visible); });
        track_column_actions_.insert(id, action);
    }
    track_layout_reset_action_ =
        track_layout_menu->addAction(QStringLiteral("Reset current list layout"));
    track_layout_reset_action_->setObjectName(QStringLiteral("action-reset-track-layout"));
    connect(track_layout_reset_action_, &QAction::triggered, this,
            &BenchMainWindow::resetTrackViewLayout);
    track_layout_copy_action_ = track_layout_menu->addAction(
        QStringLiteral("Apply current layout to all queues and lists"));
    track_layout_copy_action_->setObjectName(QStringLiteral("action-copy-track-layout"));
    connect(track_layout_copy_action_, &QAction::triggered, this,
            &BenchMainWindow::copyTrackViewLayoutToAllTabs);

    workspace_menu->addSeparator();
    layout_edit_action_ = workspace_menu->addAction(QStringLiteral("Edit panel layout"));
    layout_edit_action_->setObjectName(QStringLiteral("action-edit-panel-layout"));
    layout_edit_action_->setCheckable(true);
    layout_edit_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+L")));
    connect(layout_edit_action_, &QAction::toggled, this, &BenchMainWindow::setLayoutEditMode);
    auto* arrangement_menu = workspace_menu->addMenu(QStringLiteral("Panel arrangement"));
    arrangement_menu->setObjectName(QStringLiteral("menu-panel-arrangement"));
    layout_arrangement_group_ = new QActionGroup(arrangement_menu);
    layout_arrangement_group_->setExclusive(true);
    layout_side_by_side_action_ = arrangement_menu->addAction(QStringLiteral("Side by side"));
    layout_side_by_side_action_->setObjectName(QStringLiteral("action-layout-side-by-side"));
    layout_side_by_side_action_->setCheckable(true);
    layout_arrangement_group_->addAction(layout_side_by_side_action_);
    connect(layout_side_by_side_action_, &QAction::triggered, this,
            [this] { arrangePanelLayout(ui::PanelLayoutNodeKind::split, Qt::Horizontal); });
    layout_top_bottom_action_ = arrangement_menu->addAction(QStringLiteral("Top and bottom"));
    layout_top_bottom_action_->setObjectName(QStringLiteral("action-layout-top-bottom"));
    layout_top_bottom_action_->setCheckable(true);
    layout_arrangement_group_->addAction(layout_top_bottom_action_);
    connect(layout_top_bottom_action_, &QAction::triggered, this,
            [this] { arrangePanelLayout(ui::PanelLayoutNodeKind::split, Qt::Vertical); });
    layout_tabbed_action_ = arrangement_menu->addAction(QStringLiteral("Tabbed stack"));
    layout_tabbed_action_->setObjectName(QStringLiteral("action-layout-tabbed"));
    layout_tabbed_action_->setCheckable(true);
    layout_arrangement_group_->addAction(layout_tabbed_action_);
    connect(layout_tabbed_action_, &QAction::triggered, this,
            [this] { arrangePanelLayout(ui::PanelLayoutNodeKind::tabs, Qt::Horizontal); });
    layout_swap_action_ = workspace_menu->addAction(QStringLiteral("Swap panels"));
    layout_swap_action_->setObjectName(QStringLiteral("action-layout-swap-panels"));
    connect(layout_swap_action_, &QAction::triggered, this, &BenchMainWindow::swapPanelLayout);
    layout_reset_action_ = workspace_menu->addAction(QStringLiteral("Reset panel layout"));
    layout_reset_action_->setObjectName(QStringLiteral("action-reset-panel-layout"));
    connect(layout_reset_action_, &QAction::triggered, this, &BenchMainWindow::resetPanelLayout);

    tab_context_menu_ = new QMenu(tabs_);
    tab_context_menu_->setObjectName(QStringLiteral("bench-tab-context-menu"));
    tab_context_menu_->addAction(rename_tab_action_);
    tab_context_menu_->addAction(save_tab_action_);
    tab_context_menu_->addAction(pin_tab_action_);
    tab_context_menu_->addAction(duplicate_tab_action_);
    tab_context_menu_->addSeparator();
    tab_context_menu_->addAction(close_tab_action_);
    tabs_->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tabs_->tabBar(), &QWidget::customContextMenuRequested, this,
            &BenchMainWindow::showTabContextMenu);
    track_context_menu_ = new QMenu(tabs_);
    track_context_menu_->setObjectName(QStringLiteral("bench-track-context-menu"));
    folder_context_menu_ = new QMenu(folders_panel_);
    folder_context_menu_->setObjectName(QStringLiteral("bench-folder-context-menu"));
    mpd_library_context_menu_ = new QMenu(server_library_view_);
    mpd_library_context_menu_->setObjectName(QStringLiteral("bench-mpd-library-context-menu"));
    refreshTabActions();
    refreshTrackViewActions();
    refreshPanelLayoutActions();
}

ui::PanelLayout BenchMainWindow::defaultPanelLayout() const {
    std::vector<ui::PanelLayoutNode> children;
    children.push_back(ui::panelLayoutPanel(QString::fromLatin1(folders_panel_id)));
    children.push_back(ui::panelLayoutPanel(QString::fromLatin1(track_lists_panel_id)));
    return ui::PanelLayout{
        .schema_version = ui::panel_layout_schema_version,
        .root = ui::panelLayoutSplit(Qt::Horizontal, std::move(children), {1, 3}),
    };
}

void BenchMainWindow::loadPanelLayout() {
    QSettings settings;
    const auto encoded =
        settings.value(QString::fromLatin1(panel_layout_settings_key)).toByteArray();
    if (encoded.isEmpty()) {
        applyPanelLayout(defaultPanelLayout());
        return;
    }

    QString error;
    const QStringList registered_panel_ids = panel_widgets_.keys();
    auto restored = ui::deserializePanelLayout(encoded, registered_panel_ids, &error);
    if (!restored) {
        panel_layout_persistence_protected_ = true;
        applyPanelLayout(defaultPanelLayout());
        statusBar()->showMessage(
            QStringLiteral("Panel layout was not loaded (%1); the saved value was preserved")
                .arg(error),
            7'000);
        return;
    }
    applyPanelLayout(*restored);
}

void BenchMainWindow::applyPanelLayout(const ui::PanelLayout& layout) {
    applying_panel_layout_ = true;
    auto* previous_root = layout_root_;
    for (auto* panel : panel_widgets_) {
        panel->hide();
        panel->setParent(layout_host_);
    }
    if (previous_root != nullptr) {
        layout_host_layout_->removeWidget(previous_root);
    }
    layout_root_ = renderPanelLayoutNode(layout.root, layout_host_);
    layout_host_layout_->addWidget(layout_root_);
    layout_root_->show();
    for (auto* panel : panel_widgets_) {
        panel->show();
    }
    if (previous_root != nullptr && !panel_widgets_.values().contains(previous_root)) {
        previous_root->deleteLater();
    }
    applying_panel_layout_ = false;
    refreshPanelLayoutActions();
}

QWidget* BenchMainWindow::renderPanelLayoutNode(const ui::PanelLayoutNode& node, QWidget* parent) {
    if (node.kind == ui::PanelLayoutNodeKind::panel) {
        auto* panel = panel_widgets_.value(node.panel_id, nullptr);
        Q_ASSERT(panel != nullptr);
        panel->setParent(parent);
        return panel;
    }
    if (node.kind == ui::PanelLayoutNodeKind::split) {
        auto* splitter = new QSplitter(node.orientation, parent);
        splitter->setObjectName(QStringLiteral("bench-panel-layout-split"));
        splitter->setProperty(layout_container_kind_property, QStringLiteral("split"));
        splitter->setChildrenCollapsible(false);
        for (const auto& child : node.children) {
            splitter->addWidget(renderPanelLayoutNode(child, splitter));
        }
        QList<int> sizes;
        sizes.reserve(static_cast<qsizetype>(node.weights.size()));
        for (int index = 0; const auto weight : node.weights) {
            sizes.push_back(weight);
            splitter->setStretchFactor(index++, weight);
        }
        splitter->setSizes(sizes);
        QTimer::singleShot(0, splitter, [splitter, weights = node.weights] {
            const auto extent =
                splitter->orientation() == Qt::Horizontal ? splitter->width() : splitter->height();
            int total = 0;
            for (const auto weight : weights) {
                total += weight;
            }
            total = std::max(1, total);
            QList<int> scaled;
            scaled.reserve(static_cast<qsizetype>(weights.size()));
            for (const auto weight : weights) {
                scaled.push_back(std::max(1, extent * weight / total));
            }
            splitter->setSizes(scaled);
        });
        connect(splitter, &QSplitter::splitterMoved, this, [this](const int, const int) {
            if (applying_panel_layout_) {
                return;
            }
            panel_layout_persistence_protected_ = false;
            persistPanelLayout();
        });
        return splitter;
    }

    auto* stack = new QTabWidget(parent);
    stack->setObjectName(QStringLiteral("bench-panel-layout-tabs"));
    stack->setProperty(layout_container_kind_property, QStringLiteral("tabs"));
    stack->setDocumentMode(true);
    stack->setMovable(true);
    for (const auto& child : node.children) {
        auto* child_widget = renderPanelLayoutNode(child, stack);
        auto title = child_widget->property(layout_panel_title_property).toString();
        if (title.isEmpty()) {
            title = QStringLiteral("Panel group");
        }
        stack->addTab(child_widget, title);
    }
    stack->setCurrentIndex(node.active_child);
    connect(stack, &QTabWidget::currentChanged, this, [this](const int) {
        if (!applying_panel_layout_) {
            persistPanelLayout();
        }
    });
    connect(stack->tabBar(), &QTabBar::tabMoved, this, [this](const int, const int) {
        if (!applying_panel_layout_) {
            panel_layout_persistence_protected_ = false;
            persistPanelLayout();
        }
    });
    return stack;
}

ui::PanelLayoutNode BenchMainWindow::capturePanelLayoutNode(QWidget* widget) const {
    const auto panel_id = widget->property(layout_panel_id_property).toString();
    if (!panel_id.isEmpty()) {
        return ui::panelLayoutPanel(panel_id);
    }
    const auto container_kind = widget->property(layout_container_kind_property).toString();
    if (container_kind == QStringLiteral("split")) {
        auto* splitter = qobject_cast<QSplitter*>(widget);
        Q_ASSERT(splitter != nullptr);
        std::vector<ui::PanelLayoutNode> children;
        children.reserve(static_cast<std::size_t>(splitter->count()));
        for (int index = 0; index < splitter->count(); ++index) {
            children.push_back(capturePanelLayoutNode(splitter->widget(index)));
        }
        std::vector<int> weights;
        const auto sizes = splitter->sizes();
        weights.reserve(static_cast<std::size_t>(sizes.size()));
        for (const auto size : sizes) {
            weights.push_back(std::max(size, 1));
        }
        return ui::panelLayoutSplit(splitter->orientation(), std::move(children),
                                    std::move(weights));
    }
    if (container_kind == QStringLiteral("tabs")) {
        auto* stack = qobject_cast<QTabWidget*>(widget);
        Q_ASSERT(stack != nullptr);
        std::vector<ui::PanelLayoutNode> children;
        children.reserve(static_cast<std::size_t>(stack->count()));
        for (int index = 0; index < stack->count(); ++index) {
            children.push_back(capturePanelLayoutNode(stack->widget(index)));
        }
        return ui::panelLayoutTabs(std::move(children), stack->currentIndex());
    }
    Q_ASSERT_X(false, "BenchMainWindow::capturePanelLayoutNode",
               "panel-layout renderer produced an unknown widget");
    return ui::panelLayoutPanel(QStringLiteral("invalid"));
}

void BenchMainWindow::persistPanelLayout() {
    if (layout_root_ == nullptr || applying_panel_layout_ || panel_layout_persistence_protected_) {
        return;
    }
    const ui::PanelLayout layout{.schema_version = ui::panel_layout_schema_version,
                                 .root = capturePanelLayoutNode(layout_root_)};
    QSettings settings;
    settings.setValue(QString::fromLatin1(panel_layout_settings_key),
                      ui::serializePanelLayout(layout));
}

void BenchMainWindow::setLayoutEditMode(const bool editing) {
    layout_host_->setProperty("trackknifeLayoutEditing", editing);
    layout_host_->setStyleSheet(editing
                                    ? QStringLiteral("QWidget[trackknifeLayoutPanel=\"true\"] {"
                                                     " border: 1px dashed palette(highlight); }")
                                    : QString{});
    if (editing) {
        statusBar()->showMessage(
            QStringLiteral("Panel layout editing: choose an arrangement or swap panels"));
    } else {
        statusBar()->clearMessage();
    }
    refreshPanelLayoutActions();
}

void BenchMainWindow::arrangePanelLayout(const ui::PanelLayoutNodeKind kind,
                                         const Qt::Orientation orientation) {
    if (layout_root_ == nullptr || kind == ui::PanelLayoutNodeKind::panel) {
        return;
    }
    auto root = capturePanelLayoutNode(layout_root_);
    if (root.children.empty()) {
        return;
    }
    auto children = std::move(root.children);
    ui::PanelLayoutNode replacement;
    if (kind == ui::PanelLayoutNodeKind::split) {
        auto weights = root.kind == ui::PanelLayoutNodeKind::split
                           ? std::move(root.weights)
                           : std::vector<int>(children.size(), 1);
        replacement = ui::panelLayoutSplit(orientation, std::move(children), std::move(weights));
    } else {
        auto active = root.kind == ui::PanelLayoutNodeKind::tabs ? root.active_child : 0;
        if (root.kind != ui::PanelLayoutNodeKind::tabs) {
            const auto track_lists =
                std::ranges::find(children, QString::fromLatin1(track_lists_panel_id),
                                  &ui::PanelLayoutNode::panel_id);
            if (track_lists != children.end()) {
                active = static_cast<int>(std::distance(children.begin(), track_lists));
            }
        }
        replacement = ui::panelLayoutTabs(std::move(children), active);
    }
    panel_layout_persistence_protected_ = false;
    applyPanelLayout(ui::PanelLayout{.schema_version = ui::panel_layout_schema_version,
                                     .root = std::move(replacement)});
    persistPanelLayout();
}

void BenchMainWindow::swapPanelLayout() {
    if (layout_root_ == nullptr) {
        return;
    }
    auto root = capturePanelLayoutNode(layout_root_);
    if (root.children.size() < 2U) {
        return;
    }
    std::ranges::reverse(root.children);
    if (root.kind == ui::PanelLayoutNodeKind::split) {
        std::ranges::reverse(root.weights);
    } else if (root.kind == ui::PanelLayoutNodeKind::tabs) {
        root.active_child = static_cast<int>(root.children.size()) - 1 - root.active_child;
    }
    panel_layout_persistence_protected_ = false;
    applyPanelLayout(ui::PanelLayout{.schema_version = ui::panel_layout_schema_version,
                                     .root = std::move(root)});
    persistPanelLayout();
}

void BenchMainWindow::resetPanelLayout() {
    panel_layout_persistence_protected_ = false;
    applyPanelLayout(defaultPanelLayout());
    persistPanelLayout();
}

void BenchMainWindow::refreshPanelLayoutActions() {
    if (layout_edit_action_ == nullptr) {
        return;
    }
    const bool editing = layout_edit_action_->isChecked();
    for (auto* action : {layout_side_by_side_action_, layout_top_bottom_action_,
                         layout_tabbed_action_, layout_swap_action_}) {
        action->setEnabled(editing && layout_root_ != nullptr);
    }
    layout_reset_action_->setEnabled(layout_root_ != nullptr);
    if (layout_root_ == nullptr) {
        return;
    }
    const auto root = capturePanelLayoutNode(layout_root_);
    const QSignalBlocker horizontal_blocker{layout_side_by_side_action_};
    const QSignalBlocker vertical_blocker{layout_top_bottom_action_};
    const QSignalBlocker tabbed_blocker{layout_tabbed_action_};
    layout_side_by_side_action_->setChecked(root.kind == ui::PanelLayoutNodeKind::split &&
                                            root.orientation == Qt::Horizontal);
    layout_top_bottom_action_->setChecked(root.kind == ui::PanelLayoutNodeKind::split &&
                                          root.orientation == Qt::Vertical);
    layout_tabbed_action_->setChecked(root.kind == ui::PanelLayoutNodeKind::tabs);
}

} // namespace trackknife::bench
