// SPDX-License-Identifier: GPL-3.0-only

#include "ui/main_window.hpp"

#include "quick/mpd_browser_model.hpp"
#include "quick/mpd_output_model.hpp"
#include "quick/mpd_probe_controller.hpp"
#include "quick/mpd_queue_model.hpp"
#include "quick/mpd_search_result_model.hpp"
#include "trackknife/persistence/list_repository.hpp"
#include "trackknife/qtmodels/paged_track_model.hpp"
#include "ui/format_sandbox.hpp"
#include "ui/library_tree_editor_dialog.hpp"
#include "ui/mpd_connection_dialog.hpp"
#include "ui/server_library_tree_model.hpp"
#include "ui/server_library_tree_view.hpp"
#include "uicommon/command_palette.hpp"
#include "uicommon/line_slider.hpp"
#include "uicommon/list_persistence_service.hpp"
#include "uicommon/queue_item_delegate.hpp"
#include "uicommon/queue_table_view.hpp"
#include "uicommon/track_row_roles.hpp"

#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCache>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHelpEvent>
#include <QIcon>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSlider>
#include <QSortFilterProxyModel>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringList>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QToolTip>
#include <QTreeView>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <random>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace trackknife::ui {
namespace {

constexpr int layout_version = 3;
constexpr int chrome_horizontal_padding = 8;
constexpr auto hover_row_property = "trackknife-hover-row";

[[nodiscard]] QWidget* makeChromeEdgeSpacer(QWidget* parent, const QString& object_name) {
    auto* spacer = new QWidget(parent);
    spacer->setObjectName(object_name);
    spacer->setFixedWidth(chrome_horizontal_padding);
    spacer->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    return spacer;
}

[[nodiscard]] std::string utf8Bytes(const QString& value) {
    const auto encoded = value.toUtf8();
    return {encoded.constData(), static_cast<std::size_t>(encoded.size())};
}

[[nodiscard]] QString displayText(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

class RowHoverDelegate final : public QStyledItemDelegate {
  public:
    explicit RowHoverDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        auto row_option = option;
        row_option.state &= ~QStyle::State_MouseOver;
        const auto* view = qobject_cast<const QTableView*>(parent());
        if (view != nullptr && view->property(hover_row_property).toInt() == index.row()) {
            row_option.state |= QStyle::State_MouseOver;
        }
        QStyledItemDelegate::paint(painter, row_option, index);
    }
};

class RowHoverFilter final : public QObject {
  public:
    explicit RowHoverFilter(QTableView* view)
        : QObject(view), view_object_(view), viewport_(view->viewport()) {}

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched != viewport_) {
            return QObject::eventFilter(watched, event);
        }
        auto* view = qobject_cast<QTableView*>(view_object_);
        if (view == nullptr) {
            return QObject::eventFilter(watched, event);
        }
        if (event->type() == QEvent::MouseMove) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            setHoveredRow(view, view->indexAt(mouse->position().toPoint()).row());
        } else if (event->type() == QEvent::Leave) {
            setHoveredRow(view, -1);
        }
        return QObject::eventFilter(watched, event);
    }

  private:
    static void setHoveredRow(QTableView* view, const int row) {
        const auto previous = view->property(hover_row_property).toInt();
        if (previous == row) {
            return;
        }
        view->setProperty(hover_row_property, row);
        updateRow(view, previous);
        updateRow(view, row);
    }

    static void updateRow(QTableView* view, const int row) {
        if (row < 0 || row >= view->model()->rowCount()) {
            return;
        }
        view->viewport()->update(0, view->rowViewportPosition(row), view->viewport()->width(),
                                 view->rowHeight(row));
    }

    QObject* view_object_;
    QWidget* viewport_;
};

enum class SearchQueueAction { append, insert, replace };

class LiveSearchLineEdit final : public QLineEdit {
  public:
    explicit LiveSearchLineEdit(QWidget* parent) : QLineEdit(parent) {}

    void setResultFocusCallback(std::function<void()> callback) {
        result_focus_callback_ = std::move(callback);
    }
    void setCloseCallback(std::function<void()> callback) { close_callback_ = std::move(callback); }
    void setActionCallback(std::function<void(SearchQueueAction)> callback) {
        action_callback_ = std::move(callback);
    }
    void setCommitCallback(std::function<void()> callback) {
        commit_callback_ = std::move(callback);
    }

  protected:
    void keyPressEvent(QKeyEvent* event) override {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            event->modifiers() == Qt::ShiftModifier && commit_callback_) {
            commit_callback_();
            event->accept();
            return;
        }
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            event->modifiers() == Qt::ControlModifier && action_callback_) {
            action_callback_(SearchQueueAction::replace);
            event->accept();
            return;
        }
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            event->modifiers() == Qt::NoModifier && action_callback_) {
            action_callback_(SearchQueueAction::append);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Down && result_focus_callback_) {
            result_focus_callback_();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Escape && close_callback_) {
            close_callback_();
            event->accept();
            return;
        }
        QLineEdit::keyPressEvent(event);
    }

  private:
    std::function<void()> result_focus_callback_;
    std::function<void()> close_callback_;
    std::function<void(SearchQueueAction)> action_callback_;
    std::function<void()> commit_callback_;
};

class LiveSearchTableView final : public QTableView {
  public:
    explicit LiveSearchTableView(QWidget* parent) : QTableView(parent) {}

    void setSearchField(QLineEdit* field) { search_field_ = field; }
    void setCloseCallback(std::function<void()> callback) { close_callback_ = std::move(callback); }
    void setActionCallback(std::function<void(int, SearchQueueAction)> callback) {
        action_callback_ = std::move(callback);
    }
    void setCommitCallback(std::function<void()> callback) {
        commit_callback_ = std::move(callback);
    }

    void focusFirstResult() {
        const auto* results = qobject_cast<const quick::MpdSearchResultModel*>(model());
        const auto row = results != nullptr ? results->firstResultRow() : -1;
        if (row < 0) {
            return;
        }
        const auto first = model()->index(row, 1);
        selectionModel()->setCurrentIndex(first, QItemSelectionModel::ClearAndSelect |
                                                     QItemSelectionModel::Rows);
        setFocus();
        scrollTo(currentIndex());
    }

    void focusCurrentResult() {
        const auto* results = qobject_cast<const quick::MpdSearchResultModel*>(model());
        auto current = currentIndex();
        if (results == nullptr || !current.isValid() ||
            results->kindAt(current.row()) == quick::MpdSearchResultModel::ResultKind::section) {
            const auto row = results != nullptr ? results->firstResultRow() : -1;
            if (row < 0) {
                return;
            }
            current = model()->index(row, 1);
        }
        selectionModel()->setCurrentIndex(current, QItemSelectionModel::ClearAndSelect |
                                                       QItemSelectionModel::Rows);
        setFocus();
        scrollTo(current);
    }

    void activateDefault(const SearchQueueAction action) {
        const auto* results = qobject_cast<const quick::MpdSearchResultModel*>(model());
        auto row = currentIndex().row();
        if (results == nullptr || !currentIndex().isValid() ||
            results->kindAt(row) == quick::MpdSearchResultModel::ResultKind::section) {
            row = results != nullptr ? results->firstResultRow() : -1;
        }
        if (row >= 0 && action_callback_) {
            action_callback_(row, action);
        }
    }

  protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Escape) {
            if (close_callback_) {
                close_callback_();
            }
            event->accept();
            return;
        }
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            event->modifiers() == Qt::ShiftModifier) {
            if (commit_callback_) {
                commit_callback_();
            }
            event->accept();
            return;
        }
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            event->modifiers() == Qt::ControlModifier) {
            activateCurrent(SearchQueueAction::replace);
            event->accept();
            return;
        }
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            event->modifiers() == Qt::NoModifier) {
            if (const auto action = actionForColumn(currentIndex().column())) {
                activateCurrent(*action);
            } else {
                activateCurrent(SearchQueueAction::append);
            }
            event->accept();
            return;
        }
        if (event->modifiers() == Qt::NoModifier &&
            (event->key() == Qt::Key_Backspace || isSearchText(event->text()))) {
            forwardToSearch(event);
            return;
        }
        if (event->key() == Qt::Key_Down || event->key() == Qt::Key_Up) {
            moveVertically(event->key() == Qt::Key_Down ? 1 : -1);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Right &&
            currentIndex().column() < quick::MpdSearchResultModel::first_action_column) {
            setCurrentIndex(currentIndex().siblingAtColumn(
                quick::MpdSearchResultModel::first_action_column + 1));
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Left &&
            currentIndex().column() <= quick::MpdSearchResultModel::first_action_column + 1) {
            setCurrentIndex(currentIndex().siblingAtColumn(1));
            event->accept();
            return;
        }
        QTableView::keyPressEvent(event);
    }

  private:
    [[nodiscard]] static bool isSearchText(const QString& text) {
        return !text.isEmpty() &&
               std::ranges::all_of(text, [](const QChar character) { return character.isPrint(); });
    }

    [[nodiscard]] static std::optional<SearchQueueAction> actionForColumn(const int column) {
        switch (column) {
        case 4:
            return SearchQueueAction::append;
        case 5:
            return SearchQueueAction::insert;
        case 6:
            return SearchQueueAction::replace;
        default:
            return std::nullopt;
        }
    }

    void activateCurrent(const SearchQueueAction action) {
        if (currentIndex().isValid() && action_callback_) {
            action_callback_(currentIndex().row(), action);
        }
    }

    void forwardToSearch(QKeyEvent* event) {
        if (search_field_ == nullptr) {
            return;
        }
        const auto cursor = static_cast<int>(
            std::min<qsizetype>(search_field_->text().size(), std::numeric_limits<int>::max()));
        const auto repeat_count = static_cast<quint16>(
            std::clamp(event->count(), 0, static_cast<int>(std::numeric_limits<quint16>::max())));
        if (!search_field_->hasSelectedText()) {
            search_field_->setCursorPosition(cursor);
        }
        search_field_->setFocus();
        QKeyEvent forwarded{event->type(), event->key(),          event->modifiers(),
                            event->text(), event->isAutoRepeat(), repeat_count};
        QApplication::sendEvent(search_field_, &forwarded);
        event->accept();
    }

    void moveVertically(const int direction) {
        const auto* results = qobject_cast<const quick::MpdSearchResultModel*>(model());
        if (results == nullptr) {
            return;
        }
        const auto row = results->nextResultRow(currentIndex().row(), direction);
        if (row >= 0) {
            const auto column = currentIndex().column() >= 0 ? currentIndex().column() : 1;
            setCurrentIndex(model()->index(row, column));
            scrollTo(currentIndex());
        } else if (direction < 0 && search_field_ != nullptr) {
            search_field_->setFocus();
        }
    }

    QLineEdit* search_field_{nullptr};
    std::function<void()> close_callback_;
    std::function<void(int, SearchQueueAction)> action_callback_;
    std::function<void()> commit_callback_;
};

class SearchActionDelegate final : public QStyledItemDelegate {
  public:
    SearchActionDelegate(QIcon icon, QObject* parent)
        : QStyledItemDelegate(parent), icon_(std::move(icon)) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        auto item = option;
        initStyleOption(&item, index);
        item.state &= ~QStyle::State_MouseOver;
        const auto* view = qobject_cast<const QTableView*>(parent());
        if (view != nullptr && view->property(hover_row_property).toInt() == index.row()) {
            item.state |= QStyle::State_MouseOver;
        }
        item.text.clear();
        item.icon = {};
        item.features &= ~QStyleOptionViewItem::HasDecoration;
        item.state &= ~QStyle::State_HasFocus;
        const auto* widget = item.widget;
        auto* item_style = widget != nullptr ? widget->style() : QApplication::style();
        item_style->drawControl(QStyle::CE_ItemViewItem, &item, painter, widget);

        constexpr QSize icon_size{12, 12};
        constexpr QSize focus_size{18, 18};
        const auto icon_rect =
            QStyle::alignedRect(item.direction, Qt::AlignCenter, icon_size, item.rect);
        auto active = false;
        if (view != nullptr && view->hasFocus()) {
            const auto current = view->currentIndex();
            const auto active_column =
                current.column() >= quick::MpdSearchResultModel::first_action_column
                    ? current.column()
                    : quick::MpdSearchResultModel::first_action_column;
            active = current.row() == index.row() && active_column == index.column();
        }
        if (active) {
            const auto focus_rect =
                QStyle::alignedRect(item.direction, Qt::AlignCenter, focus_size, item.rect)
                    .adjusted(0, 0, -1, -1);
            auto marker = item.palette.color(item.state.testFlag(QStyle::State_Selected)
                                                 ? QPalette::HighlightedText
                                                 : QPalette::Highlight);
            auto fill = marker;
            fill.setAlpha(36);
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setBrush(fill);
            painter->setPen(QPen(marker, 1.0));
            painter->drawRoundedRect(focus_rect, 3.0, 3.0);
            painter->restore();
        }

        const auto mode =
            item.state.testFlag(QStyle::State_Enabled) ? QIcon::Normal : QIcon::Disabled;
        painter->save();
        painter->setOpacity(active ? 1.0 : 0.72);
        icon_.paint(painter, icon_rect, Qt::AlignCenter, mode);
        painter->restore();
    }

  private:
    QIcon icon_;
};

[[nodiscard]] QString formatTime(const qint64 milliseconds) {
    if (milliseconds < 0) {
        return QStringLiteral("−:−−");
    }
    const auto total_seconds = milliseconds / 1'000;
    return QStringLiteral("%1:%2")
        .arg(total_seconds / 60)
        .arg(total_seconds % 60, 2, 10, QLatin1Char('0'));
}

[[nodiscard]] QString formatLongTime(const qint64 milliseconds) {
    const auto total_seconds = std::max<qint64>(0, milliseconds / 1'000);
    const auto hours = total_seconds / 3'600;
    return QStringLiteral("%1:%2:%3")
        .arg(hours)
        .arg((total_seconds / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(total_seconds % 60, 2, 10, QLatin1Char('0'));
}

void configureTrackView(QTableView* view, QAbstractItemModel* model) {
    view->setModel(model);
    view->setAlternatingRowColors(true);
    view->setShowGrid(false);
    view->setProperty(hover_row_property, -1);
    view->setMouseTracking(true);
    view->viewport()->setMouseTracking(true);
    view->setItemDelegate(new RowHoverDelegate(view));
    view->viewport()->installEventFilter(new RowHoverFilter(view));
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setSortingEnabled(false);
    view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->verticalHeader()->setDefaultSectionSize(22);
    view->verticalHeader()->setMinimumSectionSize(18);
    view->verticalHeader()->hide();
    view->horizontalHeader()->setSectionsMovable(true);
    view->horizontalHeader()->setHighlightSections(false);
    view->horizontalHeader()->setStretchLastSection(false);
    view->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    view->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    view->setColumnWidth(0, 180);
    view->setColumnWidth(2, 220);
    view->setColumnWidth(3, 72);
    view->setColumnWidth(4, 60);
    view->setColumnWidth(5, 72);
}

[[nodiscard]] QTableView* makeTrackView(QAbstractItemModel* model, QWidget* parent,
                                        const bool queue = false) {
    auto* view =
        queue ? static_cast<QTableView*>(new QueueTableView(parent)) : new QTableView(parent);
    configureTrackView(view, model);
    return view;
}

[[nodiscard]] QVariantList selectedRows(const QTableView* view) {
    QVariantList rows;
    if (view == nullptr || view->selectionModel() == nullptr) {
        return rows;
    }
    auto indexes = view->selectionModel()->selectedRows(0);
    std::ranges::sort(indexes, {}, &QModelIndex::row);
    rows.reserve(indexes.size());
    for (const auto& index : indexes) {
        rows.push_back(index.row());
    }
    return rows;
}

[[nodiscard]] mpd::Track trackFromListItem(const persistence::ListItem& item) {
    std::vector<mpd::Pair> fields;
    fields.reserve(item.fields.size());
    for (const auto& field : item.fields) {
        fields.push_back(mpd::Pair{.name = field.name, .value = field.value});
    }
    mpd::Metadata metadata{std::move(fields)};
    return mpd::Track{
        .uri = item.source_reference,
        .metadata = metadata,
        .musicbrainz = mpd::project_musicbrainz(metadata),
        .queue_id = std::nullopt,
        .queue_position = std::nullopt,
        .duration = item.duration_ms ? std::optional{std::chrono::milliseconds{*item.duration_ms}}
                                     : std::nullopt,
        .last_modified = std::nullopt,
        .audio_format = std::nullopt,
        .priority = std::nullopt,
        .unknown_structural_pairs = {},
    };
}

[[nodiscard]] persistence::ListItem listItemFromTrack(const mpd::Track& track,
                                                      const core::StableId& profile_id) {
    std::vector<persistence::SnapshotField> fields;
    fields.reserve(track.metadata.fields().size());
    for (const auto& field : track.metadata.fields()) {
        fields.push_back(persistence::SnapshotField{.name = field.name, .value = field.value});
    }
    return persistence::ListItem{
        .source = persistence::ListSource::mpd,
        .profile_id = profile_id,
        .source_reference = track.uri,
        .logical_reference = std::nullopt,
        .segment = std::nullopt,
        .source_selection = std::nullopt,
        .duration_ms = track.duration ? std::optional{track.duration->count()} : std::nullopt,
        .fields = std::move(fields),
    };
}

[[nodiscard]] QIcon themedIcon(const QString& name, QStyle* style,
                               const QStyle::StandardPixmap fallback) {
    return QIcon::fromTheme(name, style->standardIcon(fallback));
}

// Expands every row that survives the active library filter so the tree
// shrinks to visible results instead of a list of collapsed artists. Unloaded
// lazy roots are fetched too, but only while the visible root count is small
// enough to keep the resulting server traffic bounded. Expansion uses one
// expandAll() layout pass — per-row expand() forces a relayout each time and
// turns typing into quadratic work on larger trees.
void expandFilteredServerTree(QTreeView* tree) {
    auto* model = tree->model();
    constexpr int auto_fetch_root_limit = 8;
    const auto top_level = model->rowCount();
    if (top_level <= auto_fetch_root_limit) {
        for (int row = 0; row < top_level; ++row) {
            if (const auto index = model->index(row, 0); model->canFetchMore(index)) {
                model->fetchMore(index);
            }
        }
    }
    tree->expandAll();
}

} // namespace

struct MainWindow::Impl {
    enum class LibraryMode { none, browser, playlist_index };

    quick::MpdProbeController* controller{nullptr};
    qtmodels::PagedTrackModel* scratch_model{nullptr};
    QStandardItemModel* empty_model{nullptr};
    QStandardItemModel* tag_model{nullptr};
    QDockWidget* library_dock{nullptr};
    QDockWidget* details_dock{nullptr};
    QDockWidget* jobs_dock{nullptr};
    QTabWidget* library_source_tabs{nullptr};
    ServerLibraryTreeModel* server_library_model{nullptr};
    ServerLibraryFilterModel* server_library_filter_model{nullptr};
    QLineEdit* server_library_filter{nullptr};
    QTimer* server_library_filter_delay{nullptr};
    quint64 server_library_filter_generation{0U};
    ServerLibraryTreeView* server_library_tree{nullptr};
    QPushButton* server_folders{nullptr};
    QPushButton* server_playlists{nullptr};
    QPushButton* configure_server_tree{nullptr};
    QStackedWidget* center_stack{nullptr};
    QTabWidget* tabs{nullptr};
    QTableView* queue_view{nullptr};
    QTableView* scratch_view{nullptr};
    QWidget* search_page{nullptr};
    quick::MpdSearchResultModel* live_search_model{nullptr};
    LiveSearchTableView* search_view{nullptr};
    QLabel* search_status{nullptr};
    QWidget* library_page{nullptr};
    QTableView* library_view{nullptr};
    QLabel* library_title{nullptr};
    QLabel* library_status{nullptr};
    QPushButton* browser_back{nullptr};
    QLineEdit* search{nullptr};
    QTimer* search_delay{nullptr};
    QLabel* details{nullptr};
    QLabel* cover_art{nullptr};
    QLabel* now_playing{nullptr};
    QLabel* now_playing_detail{nullptr};
    QLabel* queue_summary{nullptr};
    QLabel* elapsed{nullptr};
    QLabel* duration{nullptr};
    QLabel* metrics{nullptr};
    QSlider* seek{nullptr};
    QSlider* volume{nullptr};
    QToolBar* transport{nullptr};
    QToolBar* progress_toolbar{nullptr};
    QLabel* toast{nullptr};
    QTimer* toast_timer{nullptr};
    QPointer<quick::MpdQueueModel> pending_search_model;
    QPointer<QLabel> pending_search_status;
    QString pending_search_query;
    QAction* connect_action{nullptr};
    QAction* disconnect_action{nullptr};
    QAction* previous_action{nullptr};
    QAction* play_pause_action{nullptr};
    QAction* stop_action{nullptr};
    QAction* next_action{nullptr};
    QAction* repeat_action{nullptr};
    QAction* random_action{nullptr};
    QAction* single_cycle_action{nullptr};
    QAction* consume_cycle_action{nullptr};
    QAction* activate_action{nullptr};
    QAction* add_action{nullptr};
    QAction* add_next_action{nullptr};
    QAction* remove_action{nullptr};
    QAction* move_up_action{nullptr};
    QAction* move_down_action{nullptr};
    QAction* crop_action{nullptr};
    QAction* clear_action{nullptr};
    QAction* sort_action{nullptr};
    QAction* reverse_action{nullptr};
    QAction* randomize_list_action{nullptr};
    QAction* deduplicate_action{nullptr};
    QAction* load_more_search_action{nullptr};
    QMenu* priority_menu{nullptr};
    QAction* save_queue_playlist_action{nullptr};
    QAction* playlist_load_action{nullptr};
    QAction* playlist_clear_action{nullptr};
    QAction* playlist_rename_action{nullptr};
    QAction* playlist_delete_action{nullptr};
    QAction* new_scratch_action{nullptr};
    QAction* new_named_list_action{nullptr};
    QAction* duplicate_tab_action{nullptr};
    QAction* pin_tab_action{nullptr};
    QAction* save_working_list_action{nullptr};
    QAction* rename_tab_action{nullptr};
    QAction* close_tab_action{nullptr};
    QAction* move_tab_left_action{nullptr};
    QAction* move_tab_right_action{nullptr};
    QAction* command_palette_action{nullptr};
    QMenu* copy_to_menu{nullptr};
    QMenu* move_to_menu{nullptr};
    QToolButton* single_button{nullptr};
    QToolButton* consume_button{nullptr};
    QToolButton* replay_gain_button{nullptr};
    QToolButton* output_button{nullptr};
    QToolButton* list_button{nullptr};
    QToolButton* add_button{nullptr};
    QToolButton* add_next_button{nullptr};
    QMenu* track_context_menu{nullptr};
    QMenu* tab_context_menu{nullptr};
    QMenu* replay_gain_menu{nullptr};
    QMenu* output_menu{nullptr};
    QMenu* profile_menu{nullptr};
    QActionGroup* replay_gain_group{nullptr};
    LibraryMode library_mode{LibraryMode::none};
    bool seeking{false};
    bool changing_volume{false};
    qint64 update_cursor{0};
    bool synthetic_scratch{false};
    ListPersistenceService* persistence{nullptr};
    QTimer* persistence_timer{nullptr};
    bool persistence_ready{false};
    std::vector<persistence::ConnectionProfile> profiles;
    std::vector<persistence::ListDocument> pending_documents;
    std::vector<persistence::TrackViewPreset> pending_view_presets;
    QHash<QString, persistence::ListDocument> local_documents;
    QHash<QString, QByteArray> view_presets;
    QString persistence_error;
    QCache<QString, QPixmap> artwork_cache{8 * 1024};
    QString current_artwork_uri;
    bool server_tree_connected{false};
    QString server_tree_profile;
    enum class ServerTreeAction { append, insert, replace, add_to_list };
    std::optional<ServerTreeAction> pending_server_tree_action;
    QPersistentModelIndex pending_server_tree_index;
    QString pending_server_tree_target;
};

MainWindow::MainWindow(const qint64 logical_rows, QWidget* parent)
    : QMainWindow(parent), implementation_(std::make_unique<Impl>()) {
    setObjectName(QStringLiteral("trackknife-main-window"));
    setWindowTitle(QStringLiteral("Trackknife"));
    setDockNestingEnabled(true);
    setAnimated(false);
    setMinimumSize(900, 600);
    resize(1280, 800);
    setProperty("trackknife-persistence-ready", logical_rows > 0);
    buildWorkspace(logical_rows);
    restoreWorkspace();
    refreshUi();
    initializePersistence();
}

MainWindow::~MainWindow() {
    persistLocalTabs();
    flushLocalTabs();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    persistLocalTabs();
    flushLocalTabs();
    QSettings settings;
    settings.setValue(QStringLiteral("workspace/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("workspace/state"), saveState(layout_version));
    const auto& ui = *implementation_;
    const auto library_area = dockWidgetArea(ui.library_dock);
    if (!ui.library_dock->isFloating() &&
        (library_area == Qt::LeftDockWidgetArea || library_area == Qt::RightDockWidgetArea)) {
        settings.setValue(QStringLiteral("workspace/library-dock-width"), ui.library_dock->width());
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    positionToast();
    positionLiveSearchSurface();
}

void MainWindow::showToast(const QString& message) {
    auto& ui = *implementation_;
    if (message.isEmpty() || ui.toast == nullptr) {
        return;
    }
    ui.toast->setMaximumWidth(std::min(600, width() - 40));
    ui.toast->setText(message);
    ui.toast->adjustSize();
    positionToast();
    ui.toast->show();
    ui.toast->raise();
    ui.toast_timer->start();
}

void MainWindow::positionToast() {
    auto& ui = *implementation_;
    if (ui.toast == nullptr) {
        return;
    }
    const auto x = std::max(8, (width() - ui.toast->width()) / 2);
    const auto status_top = statusBar() != nullptr ? statusBar()->geometry().top() : height();
    const auto y = std::max(8, status_top - ui.toast->height() - 8);
    ui.toast->move(x, y);
}

void MainWindow::positionLiveSearchSurface() {
    auto& ui = *implementation_;
    if (ui.search_page == nullptr || ui.search == nullptr) {
        return;
    }
    const auto anchor = ui.search->mapTo(this, QPoint{ui.search->width(), ui.search->height()});
    const auto maximum_width = std::max(520, width() - 24);
    const auto surface_width = std::clamp(width() * 3 / 5, 520, maximum_width);
    const auto x =
        std::clamp(anchor.x() - surface_width, 12, std::max(12, width() - 12 - surface_width));
    const auto y = anchor.y() + 4;
    const auto available_bottom = statusBar() != nullptr ? statusBar()->geometry().top() : height();
    const auto surface_height = std::min(480, std::max(220, available_bottom - y - 12));
    ui.search_page->setGeometry(x, y, surface_width, surface_height);
}

void MainWindow::buildWorkspace(const qint64 logical_rows) {
    auto& ui = *implementation_;
    ui.controller = new quick::MpdProbeController(this);
    ui.synthetic_scratch = logical_rows > 0;
    if (ui.synthetic_scratch) {
        ui.scratch_model = new qtmodels::PagedTrackModel(logical_rows, this);
    } else {
        auto database_path = qApp->property("trackknife-state-database-path").toString();
        if (database_path.isEmpty()) {
            const auto state_directory =
                QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
            QDir{}.mkpath(state_directory);
            database_path = QDir{state_directory}.filePath(QStringLiteral("state.sqlite3"));
        }
        ui.persistence = new ListPersistenceService(
            std::filesystem::path{QFile::encodeName(database_path).toStdString()}, this);
        ui.persistence_timer = new QTimer(this);
        ui.persistence_timer->setSingleShot(true);
        ui.persistence_timer->setInterval(150);
        connect(ui.persistence_timer, &QTimer::timeout, this, [this] {
            auto& state = *implementation_;
            if (state.persistence == nullptr || !state.persistence_ready) {
                return;
            }
            state.persistence->saveWorkspace(state.pending_documents, state.pending_view_presets,
                                             [this](const QString& error) {
                                                 if (!error.isEmpty()) {
                                                     implementation_->persistence_error = error;
                                                     showToast(error);
                                                 }
                                             });
        });
    }
    ui.empty_model = new QStandardItemModel(0, 6, this);
    ui.empty_model->setHorizontalHeaderLabels(
        {QStringLiteral("Artist"), QStringLiteral("Title"), QStringLiteral("Album"),
         QStringLiteral("Date"), QStringLiteral("Track"), QStringLiteral("Duration")});
    ui.tag_model = new QStandardItemModel(0, 1, this);
    ui.tag_model->setHorizontalHeaderLabels({QStringLiteral("Value")});

    ui.transport = new QToolBar(QStringLiteral("Playback and search"), this);
    addToolBar(Qt::TopToolBarArea, ui.transport);
    ui.transport->setObjectName(QStringLiteral("toolbar-transport"));
    ui.transport->setMovable(false);
    ui.transport->setFloatable(false);
    ui.transport->setIconSize(QSize(22, 22));
    ui.transport->addWidget(
        makeChromeEdgeSpacer(ui.transport, QStringLiteral("transport-left-edge-padding")));
    ui.previous_action = ui.transport->addAction(
        themedIcon(QStringLiteral("media-skip-backward"), style(), QStyle::SP_MediaSkipBackward),
        QStringLiteral("Previous"));
    ui.previous_action->setObjectName(QStringLiteral("action-playback-previous"));
    ui.play_pause_action = ui.transport->addAction(
        themedIcon(QStringLiteral("media-playback-start"), style(), QStyle::SP_MediaPlay),
        QStringLiteral("Play"));
    ui.play_pause_action->setObjectName(QStringLiteral("play-pause-button"));
    ui.stop_action = ui.transport->addAction(
        themedIcon(QStringLiteral("media-playback-stop"), style(), QStyle::SP_MediaStop),
        QStringLiteral("Stop"));
    ui.stop_action->setObjectName(QStringLiteral("action-playback-stop"));
    ui.next_action = ui.transport->addAction(
        themedIcon(QStringLiteral("media-skip-forward"), style(), QStyle::SP_MediaSkipForward),
        QStringLiteral("Next"));
    ui.next_action->setObjectName(QStringLiteral("action-playback-next"));

    ui.cover_art = new QLabel(ui.transport);
    ui.cover_art->setObjectName(QStringLiteral("now-playing-cover"));
    ui.cover_art->setFixedSize(46, 46);
    ui.cover_art->setAlignment(Qt::AlignCenter);
    ui.cover_art->setPixmap(
        themedIcon(QStringLiteral("media-optical-audio"), style(), QStyle::SP_FileIcon)
            .pixmap(38, 38));
    ui.transport->addWidget(ui.cover_art);
    auto* track_info = new QWidget(ui.transport);
    auto* track_info_layout = new QVBoxLayout(track_info);
    track_info_layout->setContentsMargins(6, 0, 12, 0);
    track_info_layout->setSpacing(0);
    ui.now_playing = new QLabel(QStringLiteral("Nothing playing"), track_info);
    ui.now_playing->setObjectName(QStringLiteral("now-playing"));
    auto title_font = ui.now_playing->font();
    title_font.setBold(true);
    ui.now_playing->setFont(title_font);
    ui.now_playing_detail = new QLabel(QStringLiteral("Connect to MPD or Melody"), track_info);
    ui.now_playing_detail->setObjectName(QStringLiteral("now-playing-detail"));
    auto detail_font = ui.now_playing_detail->font();
    detail_font.setPointSizeF(std::max(7.0, detail_font.pointSizeF() - 1.0));
    ui.now_playing_detail->setFont(detail_font);
    ui.now_playing->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    ui.now_playing_detail->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    track_info_layout->addWidget(ui.now_playing);
    track_info_layout->addWidget(ui.now_playing_detail);
    track_info->setMinimumWidth(220);
    track_info->setMaximumWidth(520);
    track_info->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    ui.transport->addWidget(track_info);

    auto* search_spacer = new QWidget(ui.transport);
    search_spacer->setObjectName(QStringLiteral("toolbar-search-spacer"));
    search_spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    ui.transport->addWidget(search_spacer);

    ui.connect_action =
        new QAction(themedIcon(QStringLiteral("network-connect"), style(), QStyle::SP_DriveNetIcon),
                    QStringLiteral("Connect"), this);
    ui.connect_action->setObjectName(QStringLiteral("action-connect-mpd"));
    ui.connect_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+K")));
    connect(ui.connect_action, &QAction::triggered, this, &MainWindow::openMpdConnectionDialog);
    ui.disconnect_action = new QAction(QStringLiteral("Disconnect"), this);
    ui.disconnect_action->setObjectName(QStringLiteral("action-disconnect-mpd"));
    connect(ui.disconnect_action, &QAction::triggered, ui.controller,
            &quick::MpdProbeController::disconnectFromServer);

    ui.search = new LiveSearchLineEdit(ui.transport);
    ui.search->setObjectName(QStringLiteral("global-search"));
    ui.search->setClearButtonEnabled(true);
    ui.search->setPlaceholderText(QStringLiteral("Server library…"));
    ui.search->setMinimumWidth(260);
    ui.search->setMaximumWidth(360);
    ui.search->addAction(
        themedIcon(QStringLiteral("edit-find"), style(), QStyle::SP_FileDialogContentsView),
        QLineEdit::LeadingPosition);
    ui.transport->addWidget(ui.search);
    ui.transport->addWidget(
        makeChromeEdgeSpacer(ui.transport, QStringLiteral("transport-right-edge-padding")));

    ui.search_delay = new QTimer(this);
    ui.search_delay->setSingleShot(true);
    ui.search_delay->setInterval(180);
    connect(ui.search, &QLineEdit::textEdited, this, [this] {
        auto& state = *implementation_;
        state.search_page->show();
        state.search_page->raise();
        positionLiveSearchSurface();
        state.search_delay->start();
        refreshUi();
    });
    connect(ui.search_delay, &QTimer::timeout, this, &MainWindow::previewSearch);
    auto* focus_search = new QShortcut(QKeySequence(QStringLiteral("Ctrl+L")), this);
    connect(focus_search, &QShortcut::activated, this, [this] {
        auto& state = *implementation_;
        const auto reopening = !state.search_page->isVisible();
        state.search_page->show();
        state.search_page->raise();
        positionLiveSearchSurface();
        state.search->setFocus();
        state.search->selectAll();
        if (reopening && !state.search->text().isEmpty()) {
            state.search_view->focusCurrentResult();
        }
    });

    auto* central = new QWidget(this);
    auto* central_layout = new QVBoxLayout(central);
    central_layout->setContentsMargins(0, 0, 0, 0);
    central_layout->setSpacing(0);

    ui.center_stack = new QStackedWidget(central);
    ui.center_stack->setObjectName(QStringLiteral("center-stack"));
    ui.tabs = new QTabWidget(ui.center_stack);
    ui.tabs->setObjectName(QStringLiteral("track-tabs"));
    ui.tabs->setDocumentMode(true);
    ui.tabs->setMovable(true);
    ui.queue_view = makeTrackView(ui.controller->queueModel(), ui.tabs, true);
    ui.queue_view->setObjectName(QStringLiteral("track-view-queue"));
    ui.queue_view->setDragEnabled(true);
    ui.queue_view->setAcceptDrops(true);
    ui.queue_view->setDropIndicatorShown(true);
    ui.queue_view->setDragDropOverwriteMode(false);
    ui.queue_view->setDragDropMode(QAbstractItemView::InternalMove);
    ui.queue_view->setDefaultDropAction(Qt::MoveAction);
    static_cast<QueueTableView*>(ui.queue_view)
        ->setReorderCallback(
            [controller = ui.controller](const QVariantList& rows, const int insertion_row) {
                controller->moveQueueItems(rows, insertion_row);
            });
    ui.queue_view->setAlternatingRowColors(false);
    ui.queue_view->setShowGrid(false);
    ui.queue_view->setWordWrap(false);
    ui.queue_view->verticalHeader()->hide();
    ui.queue_view->horizontalHeader()->hide();
    ui.queue_view->horizontalHeader()->setStretchLastSection(false);
    ui.queue_view->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    ui.queue_view->setItemDelegate(new QueueItemDelegate(ui.queue_view));
    ui.queue_view->setColumnHidden(track_number_column, true);
    ui.queue_view->setColumnHidden(track_album_column, true);
    ui.queue_view->setColumnHidden(track_date_column, true);
    constexpr std::array queue_column_order{
        track_artwork_column, track_artist_column, track_number_column, track_title_column,
        track_album_column,   track_date_column,   track_length_column};
    for (int visual = 0; visual < static_cast<int>(queue_column_order.size()); ++visual) {
        const auto logical = queue_column_order.at(static_cast<std::size_t>(visual));
        ui.queue_view->horizontalHeader()->moveSection(
            ui.queue_view->horizontalHeader()->visualIndex(logical), visual);
    }
    ui.queue_view->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    ui.queue_view->horizontalHeader()->setSectionResizeMode(track_title_column,
                                                            QHeaderView::Stretch);
    ui.queue_view->setColumnWidth(track_artwork_column, 62);
    ui.queue_view->setColumnWidth(track_length_column, 68);
    if (const auto state = ui.view_presets.value(QStringLiteral("live-queue")); !state.isEmpty()) {
        static_cast<void>(ui.queue_view->horizontalHeader()->restoreState(state));
    }
    ui.tabs->addTab(ui.queue_view, QStringLiteral("Live queue"));
    ui.center_stack->addWidget(ui.tabs);
    if (ui.synthetic_scratch) {
        ui.scratch_view = makeTrackView(ui.scratch_model, ui.tabs);
        ui.scratch_view->setObjectName(QStringLiteral("track-view-library"));
        ui.scratch_view->setProperty("trackknife-local-list-tab", true);
        ui.tabs->addTab(ui.scratch_view, QStringLiteral("Scratch 1"));
    } else {
        createScratchTab();
    }

    auto* search_surface = new QFrame(this);
    search_surface->setFrameShape(QFrame::StyledPanel);
    search_surface->setFrameShadow(QFrame::Raised);
    search_surface->setAutoFillBackground(true);
    search_surface->hide();
    ui.search_page = search_surface;
    ui.search_page->setObjectName(QStringLiteral("live-search-surface"));
    auto* search_layout = new QVBoxLayout(ui.search_page);
    search_layout->setContentsMargins(0, 0, 0, 0);
    search_layout->setSpacing(0);
    ui.live_search_model = new quick::MpdSearchResultModel(ui.search_page);
    ui.live_search_model->setAlbumPlaceholder(
        themedIcon(QStringLiteral("media-optical-audio"), style(), QStyle::SP_FileIcon));
    ui.search_view = new LiveSearchTableView(ui.search_page);
    configureTrackView(ui.search_view, ui.live_search_model);
    ui.search_view->setObjectName(QStringLiteral("track-view-live-search"));
    ui.search_view->setAccessibleName(QStringLiteral("Live library search results"));
    ui.search_view->setAccessibleDescription(QStringLiteral(
        "Type to continue the query. Use Up and Down for results, Left and Right for actions, "
        "Enter activates the selected action, Shift Enter keeps the search as a tab, and Control "
        "Enter replaces the queue."));
    ui.search_view->setSearchField(ui.search);
    ui.search_view->setSelectionMode(QAbstractItemView::SingleSelection);
    ui.search_view->setAlternatingRowColors(false);
    ui.search_view->setIconSize(QSize{30, 30});
    ui.search_view->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    ui.search_view->horizontalHeader()->hide();
    ui.search_view->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    ui.search_view->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    ui.search_view->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    ui.search_view->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Interactive);
    ui.search_view->horizontalHeader()->setMinimumSectionSize(18);
    ui.search_view->setColumnWidth(0, 190);
    ui.search_view->setColumnWidth(2, 230);
    ui.search_view->setColumnWidth(3, 92);
    for (int column = quick::MpdSearchResultModel::first_action_column;
         column < quick::MpdSearchResultModel::column_count; ++column) {
        ui.search_view->horizontalHeader()->setSectionResizeMode(column, QHeaderView::Fixed);
        ui.search_view->horizontalHeader()->resizeSection(column, 24);
    }
    ui.search_view->setItemDelegateForColumn(
        4, new SearchActionDelegate(
               themedIcon(QStringLiteral("list-add"), style(), QStyle::SP_ArrowRight),
               ui.search_view));
    ui.search_view->setItemDelegateForColumn(
        5, new SearchActionDelegate(
               themedIcon(QStringLiteral("go-next"), style(), QStyle::SP_ArrowForward),
               ui.search_view));
    ui.search_view->setItemDelegateForColumn(
        6, new SearchActionDelegate(
               themedIcon(QStringLiteral("media-playback-start"), style(), QStyle::SP_MediaPlay),
               ui.search_view));
    search_layout->addWidget(ui.search_view, 1);
    ui.search_status =
        new QLabel(QStringLiteral("Type at least two characters to search"), ui.search_page);
    ui.search_status->setObjectName(QStringLiteral("search-status"));
    ui.search_status->setContentsMargins(4, 2, 4, 2);
    search_layout->addWidget(ui.search_status);
    ui.search_view->setCloseCallback([this] { closeLiveSearch(); });
    ui.search_view->setCommitCallback([this] { commitSearch(); });
    const auto activate_live_result = [this](const int row, const SearchQueueAction action) {
        auto& state = *implementation_;
        if (const auto album = state.live_search_model->albumAt(row)) {
            const auto mode = action == SearchQueueAction::append   ? quick::QueueAddMode::append
                              : action == SearchQueueAction::insert ? quick::QueueAddMode::next
                                                                    : quick::QueueAddMode::replace;
            state.controller->addAlbum(*album, mode);
            return;
        }
        const auto uris = state.live_search_model->urisAt(row);
        if (uris.isEmpty()) {
            return;
        }
        switch (action) {
        case SearchQueueAction::append:
            state.controller->addUris(uris, false);
            break;
        case SearchQueueAction::insert:
            state.controller->addUris(uris, true);
            break;
        case SearchQueueAction::replace:
            state.controller->replaceQueueWithUris(uris);
            break;
        }
    };
    ui.search_view->setActionCallback(activate_live_result);
    connect(ui.search_view, &QTableView::clicked, this,
            [activate_live_result](const QModelIndex& index) {
                if (index.column() < quick::MpdSearchResultModel::first_action_column) {
                    return;
                }
                const auto action = index.column() == 4   ? SearchQueueAction::append
                                    : index.column() == 5 ? SearchQueueAction::insert
                                                          : SearchQueueAction::replace;
                activate_live_result(index.row(), action);
            });
    auto* live_search_field = static_cast<LiveSearchLineEdit*>(ui.search);
    live_search_field->setResultFocusCallback(
        [view = ui.search_view] { view->focusFirstResult(); });
    live_search_field->setCloseCallback([this] { closeLiveSearch(); });
    live_search_field->setActionCallback(
        [view = ui.search_view](const SearchQueueAction action) { view->activateDefault(action); });
    live_search_field->setCommitCallback([this] { commitSearch(); });

    ui.library_page = new QWidget(ui.center_stack);
    auto* library_layout = new QVBoxLayout(ui.library_page);
    library_layout->setContentsMargins(4, 4, 4, 4);
    library_layout->setSpacing(3);
    auto* library_header = new QHBoxLayout;
    ui.browser_back =
        new QPushButton(themedIcon(QStringLiteral("go-up"), style(), QStyle::SP_ArrowBack),
                        QStringLiteral("Back"), ui.library_page);
    ui.browser_back->setObjectName(QStringLiteral("browser-back"));
    library_header->addWidget(ui.browser_back);
    ui.library_title = new QLabel(QStringLiteral("Music root"), ui.library_page);
    ui.library_title->setObjectName(QStringLiteral("library-title"));
    library_header->addWidget(ui.library_title, 1);
    auto* close_library = new QPushButton(QStringLiteral("Close"), ui.library_page);
    connect(close_library, &QPushButton::clicked, this, [this] {
        implementation_->library_mode = Impl::LibraryMode::none;
        implementation_->center_stack->setCurrentWidget(implementation_->tabs);
        refreshUi();
    });
    library_header->addWidget(close_library);
    library_layout->addLayout(library_header);
    ui.library_view = makeTrackView(ui.empty_model, ui.library_page, true);
    ui.library_view->setObjectName(QStringLiteral("track-view-server-library"));
    library_layout->addWidget(ui.library_view, 1);
    ui.library_status = new QLabel(QStringLiteral("No folder loaded"), ui.library_page);
    ui.library_status->setObjectName(QStringLiteral("library-status"));
    library_layout->addWidget(ui.library_status);
    ui.center_stack->addWidget(ui.library_page);
    central_layout->addWidget(ui.center_stack, 1);
    setCentralWidget(central);

    ui.library_dock = new QDockWidget(QStringLiteral("Library"), this);
    ui.library_dock->setObjectName(QStringLiteral("panel-library"));
    ui.library_source_tabs = new QTabWidget(ui.library_dock);
    ui.library_source_tabs->setObjectName(QStringLiteral("library-source-tabs"));
    ui.library_source_tabs->setDocumentMode(true);
    auto* server_library_page = new QWidget(ui.library_source_tabs);
    server_library_page->setObjectName(QStringLiteral("server-library-page"));
    auto* server_library_layout = new QVBoxLayout(server_library_page);
    server_library_layout->setContentsMargins(3, 3, 3, 3);
    server_library_layout->setSpacing(3);
    auto* server_library_actions = new QHBoxLayout;
    ui.server_folders = new QPushButton(QStringLiteral("Folders"), server_library_page);
    ui.server_folders->setObjectName(QStringLiteral("server-library-folders"));
    server_library_actions->addWidget(ui.server_folders);
    ui.server_playlists = new QPushButton(QStringLiteral("Playlists"), server_library_page);
    ui.server_playlists->setObjectName(QStringLiteral("server-library-playlists"));
    server_library_actions->addWidget(ui.server_playlists);
    ui.configure_server_tree = new QPushButton(QStringLiteral("Configure…"), server_library_page);
    ui.configure_server_tree->setObjectName(QStringLiteral("server-library-configure"));
    server_library_actions->addWidget(ui.configure_server_tree);
    server_library_layout->addLayout(server_library_actions);
    ui.server_library_model = new ServerLibraryTreeModel(server_library_page);
    QSettings server_library_settings;
    const auto saved_tree =
        server_library_settings.value(QStringLiteral("server/library-tree-definition"))
            .toByteArray();
    if (!saved_tree.isEmpty()) {
        QString error;
        if (auto definition = deserializeLibraryTreeDefinition(saved_tree, &error)) {
            (void)ui.server_library_model->setDefinition(std::move(*definition));
        }
    }
    ui.server_library_filter = new QLineEdit(server_library_page);
    ui.server_library_filter->setObjectName(QStringLiteral("server-library-filter"));
    ui.server_library_filter->setClearButtonEnabled(true);
    ui.server_library_filter->setPlaceholderText(QStringLiteral("Filter library tree…"));
    ui.server_library_filter->setAccessibleName(QStringLiteral("Filter server library tree"));
    server_library_layout->addWidget(ui.server_library_filter);
    ui.server_library_filter_model = new ServerLibraryFilterModel(server_library_page);
    ui.server_library_filter_model->setSourceModel(ui.server_library_model);
    ui.server_library_tree = new ServerLibraryTreeView(server_library_page);
    ui.server_library_tree->setObjectName(QStringLiteral("server-library-tree"));
    ui.server_library_tree->setModel(ui.server_library_filter_model);
    ui.server_library_tree->setHeaderHidden(true);
    ui.server_library_tree->setUniformRowHeights(false);
    ui.server_library_tree->setIconSize(QSize{32, 32});
    ui.server_library_tree->setIndentation(18);
    ui.server_library_tree->setAnimated(true);
    ui.server_library_tree->setExpandsOnDoubleClick(false);
    ui.server_library_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    const std::array server_tree_action_icons{
        themedIcon(QStringLiteral("list-add"), style(), QStyle::SP_DialogOpenButton),
        themedIcon(QStringLiteral("go-next"), style(), QStyle::SP_ArrowRight),
        themedIcon(QStringLiteral("media-playback-start"), style(), QStyle::SP_MediaPlay)};
    ui.server_library_tree->setItemDelegate(
        new ServerLibraryTreeDelegate(ui.server_library_tree, server_tree_action_icons));
    ui.server_library_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    ui.server_library_tree->setAccessibleName(QStringLiteral("Server library"));
    server_library_layout->addWidget(ui.server_library_tree, 1);
    ui.library_source_tabs->addTab(server_library_page, QStringLiteral("Server"));
    ui.server_library_filter_delay = new QTimer(this);
    ui.server_library_filter_delay->setSingleShot(true);
    ui.server_library_filter_delay->setInterval(300);
    // Typing only refilters, which is cheap against precomputed filter text.
    // Expansion, lazy-branch fetches, and the bounded server search all wait
    // for the debounce: doing any of them per keystroke floods the session
    // command queue and pays a full tree relayout per character.
    connect(ui.server_library_filter, &QLineEdit::textChanged, this, [this](const QString& text) {
        auto& state = *implementation_;
        state.server_library_filter_model->setFilterFixedString(text);
        // Stale server matches must not keep revealing roots for an older query.
        state.server_library_filter_model->clearServerMatches();
        ++state.server_library_filter_generation;
        if (text.trimmed().isEmpty()) {
            state.server_library_filter_delay->stop();
            state.server_library_tree->collapseAll();
            return;
        }
        state.server_library_filter_delay->start();
    });
    connect(ui.server_library_filter_delay, &QTimer::timeout, this, [this] {
        auto& state = *implementation_;
        const auto text = state.server_library_filter->text().trimmed();
        if (text.isEmpty()) {
            return;
        }
        expandFilteredServerTree(state.server_library_tree);
        if (text.size() >= 2 && state.controller->connected()) {
            state.controller->searchServerLibraryFilter(state.server_library_filter_generation,
                                                        state.server_library_model->activeRootTag(),
                                                        text);
        }
    });
    connect(ui.controller, &quick::MpdProbeController::serverLibraryFilterLoaded, this,
            [this](const quint64 token, const QStringList& root_values, const QString& error) {
                auto& state = *implementation_;
                if (token != state.server_library_filter_generation || !error.isEmpty()) {
                    return;
                }
                state.server_library_filter_model->setServerMatches(root_values);
                if (!state.server_library_filter->text().trimmed().isEmpty()) {
                    expandFilteredServerTree(state.server_library_tree);
                }
            });

    ui.library_dock->setWidget(ui.library_source_tabs);
    addDockWidget(Qt::LeftDockWidgetArea, ui.library_dock);

    connect(ui.server_folders, &QPushButton::clicked, this, [this] {
        auto& state = *implementation_;
        state.library_mode = Impl::LibraryMode::browser;
        state.center_stack->setCurrentWidget(state.library_page);
        setLibraryModel(state.controller->browserModel());
        state.library_view->setProperty("trackknife-library-index-kind", QString{});
        state.controller->browseDirectory(QString{});
        refreshUi();
    });
    connect(ui.server_playlists, &QPushButton::clicked, this, [this] {
        auto& state = *implementation_;
        state.library_mode = Impl::LibraryMode::playlist_index;
        state.center_stack->setCurrentWidget(state.library_page);
        state.library_title->setText(QStringLiteral("Stored playlists"));
        state.tag_model->clear();
        state.tag_model->setHorizontalHeaderLabels({QStringLiteral("Playlist")});
        setLibraryModel(state.tag_model);
        state.library_view->setProperty("trackknife-library-index-kind",
                                        QStringLiteral("playlist"));
        state.controller->browseStoredPlaylists();
        refreshUi();
    });
    connect(ui.configure_server_tree, &QPushButton::clicked, this, [this] {
        auto& state = *implementation_;
        LibraryTreeEditorDialog dialog{state.server_library_model->definition(), this};
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }
        auto definition = dialog.definition();
        const auto error = state.server_library_model->setDefinition(definition);
        if (!error.isEmpty()) {
            showToast(error);
            return;
        }
        QSettings settings;
        settings.setValue(QStringLiteral("server/library-tree-definition"),
                          serializeLibraryTreeDefinition(definition));
        if (state.controller->connected()) {
            state.server_library_model->reload();
        }
    });
    connect(ui.server_library_model, &ServerLibraryTreeModel::rootRequested, ui.controller,
            &quick::MpdProbeController::loadServerLibraryRoot);
    connect(ui.server_library_model, &ServerLibraryTreeModel::branchRequested, ui.controller,
            &quick::MpdProbeController::loadServerLibraryBranch);
    connect(ui.server_library_model, &ServerLibraryTreeModel::artworkRequested, ui.controller,
            &quick::MpdProbeController::loadServerLibraryArtwork);
    connect(ui.live_search_model, &quick::MpdSearchResultModel::artworkRequested, ui.controller,
            &quick::MpdProbeController::loadServerLibraryArtwork);
    auto* queue_model = qobject_cast<quick::MpdQueueModel*>(ui.controller->queueModel());
    Q_ASSERT(queue_model != nullptr);
    connect(queue_model, &quick::MpdQueueModel::artworkRequested, ui.controller,
            &quick::MpdProbeController::loadServerLibraryArtwork);
    connect(ui.controller, &quick::MpdProbeController::serverLibraryRootLoaded,
            ui.server_library_model, &ServerLibraryTreeModel::acceptRoot);
    connect(ui.controller, &quick::MpdProbeController::serverLibraryBranchLoaded,
            ui.server_library_model, &ServerLibraryTreeModel::acceptBranch);
    connect(ui.controller, &quick::MpdProbeController::serverLibraryArtworkLoaded, this,
            [this, queue_model](const quint64 token, const QByteArray& bytes) {
                if (bytes.isEmpty()) {
                    implementation_->server_library_model->acceptArtwork(token, {});
                    implementation_->live_search_model->acceptArtwork(token, {});
                    queue_model->acceptArtwork(token, {});
                    return;
                }
                auto* watcher = new QFutureWatcher<QImage>(this);
                connect(watcher, &QFutureWatcher<QImage>::finished, this,
                        [this, watcher, token, queue_model] {
                            const auto image = watcher->result();
                            watcher->deleteLater();
                            implementation_->server_library_model->acceptArtwork(token, image);
                            implementation_->live_search_model->acceptArtwork(token, image);
                            queue_model->acceptArtwork(token, image);
                        });
                watcher->setFuture(QtConcurrent::run([bytes] {
                    const auto image = QImage::fromData(bytes);
                    return image.isNull() ? image
                                          : image.scaled(64, 64, Qt::KeepAspectRatio,
                                                         Qt::SmoothTransformation);
                }));
            });
    connect(ui.controller, &quick::MpdProbeController::serverDatabaseChanged,
            ui.server_library_model, &ServerLibraryTreeModel::reload);
    connect(ui.server_library_model, &ServerLibraryTreeModel::browseError, this,
            [this](const QString& message) {
                implementation_->pending_server_tree_action.reset();
                implementation_->pending_server_tree_index = QPersistentModelIndex{};
                implementation_->pending_server_tree_target.clear();
                implementation_->server_library_tree->cancelPendingExpansions();
                showToast(QStringLiteral("Could not browse server library: %1").arg(message));
            });
    ui.server_library_tree->setActionCallback([this](const QModelIndex& index, const int action) {
        const auto source = implementation_->server_library_filter_model->mapToSource(index);
        activateServerTreeAction(source, action);
    });
    connect(ui.server_library_tree, &QWidget::customContextMenuRequested, this,
            &MainWindow::showServerTreeContextMenu);
    connect(ui.server_library_model, &QAbstractItemModel::rowsInserted, this,
            [this](const QModelIndex&, int, int) {
                QTimer::singleShot(0, this, [this] {
                    auto& state = *implementation_;
                    state.server_library_tree->completePendingExpansions();
                    if (!state.server_library_filter->text().trimmed().isEmpty()) {
                        expandFilteredServerTree(state.server_library_tree);
                    }
                });
                completePendingServerTreeAction();
            });

    ui.details_dock = new QDockWidget(QStringLiteral("Selection"), this);
    ui.details_dock->setObjectName(QStringLiteral("panel-selection"));
    ui.details = new QLabel(QStringLiteral("Select a track"), ui.details_dock);
    ui.details->setObjectName(QStringLiteral("selection-details"));
    ui.details->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    ui.details->setWordWrap(true);
    ui.details->setMargin(8);
    ui.details_dock->setWidget(ui.details);
    addDockWidget(Qt::RightDockWidgetArea, ui.details_dock);
    ui.details_dock->hide();

    ui.jobs_dock = new QDockWidget(QStringLiteral("Jobs & diagnostics"), this);
    ui.jobs_dock->setObjectName(QStringLiteral("panel-jobs"));
    auto* jobs = new QWidget(ui.jobs_dock);
    auto* jobs_layout = new QVBoxLayout(jobs);
    ui.metrics = new QLabel(QStringLiteral("No background jobs"), jobs);
    jobs_layout->addWidget(ui.metrics);
    auto* progress = new QProgressBar(jobs);
    progress->setRange(0, 1);
    progress->setValue(0);
    progress->setTextVisible(false);
    jobs_layout->addWidget(progress);
    ui.jobs_dock->setWidget(jobs);
    addDockWidget(Qt::BottomDockWidgetArea, ui.jobs_dock);
    ui.jobs_dock->hide();

    addToolBarBreak(Qt::TopToolBarArea);
    ui.progress_toolbar = new QToolBar(QStringLiteral("Position and volume"), this);
    addToolBar(Qt::TopToolBarArea, ui.progress_toolbar);
    ui.progress_toolbar->setObjectName(QStringLiteral("toolbar-progress"));
    ui.progress_toolbar->setMovable(false);
    ui.progress_toolbar->setFloatable(false);
    ui.progress_toolbar->setIconSize(QSize(16, 16));
    ui.progress_toolbar->addWidget(
        makeChromeEdgeSpacer(ui.progress_toolbar, QStringLiteral("progress-left-edge-padding")));
    ui.elapsed = new QLabel(QStringLiteral("0:00"), ui.progress_toolbar);
    ui.progress_toolbar->addWidget(ui.elapsed);
    ui.seek = new LineSlider(ui.progress_toolbar);
    ui.seek->setObjectName(QStringLiteral("seek-slider"));
    ui.seek->setAccessibleName(QStringLiteral("Playback position"));
    ui.seek->setContentsMargins(8, 0, 8, 0);
    ui.seek->setMinimumWidth(240);
    ui.seek->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    ui.progress_toolbar->addWidget(ui.seek);
    ui.duration = new QLabel(QStringLiteral("0:00"), ui.progress_toolbar);
    ui.progress_toolbar->addWidget(ui.duration);
    ui.progress_toolbar->addSeparator();
    ui.progress_toolbar->addAction(
        themedIcon(QStringLiteral("audio-volume-high"), style(), QStyle::SP_MediaVolume),
        QStringLiteral("Volume"));
    ui.volume = new LineSlider(ui.progress_toolbar);
    ui.volume->setObjectName(QStringLiteral("volume-slider"));
    ui.volume->setAccessibleName(QStringLiteral("Volume"));
    ui.volume->setRange(0, 100);
    ui.volume->setFixedWidth(72);
    ui.volume->setToolTip(QStringLiteral("Volume"));
    ui.progress_toolbar->addWidget(ui.volume);
    ui.progress_toolbar->addWidget(
        makeChromeEdgeSpacer(ui.progress_toolbar, QStringLiteral("progress-right-edge-padding")));

    statusBar()->addWidget(
        makeChromeEdgeSpacer(statusBar(), QStringLiteral("status-left-edge-padding")));
    ui.queue_summary = new QLabel(QStringLiteral("0 tracks (0:00:00)"), statusBar());
    ui.queue_summary->setObjectName(QStringLiteral("queue-summary"));
    ui.queue_summary->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    statusBar()->addWidget(ui.queue_summary, 1);

    const auto add_status_action = [this](QAction* action) {
        auto* button = new QToolButton(statusBar());
        button->setAutoRaise(true);
        button->setDefaultAction(action);
        statusBar()->addPermanentWidget(button);
        return button;
    };
    ui.activate_action = new QAction(
        themedIcon(QStringLiteral("media-playback-start"), style(), QStyle::SP_MediaPlay),
        QStringLiteral("Play"), this);
    ui.activate_action->setObjectName(QStringLiteral("action-activate-selection"));
    ui.add_action =
        new QAction(themedIcon(QStringLiteral("list-add"), style(), QStyle::SP_ArrowRight),
                    QStringLiteral("Append to queue"), this);
    ui.add_action->setObjectName(QStringLiteral("action-add-to-queue"));
    ui.add_next_action =
        new QAction(themedIcon(QStringLiteral("go-next"), style(), QStyle::SP_ArrowForward),
                    QStringLiteral("Add next"), this);
    ui.add_next_action->setObjectName(QStringLiteral("action-add-next"));
    ui.remove_action =
        new QAction(themedIcon(QStringLiteral("edit-delete"), style(), QStyle::SP_TrashIcon),
                    QStringLiteral("Remove"), this);
    ui.remove_action->setObjectName(QStringLiteral("action-remove"));
    ui.remove_action->setShortcut(QKeySequence::Delete);
    ui.move_up_action =
        new QAction(themedIcon(QStringLiteral("go-up"), style(), QStyle::SP_ArrowUp),
                    QStringLiteral("Move up"), this);
    ui.move_up_action->setObjectName(QStringLiteral("action-move-up"));
    ui.move_up_action->setShortcut(QKeySequence(QStringLiteral("Alt+Up")));
    ui.move_down_action =
        new QAction(themedIcon(QStringLiteral("go-down"), style(), QStyle::SP_ArrowDown),
                    QStringLiteral("Move down"), this);
    ui.move_down_action->setObjectName(QStringLiteral("action-move-down"));
    ui.move_down_action->setShortcut(QKeySequence(QStringLiteral("Alt+Down")));
    addAction(ui.activate_action);
    addAction(ui.add_action);
    addAction(ui.add_next_action);
    addAction(ui.remove_action);
    addAction(ui.move_up_action);
    addAction(ui.move_down_action);
    ui.add_button = add_status_action(ui.add_action);
    ui.add_next_button = add_status_action(ui.add_next_action);
    add_status_action(ui.remove_action);
    add_status_action(ui.move_up_action);
    add_status_action(ui.move_down_action);
    ui.list_button = new QToolButton(statusBar());
    ui.list_button->setObjectName(QStringLiteral("list-actions-button"));
    ui.list_button->setIcon(
        themedIcon(QStringLiteral("view-more-symbolic"), style(), QStyle::SP_TitleBarMenuButton));
    ui.list_button->setToolTip(QStringLiteral("Queue actions"));
    ui.list_button->setAutoRaise(true);
    ui.list_button->setPopupMode(QToolButton::InstantPopup);
    auto* list_menu = new QMenu(ui.list_button);
    ui.crop_action = list_menu->addAction(QStringLiteral("Crop to selection"));
    ui.crop_action->setObjectName(QStringLiteral("action-crop-selection"));
    ui.clear_action = list_menu->addAction(QStringLiteral("Clear live queue"));
    ui.clear_action->setObjectName(QStringLiteral("action-clear-list"));
    ui.sort_action = list_menu->addAction(QStringLiteral("Sort by album and track"));
    ui.sort_action->setObjectName(QStringLiteral("action-sort-working-list"));
    ui.reverse_action = list_menu->addAction(QStringLiteral("Reverse"));
    ui.reverse_action->setObjectName(QStringLiteral("action-reverse-working-list"));
    ui.randomize_list_action = list_menu->addAction(QStringLiteral("Randomize"));
    ui.randomize_list_action->setObjectName(QStringLiteral("action-randomize-working-list"));
    ui.deduplicate_action = list_menu->addAction(QStringLiteral("Remove duplicates"));
    ui.deduplicate_action->setObjectName(QStringLiteral("action-deduplicate-working-list"));
    connect(ui.sort_action, &QAction::triggered, this, &MainWindow::sortLocalList);
    connect(ui.reverse_action, &QAction::triggered, this, &MainWindow::reverseLocalList);
    connect(ui.randomize_list_action, &QAction::triggered, this, &MainWindow::randomizeLocalList);
    connect(ui.deduplicate_action, &QAction::triggered, this, &MainWindow::deduplicateLocalList);
    ui.load_more_search_action = list_menu->addAction(QStringLiteral("Load more search results"));
    ui.load_more_search_action->setObjectName(QStringLiteral("action-load-more-search"));
    ui.load_more_search_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+L")));
    connect(ui.load_more_search_action, &QAction::triggered, ui.controller,
            &quick::MpdProbeController::continueSearch);
    addAction(ui.load_more_search_action);
    ui.copy_to_menu = list_menu->addMenu(QStringLiteral("Copy selection to"));
    ui.copy_to_menu->setObjectName(QStringLiteral("menu-copy-selection-to"));
    ui.move_to_menu = list_menu->addMenu(QStringLiteral("Move selection to"));
    ui.move_to_menu->setObjectName(QStringLiteral("menu-move-selection-to"));
    connect(list_menu, &QMenu::aboutToShow, this, &MainWindow::rebuildTransferMenus);
    ui.priority_menu = list_menu->addMenu(QStringLiteral("Priority"));
    ui.priority_menu->menuAction()->setObjectName(QStringLiteral("action-queue-priority"));
    const std::array priority_choices{
        std::pair{QStringLiteral("Normal"), 0},    std::pair{QStringLiteral("Low"), 64},
        std::pair{QStringLiteral("Medium"), 128},  std::pair{QStringLiteral("High"), 192},
        std::pair{QStringLiteral("Maximum"), 255},
    };
    for (const auto& [label, priority] : priority_choices) {
        auto* action =
            ui.priority_menu->addAction(QStringLiteral("%1 (%2)").arg(label).arg(priority));
        action->setObjectName(QStringLiteral("action-queue-priority-%1").arg(priority));
        action->setCheckable(true);
        action->setData(priority);
        connect(action, &QAction::triggered, this, [this, priority] {
            implementation_->controller->setQueuePriority(selectedRows(implementation_->queue_view),
                                                          priority);
        });
    }
    list_menu->addSeparator();
    ui.playlist_load_action = list_menu->addAction(QStringLiteral("Load into live queue"));
    ui.playlist_clear_action = list_menu->addAction(QStringLiteral("Clear server playlist"));
    ui.playlist_rename_action = list_menu->addAction(QStringLiteral("Rename server playlist…"));
    ui.playlist_delete_action = list_menu->addAction(QStringLiteral("Delete server playlist…"));
    ui.playlist_load_action->setObjectName(QStringLiteral("action-playlist-load"));
    ui.playlist_clear_action->setObjectName(QStringLiteral("action-playlist-clear"));
    ui.playlist_rename_action->setObjectName(QStringLiteral("action-playlist-rename"));
    ui.playlist_delete_action->setObjectName(QStringLiteral("action-playlist-delete"));
    ui.list_button->setMenu(list_menu);
    statusBar()->addPermanentWidget(ui.list_button);
    auto* status_separator = new QFrame(statusBar());
    status_separator->setFrameShape(QFrame::VLine);
    status_separator->setFrameShadow(QFrame::Sunken);
    statusBar()->addPermanentWidget(status_separator);

    ui.repeat_action = new QAction(
        themedIcon(QStringLiteral("media-playlist-repeat"), style(), QStyle::SP_BrowserReload),
        QStringLiteral("Repeat"), this);
    ui.repeat_action->setObjectName(QStringLiteral("repeat-button"));
    ui.repeat_action->setCheckable(true);
    auto* repeat_button = new QToolButton(statusBar());
    repeat_button->setAutoRaise(true);
    repeat_button->setDefaultAction(ui.repeat_action);
    statusBar()->addPermanentWidget(repeat_button);
    ui.random_action = new QAction(
        themedIcon(QStringLiteral("media-playlist-shuffle"), style(), QStyle::SP_BrowserReload),
        QStringLiteral("Random"), this);
    ui.random_action->setObjectName(QStringLiteral("random-button"));
    ui.random_action->setCheckable(true);
    auto* random_button = new QToolButton(statusBar());
    random_button->setAutoRaise(true);
    random_button->setDefaultAction(ui.random_action);
    statusBar()->addPermanentWidget(random_button);
    ui.single_button = new QToolButton(statusBar());
    ui.single_button->setObjectName(QStringLiteral("single-button"));
    ui.single_button->setText(QStringLiteral("1"));
    ui.single_button->setCheckable(true);
    ui.single_button->setAutoRaise(true);
    ui.single_cycle_action = new QAction(QStringLiteral("Cycle single mode"), this);
    ui.single_cycle_action->setObjectName(QStringLiteral("action-cycle-single-mode"));
    ui.single_cycle_action->setCheckable(true);
    ui.single_button->setDefaultAction(ui.single_cycle_action);
    statusBar()->addPermanentWidget(ui.single_button);
    ui.consume_button = new QToolButton(statusBar());
    ui.consume_button->setObjectName(QStringLiteral("consume-button"));
    ui.consume_button->setText(QStringLiteral("C"));
    ui.consume_button->setCheckable(true);
    ui.consume_button->setAutoRaise(true);
    ui.consume_cycle_action = new QAction(QStringLiteral("Cycle consume mode"), this);
    ui.consume_cycle_action->setObjectName(QStringLiteral("action-cycle-consume-mode"));
    ui.consume_cycle_action->setCheckable(true);
    ui.consume_button->setDefaultAction(ui.consume_cycle_action);
    statusBar()->addPermanentWidget(ui.consume_button);
    ui.replay_gain_button = new QToolButton(statusBar());
    ui.replay_gain_button->setObjectName(QStringLiteral("replaygain-button"));
    ui.replay_gain_button->setIcon(
        themedIcon(QStringLiteral("view-media-equalizer"), style(), QStyle::SP_MediaVolume));
    ui.replay_gain_button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    ui.replay_gain_button->setAutoRaise(true);
    ui.replay_gain_button->setPopupMode(QToolButton::InstantPopup);
    ui.replay_gain_menu = new QMenu(ui.replay_gain_button);
    ui.replay_gain_menu->setObjectName(QStringLiteral("replaygain-popup"));
    ui.replay_gain_group = new QActionGroup(ui.replay_gain_menu);
    ui.replay_gain_group->setExclusive(true);
    const std::array replay_gain_modes{
        std::pair{QStringLiteral("Off"), QStringLiteral("off")},
        std::pair{QStringLiteral("Track"), QStringLiteral("track")},
        std::pair{QStringLiteral("Album"), QStringLiteral("album")},
        std::pair{QStringLiteral("Automatic"), QStringLiteral("auto")},
    };
    for (const auto& [label, value] : replay_gain_modes) {
        auto* action = ui.replay_gain_menu->addAction(label);
        action->setObjectName(QStringLiteral("action-replaygain-%1").arg(value));
        action->setCheckable(true);
        action->setData(value);
        ui.replay_gain_group->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, value] { implementation_->controller->setReplayGainMode(value); });
    }
    ui.replay_gain_button->setMenu(ui.replay_gain_menu);
    statusBar()->addPermanentWidget(ui.replay_gain_button);
    ui.output_button = new QToolButton(statusBar());
    ui.output_button->setObjectName(QStringLiteral("output-button"));
    ui.output_button->setIcon(
        themedIcon(QStringLiteral("audio-speakers"), style(), QStyle::SP_ComputerIcon));
    ui.output_button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    ui.output_button->setAutoRaise(true);
    ui.output_button->setPopupMode(QToolButton::InstantPopup);
    ui.output_menu = new QMenu(ui.output_button);
    ui.output_menu->setObjectName(QStringLiteral("output-popup"));
    ui.output_button->setMenu(ui.output_menu);
    statusBar()->addPermanentWidget(ui.output_button);
    statusBar()->addPermanentWidget(
        makeChromeEdgeSpacer(statusBar(), QStringLiteral("status-right-edge-padding")));

    ui.toast = new QLabel(this);
    ui.toast->setObjectName(QStringLiteral("notification-toast"));
    ui.toast->setFrameStyle(QFrame::StyledPanel | QFrame::Raised);
    ui.toast->setBackgroundRole(QPalette::ToolTipBase);
    ui.toast->setForegroundRole(QPalette::ToolTipText);
    ui.toast->setAutoFillBackground(true);
    ui.toast->setMargin(8);
    ui.toast->setWordWrap(true);
    ui.toast->hide();
    ui.toast_timer = new QTimer(this);
    ui.toast_timer->setSingleShot(true);
    ui.toast_timer->setInterval(5'000);
    connect(ui.toast_timer, &QTimer::timeout, ui.toast, &QWidget::hide);

    connect(ui.previous_action, &QAction::triggered, ui.controller,
            &quick::MpdProbeController::previous);
    connect(ui.play_pause_action, &QAction::triggered, ui.controller,
            &quick::MpdProbeController::playPause);
    connect(ui.stop_action, &QAction::triggered, ui.controller, &quick::MpdProbeController::stop);
    connect(ui.next_action, &QAction::triggered, ui.controller, &quick::MpdProbeController::next);
    connect(ui.repeat_action, &QAction::triggered, ui.controller,
            &quick::MpdProbeController::setRepeatEnabled);
    connect(ui.random_action, &QAction::triggered, ui.controller,
            &quick::MpdProbeController::setRandomEnabled);
    connect(ui.single_cycle_action, &QAction::triggered, this, [this] {
        const auto mode = implementation_->controller->singleMode();
        implementation_->controller->setSingleMode(mode < 0 || mode >= 2 ? 0 : mode + 1);
    });
    connect(ui.consume_cycle_action, &QAction::triggered, this, [this] {
        const auto mode = implementation_->controller->consumeMode();
        implementation_->controller->setConsumeMode(mode < 0 || mode >= 2 ? 0 : mode + 1);
    });
    connect(ui.seek, &QSlider::sliderPressed, this, [this] { implementation_->seeking = true; });
    connect(ui.seek, &QSlider::sliderReleased, this, [this] {
        auto& state = *implementation_;
        state.seeking = false;
        state.controller->seekTo(state.seek->value());
    });
    connect(ui.volume, &QSlider::sliderPressed, this,
            [this] { implementation_->changing_volume = true; });
    connect(ui.volume, &QSlider::sliderReleased, this, [this] {
        auto& state = *implementation_;
        state.changing_volume = false;
        state.controller->setVolume(state.volume->value());
    });

    connect(ui.activate_action, &QAction::triggered, this, &MainWindow::activateCurrentSelection);
    connect(ui.add_action, &QAction::triggered, this, [this] { addCurrentSelection(false); });
    connect(ui.add_next_action, &QAction::triggered, this, [this] { addCurrentSelection(true); });
    connect(ui.remove_action, &QAction::triggered, this, [this] {
        auto& state = *implementation_;
        auto* page = state.tabs->currentWidget();
        const auto playlist_name =
            page != nullptr ? page->property("trackknife-stored-playlist-name").toString()
                            : QString{};
        if (page != nullptr && page->property("trackknife-local-list-tab").toBool()) {
            removeLocalSelection();
        } else if (!playlist_name.isEmpty()) {
            state.controller->removeStoredPlaylistItems(playlist_name,
                                                        selectedRows(activeLibraryTabView()));
        } else {
            state.controller->removeQueueItems(selectedRows(state.queue_view));
        }
    });
    connect(ui.move_up_action, &QAction::triggered, this, [this] {
        auto& state = *implementation_;
        auto* page = state.tabs->currentWidget();
        const auto playlist_name =
            page != nullptr ? page->property("trackknife-stored-playlist-name").toString()
                            : QString{};
        if (page != nullptr && page->property("trackknife-local-list-tab").toBool()) {
            moveLocalSelection(-1);
            return;
        }
        auto* view = playlist_name.isEmpty() ? state.queue_view : activeLibraryTabView();
        const auto row = view != nullptr ? view->currentIndex().row() : -1;
        if (playlist_name.isEmpty()) {
            state.controller->moveQueueItem(row, row - 1);
        } else {
            state.controller->moveStoredPlaylistItem(playlist_name, row, row - 1);
        }
    });
    connect(ui.move_down_action, &QAction::triggered, this, [this] {
        auto& state = *implementation_;
        auto* page = state.tabs->currentWidget();
        const auto playlist_name =
            page != nullptr ? page->property("trackknife-stored-playlist-name").toString()
                            : QString{};
        if (page != nullptr && page->property("trackknife-local-list-tab").toBool()) {
            moveLocalSelection(1);
            return;
        }
        auto* view = playlist_name.isEmpty() ? state.queue_view : activeLibraryTabView();
        const auto row = view != nullptr ? view->currentIndex().row() : -1;
        if (playlist_name.isEmpty()) {
            state.controller->moveQueueItem(row, row + 1);
        } else {
            state.controller->moveStoredPlaylistItem(playlist_name, row, row + 1);
        }
    });
    connect(ui.crop_action, &QAction::triggered, this, [this] {
        auto* page = implementation_->tabs->currentWidget();
        if (page != nullptr && page->property("trackknife-local-list-tab").toBool()) {
            cropLocalSelection();
        } else {
            implementation_->controller->cropQueueToItems(
                selectedRows(implementation_->queue_view));
        }
    });
    connect(ui.clear_action, &QAction::triggered, this, [this] {
        auto* page = implementation_->tabs->currentWidget();
        if (page != nullptr && page->property("trackknife-local-list-tab").toBool()) {
            clearLocalList();
        } else {
            implementation_->controller->clearQueue();
        }
    });
    const auto active_playlist_name = [this] {
        auto* page = implementation_->tabs->currentWidget();
        return page != nullptr ? page->property("trackknife-stored-playlist-name").toString()
                               : QString{};
    };
    connect(ui.playlist_load_action, &QAction::triggered, this, [this, active_playlist_name] {
        implementation_->controller->loadStoredPlaylistIntoQueue(active_playlist_name());
    });
    connect(ui.playlist_clear_action, &QAction::triggered, this, [this, active_playlist_name] {
        const auto name = active_playlist_name();
        if (!name.isEmpty() &&
            QMessageBox::question(this, QStringLiteral("Clear server playlist"),
                                  QStringLiteral("Remove every track from “%1”?").arg(name)) ==
                QMessageBox::Yes) {
            implementation_->controller->clearStoredPlaylist(name);
        }
    });
    connect(ui.playlist_rename_action, &QAction::triggered, this, [this, active_playlist_name] {
        const auto from = active_playlist_name();
        bool accepted = false;
        const auto to =
            QInputDialog::getText(this, QStringLiteral("Rename server playlist"),
                                  QStringLiteral("Name:"), QLineEdit::Normal, from, &accepted);
        if (accepted && !to.trimmed().isEmpty() && to != from) {
            implementation_->controller->renameStoredPlaylist(from, to);
        }
    });
    connect(ui.playlist_delete_action, &QAction::triggered, this, [this, active_playlist_name] {
        const auto name = active_playlist_name();
        if (!name.isEmpty() &&
            QMessageBox::question(this, QStringLiteral("Delete server playlist"),
                                  QStringLiteral("Delete “%1” from the server?").arg(name)) ==
                QMessageBox::Yes) {
            implementation_->controller->deleteStoredPlaylist(name);
        }
    });
    connect(ui.browser_back, &QPushButton::clicked, ui.controller,
            &quick::MpdProbeController::browseParentDirectory);
    connect(ui.queue_view, &QTableView::doubleClicked, this, [this](const QModelIndex& index) {
        implementation_->controller->playQueueItem(index.row());
    });
    static_cast<QueueTableView*>(ui.queue_view)->setActivateCallback([this](const QModelIndex&) {
        implementation_->activate_action->trigger();
    });
    const auto activate_library_index = [this](const QModelIndex&) {
        implementation_->activate_action->trigger();
    };
    connect(ui.library_view, &QTableView::doubleClicked, this, activate_library_index);
    static_cast<QueueTableView*>(ui.library_view)->setActivateCallback(activate_library_index);
    auto* add_next_shortcut =
        new QShortcut(QKeySequence(QStringLiteral("Shift+Return")), ui.library_view);
    connect(add_next_shortcut, &QShortcut::activated, this,
            [this] { implementation_->add_next_action->trigger(); });
    connect(ui.queue_view->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex&, const QModelIndex&) { refreshSelectionDetails(); });
    connect(ui.queue_view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this](const QItemSelection&, const QItemSelection&) { refreshUi(); });
    connect(ui.scratch_view->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex&, const QModelIndex&) { refreshSelectionDetails(); });
    connect(ui.search_view->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex&, const QModelIndex&) { refreshSelectionDetails(); });
    connect(ui.search_view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this](const QItemSelection&, const QItemSelection&) { refreshUi(); });
    connect(ui.library_view->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex&, const QModelIndex&) { refreshSelectionDetails(); });
    connect(ui.library_view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this](const QItemSelection&, const QItemSelection&) { refreshUi(); });
    connect(ui.library_view, &QTableView::clicked, this,
            [this](const QModelIndex&) { refreshUi(); });
    connect(ui.tabs, &QTabWidget::currentChanged, this, [this](const int) {
        refreshSelectionDetails();
        refreshUi();
    });
    connect(ui.tabs->tabBar(), &QTabBar::tabMoved, this,
            [this](const int, const int) { persistLocalTabs(); });

    connect(ui.controller, &quick::MpdProbeController::stateChanged, this, &MainWindow::refreshUi);
    connect(ui.controller, &quick::MpdProbeController::notificationRequested, this,
            &MainWindow::showToast);
    connect(ui.controller, &quick::MpdProbeController::artworkLoaded, this,
            [this](const QString& uri, const QByteArray& bytes) {
                auto& state = *implementation_;
                state.current_artwork_uri = uri;
                if (auto* cached = state.artwork_cache.object(uri); cached != nullptr) {
                    state.cover_art->setPixmap(cached->scaled(
                        state.cover_art->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
                    return;
                }
                state.cover_art->setPixmap(
                    themedIcon(QStringLiteral("media-optical-audio"), style(), QStyle::SP_FileIcon)
                        .pixmap(38, 38));
                if (uri.isEmpty() || bytes.isEmpty()) {
                    return;
                }
                auto* watcher = new QFutureWatcher<QImage>(this);
                connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, uri] {
                    const auto image = watcher->result();
                    watcher->deleteLater();
                    if (image.isNull()) {
                        return;
                    }
                    auto pixmap = QPixmap::fromImage(image);
                    const auto cost = std::max(
                        1, static_cast<int>(std::min<qsizetype>(image.sizeInBytes() / 1024,
                                                                std::numeric_limits<int>::max())));
                    implementation_->artwork_cache.insert(uri, new QPixmap(pixmap), cost);
                    if (implementation_->current_artwork_uri == uri) {
                        implementation_->cover_art->setPixmap(
                            pixmap.scaled(implementation_->cover_art->size(), Qt::KeepAspectRatio,
                                          Qt::SmoothTransformation));
                    }
                });
                watcher->setFuture(QtConcurrent::run([bytes] { return QImage::fromData(bytes); }));
            });
    connect(ui.controller, &quick::MpdProbeController::searchFinished, this,
            &MainWindow::finishSearch);
    connect(ui.controller, &quick::MpdProbeController::storedPlaylistLoaded, this,
            &MainWindow::openStoredPlaylistTab);
    connect(ui.controller, &quick::MpdProbeController::storedPlaylistRenamed, this,
            [this](const QString& from, const QString& to) {
                auto& state = *implementation_;
                for (int index = 0; index < state.tabs->count(); ++index) {
                    auto* page = state.tabs->widget(index);
                    if (page != nullptr &&
                        page->property("trackknife-stored-playlist-name").toString() == from) {
                        page->setProperty("trackknife-stored-playlist-name", to);
                        state.tabs->setTabText(index, to);
                        state.tabs->setTabToolTip(index,
                                                  QStringLiteral("Server playlist: %1").arg(to));
                        break;
                    }
                }
                refreshUi();
            });
    connect(ui.controller, &quick::MpdProbeController::storedPlaylistDeleted, this,
            [this](const QString& name) {
                auto& state = *implementation_;
                for (int index = 0; index < state.tabs->count(); ++index) {
                    auto* page = state.tabs->widget(index);
                    if (page != nullptr &&
                        page->property("trackknife-stored-playlist-name").toString() == name) {
                        state.tabs->removeTab(index);
                        page->deleteLater();
                        break;
                    }
                }
                refreshUi();
            });
    connect(ui.controller, &quick::MpdProbeController::tagListLoaded, this,
            [this](const QString& tag, const QStringList& values) {
                auto& state = *implementation_;
                state.tag_model->clear();
                state.tag_model->setHorizontalHeaderLabels(
                    {tag == QStringLiteral("AlbumArtist") ? QStringLiteral("Artist") : tag});
                for (const auto& value : values) {
                    auto* item = new QStandardItem(value);
                    item->setEditable(false);
                    state.tag_model->appendRow(item);
                }
                if (state.library_view->model() == state.tag_model) {
                    state.library_status->setText(state.controller->browserStatus());
                }
            });
    connect(ui.controller, &quick::MpdProbeController::storedPlaylistListLoaded, this,
            [this](const QStringList& names) {
                auto& state = *implementation_;
                if (state.library_view->property("trackknife-library-index-kind").toString() !=
                    QStringLiteral("playlist")) {
                    return;
                }
                state.tag_model->clear();
                state.tag_model->setHorizontalHeaderLabels({QStringLiteral("Playlist")});
                for (const auto& name : names) {
                    auto* item = new QStandardItem(name);
                    item->setEditable(false);
                    state.tag_model->appendRow(item);
                }
                state.library_status->setText(state.controller->browserStatus());
            });

    auto* server_menu = menuBar()->addMenu(QStringLiteral("&Server"));
    server_menu->addAction(ui.connect_action);
    server_menu->addAction(ui.disconnect_action);
    ui.profile_menu = server_menu->addMenu(QStringLiteral("Profiles"));
    ui.profile_menu->setObjectName(QStringLiteral("menu-connection-profiles"));
    connect(ui.profile_menu, &QMenu::aboutToShow, this, [this] {
        auto& state = *implementation_;
        state.profile_menu->clear();
        if (!state.persistence_ready) {
            auto* unavailable = state.profile_menu->addAction(QStringLiteral("No saved profiles"));
            unavailable->setEnabled(false);
            return;
        }
        if (state.profiles.empty()) {
            auto* unavailable = state.profile_menu->addAction(QStringLiteral("No saved profiles"));
            unavailable->setEnabled(false);
            return;
        }
        for (const auto& profile : state.profiles) {
            auto* action = state.profile_menu->addAction(displayText(profile.name));
            action->setCheckable(true);
            action->setChecked(state.controller->profileId() ==
                               QString::fromStdString(profile.id.to_string()));
            action->setToolTip(
                QStringLiteral("%1:%2").arg(displayText(profile.host)).arg(profile.port));
            connect(action, &QAction::triggered, this, [this, profile] {
                const auto root =
                    profile.local_music_root
                        ? QFile::decodeName(
                              QByteArray{profile.local_music_root->data(),
                                         static_cast<qsizetype>(profile.local_music_root->size())})
                        : QString{};
                implementation_->controller->probeProfile(
                    QString::fromStdString(profile.id.to_string()), displayText(profile.host),
                    static_cast<int>(profile.port), QString{}, root);
            });
        }
    });
    server_menu->addSeparator();
    ui.save_queue_playlist_action =
        server_menu->addAction(QStringLiteral("Save live queue as playlist…"));
    ui.save_queue_playlist_action->setObjectName(QStringLiteral("action-playlist-save-queue"));
    connect(ui.save_queue_playlist_action, &QAction::triggered, this, [this] {
        bool accepted = false;
        const auto name =
            QInputDialog::getText(this, QStringLiteral("Save server playlist"),
                                  QStringLiteral("Name:"), QLineEdit::Normal, QString{}, &accepted);
        if (accepted && !name.trimmed().isEmpty()) {
            implementation_->controller->saveQueueAsPlaylist(name);
        }
    });
    auto* workspace_menu = menuBar()->addMenu(QStringLiteral("&Workspace"));
    ui.new_scratch_action = workspace_menu->addAction(QStringLiteral("New scratch list"));
    ui.new_scratch_action->setObjectName(QStringLiteral("action-new-scratch"));
    ui.new_scratch_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+T")));
    connect(ui.new_scratch_action, &QAction::triggered, this, &MainWindow::createScratchTab);
    ui.new_named_list_action = workspace_menu->addAction(QStringLiteral("New named list…"));
    ui.new_named_list_action->setObjectName(QStringLiteral("action-new-named-list"));
    ui.new_named_list_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+T")));
    connect(ui.new_named_list_action, &QAction::triggered, this, &MainWindow::createNamedList);
    ui.duplicate_tab_action = workspace_menu->addAction(QStringLiteral("Duplicate tab"));
    ui.duplicate_tab_action->setObjectName(QStringLiteral("action-duplicate-tab"));
    ui.duplicate_tab_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+D")));
    connect(ui.duplicate_tab_action, &QAction::triggered, this, &MainWindow::duplicateCurrentTab);
    ui.pin_tab_action = workspace_menu->addAction(QStringLiteral("Pin tab"));
    ui.pin_tab_action->setObjectName(QStringLiteral("action-pin-tab"));
    ui.pin_tab_action->setCheckable(true);
    ui.pin_tab_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+P")));
    connect(ui.pin_tab_action, &QAction::triggered, this, &MainWindow::toggleCurrentTabPinned);
    ui.save_working_list_action = workspace_menu->addAction(QStringLiteral("Save working list"));
    ui.save_working_list_action->setObjectName(QStringLiteral("action-save-working-list"));
    ui.save_working_list_action->setShortcut(QKeySequence::Save);
    connect(ui.save_working_list_action, &QAction::triggered, this,
            &MainWindow::saveCurrentWorkingList);
    ui.rename_tab_action = workspace_menu->addAction(QStringLiteral("Rename tab…"));
    ui.rename_tab_action->setObjectName(QStringLiteral("action-rename-tab"));
    ui.rename_tab_action->setShortcut(QKeySequence(Qt::Key_F2));
    connect(ui.rename_tab_action, &QAction::triggered, this, &MainWindow::renameCurrentTab);
    ui.close_tab_action = workspace_menu->addAction(QStringLiteral("Close tab"));
    ui.close_tab_action->setObjectName(QStringLiteral("action-close-tab"));
    ui.close_tab_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+W")));
    connect(ui.close_tab_action, &QAction::triggered, this, &MainWindow::closeCurrentTab);
    auto* previous_tab = workspace_menu->addAction(QStringLiteral("Previous tab"));
    previous_tab->setObjectName(QStringLiteral("action-previous-tab"));
    previous_tab->setShortcut(QKeySequence(QStringLiteral("Ctrl+PgUp")));
    connect(previous_tab, &QAction::triggered, this, [this] {
        auto& state = *implementation_;
        if (state.tabs->count() < 1) {
            return;
        }
        state.center_stack->setCurrentWidget(state.tabs);
        state.tabs->setCurrentIndex((state.tabs->currentIndex() + state.tabs->count() - 1) %
                                    state.tabs->count());
        if (auto* view = qobject_cast<QTableView*>(state.tabs->currentWidget())) {
            view->setFocus(Qt::ShortcutFocusReason);
        }
    });
    auto* next_tab = workspace_menu->addAction(QStringLiteral("Next tab"));
    next_tab->setObjectName(QStringLiteral("action-next-tab"));
    next_tab->setShortcut(QKeySequence(QStringLiteral("Ctrl+PgDown")));
    connect(next_tab, &QAction::triggered, this, [this] {
        auto& state = *implementation_;
        if (state.tabs->count() < 1) {
            return;
        }
        state.center_stack->setCurrentWidget(state.tabs);
        state.tabs->setCurrentIndex((state.tabs->currentIndex() + 1) % state.tabs->count());
        auto* page = state.tabs->currentWidget();
        auto* view = qobject_cast<QTableView*>(page);
        if (view == nullptr && page != nullptr) {
            view = page->findChild<QTableView*>();
        }
        (view != nullptr ? static_cast<QWidget*>(view) : page)->setFocus(Qt::ShortcutFocusReason);
    });
    ui.move_tab_left_action = workspace_menu->addAction(QStringLiteral("Move tab left"));
    ui.move_tab_left_action->setObjectName(QStringLiteral("action-move-tab-left"));
    ui.move_tab_left_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+PgUp")));
    connect(ui.move_tab_left_action, &QAction::triggered, this, [this] {
        auto& state = *implementation_;
        const auto from = state.tabs->currentIndex();
        if (from > 0) {
            state.tabs->tabBar()->moveTab(from, from - 1);
        }
    });
    ui.move_tab_right_action = workspace_menu->addAction(QStringLiteral("Move tab right"));
    ui.move_tab_right_action->setObjectName(QStringLiteral("action-move-tab-right"));
    ui.move_tab_right_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+PgDown")));
    connect(ui.move_tab_right_action, &QAction::triggered, this, [this] {
        auto& state = *implementation_;
        const auto from = state.tabs->currentIndex();
        if (from >= 0 && from + 1 < state.tabs->count()) {
            state.tabs->tabBar()->moveTab(from, from + 1);
        }
    });
    workspace_menu->addSeparator();
    auto* reset_action = workspace_menu->addAction(QStringLiteral("Reset layout"));
    reset_action->setObjectName(QStringLiteral("action-reset-workspace"));
    connect(reset_action, &QAction::triggered, this, &MainWindow::resetWorkspace);
    workspace_menu->addAction(ui.library_dock->toggleViewAction());
    workspace_menu->addAction(ui.details_dock->toggleViewAction());
    workspace_menu->addAction(ui.jobs_dock->toggleViewAction());
    auto* tools_menu = menuBar()->addMenu(QStringLiteral("&Tools"));
    ui.command_palette_action = tools_menu->addAction(QStringLiteral("Commands and shortcuts…"));
    ui.command_palette_action->setObjectName(QStringLiteral("action-command-palette"));
    ui.command_palette_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+P")));
    connect(ui.command_palette_action, &QAction::triggered, this, [this] {
        if (auto* existing = findChild<CommandPalette*>(QStringLiteral("command-palette"))) {
            existing->show();
            existing->raise();
            existing->activateWindow();
            return;
        }
        auto* palette = new CommandPalette(findChildren<QAction*>(), this);
        palette->setAttribute(Qt::WA_DeleteOnClose);
        palette->show();
    });
    tools_menu->addSeparator();
    auto* sandbox_action = tools_menu->addAction(QStringLiteral("Format Expression Sandbox…"));
    sandbox_action->setObjectName(QStringLiteral("action-format-sandbox"));
    connect(sandbox_action, &QAction::triggered, this, &MainWindow::openFormatSandbox);

    ui.library_dock->toggleViewAction()->setObjectName(QStringLiteral("action-panel-library"));
    ui.details_dock->toggleViewAction()->setObjectName(QStringLiteral("action-panel-selection"));
    ui.jobs_dock->toggleViewAction()->setObjectName(QStringLiteral("action-panel-jobs"));

    ui.track_context_menu = new QMenu(this);
    ui.track_context_menu->setObjectName(QStringLiteral("track-row-context-menu"));
    ui.tab_context_menu = new QMenu(this);
    ui.tab_context_menu->setObjectName(QStringLiteral("track-tab-context-menu"));
    installTrackContextMenu(ui.queue_view);
    installTrackContextMenu(ui.search_view);
    installTrackContextMenu(ui.library_view);
    for (int index = 0; index < ui.tabs->count(); ++index) {
        auto* page = ui.tabs->widget(index);
        auto* view = qobject_cast<QTableView*>(page);
        if (view == nullptr && page != nullptr) {
            view = page->findChild<QTableView*>();
        }
        installTrackContextMenu(view);
    }
    ui.tabs->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui.tabs->tabBar(), &QWidget::customContextMenuRequested, this,
            &MainWindow::showTabContextMenu);

    CommandPalette::restoreShortcuts(findChildren<QAction*>());

    if (ui.scratch_model != nullptr) {
        connect(ui.scratch_model, &qtmodels::PagedTrackModel::pageLoaded, this,
                [this](const qint64 first, const qint64 count, const qint64 microseconds) {
                    implementation_->metrics->setText(
                        QStringLiteral("Paged rows %1–%2 in %3 µs; no model reset")
                            .arg(first + 1)
                            .arg(first + count)
                            .arg(microseconds));
                });
    }
    if (logical_rows > 0) {
        auto* update_timer = new QTimer(this);
        update_timer->setInterval(750);
        connect(update_timer, &QTimer::timeout, this, [this, logical_rows] {
            implementation_->scratch_model->refreshPageContaining(implementation_->update_cursor %
                                                                  logical_rows);
            implementation_->update_cursor += 4093;
        });
        update_timer->start();
    }
    if (!ui.persistence_error.isEmpty()) {
        QTimer::singleShot(0, this, [this] { showToast(implementation_->persistence_error); });
    }
}

void MainWindow::previewSearch() {
    auto& ui = *implementation_;
    if (ui.pending_search_status != nullptr) {
        ui.pending_search_status->setText(QStringLiteral("Search superseded"));
    }
    ui.pending_search_model.clear();
    ui.pending_search_status.clear();
    ui.pending_search_query.clear();

    ui.search_page->show();
    ui.search_page->raise();
    positionLiveSearchSurface();
    const auto query = ui.search->text().trimmed();
    if (query.size() < 2) {
        ui.live_search_model->replaceTracks({});
        syncLiveSearchView();
    }
    ui.controller->searchLibrary(query);
    refreshUi();
}

void MainWindow::commitSearch() {
    auto& ui = *implementation_;
    ui.search_delay->stop();
    const auto query = ui.search->text().trimmed();
    if (query.size() < 2) {
        previewSearch();
        return;
    }

    if (ui.pending_search_status != nullptr) {
        ui.pending_search_status->setText(QStringLiteral("Search superseded"));
    }
    ui.pending_search_model.clear();
    ui.pending_search_status.clear();
    ui.pending_search_query.clear();

    auto* page = new QWidget(ui.tabs);
    page->setProperty("trackknife-search-tab", true);
    page->setProperty("trackknife-search-query", query);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* model = new quick::MpdQueueModel(page);
    auto* view = makeTrackView(model, page);
    view->setObjectName(QStringLiteral("track-view-search-result"));
    layout->addWidget(view, 1);
    auto* status = new QLabel(QStringLiteral("Searching…"), page);
    status->setObjectName(QStringLiteral("search-result-status"));
    status->setContentsMargins(4, 2, 4, 2);
    layout->addWidget(status);

    auto tab_title = QStringLiteral("Search: %1").arg(query);
    if (tab_title.size() > 36) {
        tab_title = tab_title.left(33) + QChar{0x2026};
    }
    const auto index = ui.tabs->addTab(page, tab_title);
    ui.tabs->setTabToolTip(index, query);
    auto* close = new QToolButton(ui.tabs->tabBar());
    close->setObjectName(QStringLiteral("search-tab-close"));
    close->setAutoRaise(true);
    close->setIcon(
        themedIcon(QStringLiteral("window-close"), style(), QStyle::SP_TitleBarCloseButton));
    close->setToolTip(QStringLiteral("Close search"));
    ui.tabs->tabBar()->setTabButton(index, QTabBar::RightSide, close);
    connect(close, &QToolButton::clicked, page, [this, page, model, status] {
        auto& state = *implementation_;
        if (state.pending_search_model == model || state.pending_search_status == status) {
            state.pending_search_model.clear();
            state.pending_search_status.clear();
            state.pending_search_query.clear();
        }
        const auto tab_index = state.tabs->indexOf(page);
        if (tab_index >= 0) {
            state.tabs->removeTab(tab_index);
        }
        page->deleteLater();
        refreshUi();
    });

    installSearchView(view);
    ui.pending_search_model = model;
    ui.pending_search_status = status;
    ui.pending_search_query = query;
    ui.search_page->hide();
    ui.center_stack->setCurrentWidget(ui.tabs);
    ui.tabs->setCurrentWidget(page);
    ui.controller->searchLibrary(query);
    refreshUi();
}

void MainWindow::finishSearch(const QString& query, const bool success) {
    auto& ui = *implementation_;
    const auto current_live_query = ui.search->text().trimmed();
    std::vector<mpd::Track> tracks;
    if (success) {
        if (const auto* source =
                qobject_cast<const quick::MpdQueueModel*>(ui.controller->libraryModel())) {
            tracks = source->tracksSnapshot();
        }
        if (query == current_live_query) {
            ui.live_search_model->replaceSearchResults(ui.controller->libraryAlbumsSnapshot(),
                                                       tracks);
            syncLiveSearchView();
        }
    } else if (query == current_live_query) {
        ui.live_search_model->replaceTracks({});
        syncLiveSearchView();
    }

    if (ui.pending_search_model != nullptr && ui.pending_search_status != nullptr &&
        ui.pending_search_query == query) {
        if (success) {
            ui.pending_search_model->replaceTracks(tracks);
            ui.pending_search_status->setText(ui.controller->libraryStatus());
        } else {
            ui.pending_search_status->setText(QStringLiteral("Search did not complete"));
        }
        ui.pending_search_model.clear();
        ui.pending_search_status.clear();
        ui.pending_search_query.clear();
    }
    if (success) {
        for (int index = 0; index < ui.tabs->count(); ++index) {
            auto* page = ui.tabs->widget(index);
            if (page == nullptr || page->property("trackknife-search-query").toString() != query) {
                continue;
            }
            if (auto* model = page->findChild<quick::MpdQueueModel*>()) {
                model->replaceTracks(tracks);
            }
            if (auto* status = page->findChild<QLabel*>(QStringLiteral("search-result-status"))) {
                status->setText(ui.controller->libraryStatus());
            }
        }
    }
    refreshUi();
}

void MainWindow::createScratchTab() {
    auto& ui = *implementation_;
    int number = 1;
    while (true) {
        const auto candidate = QStringLiteral("Scratch %1").arg(number);
        bool used = false;
        for (int index = 0; index < ui.tabs->count(); ++index) {
            if (ui.tabs->widget(index)->property("trackknife-local-list-tab").toBool() &&
                ui.tabs->tabText(index) == candidate) {
                used = true;
                break;
            }
        }
        if (!used) {
            break;
        }
        ++number;
    }

    persistence::ListDocument document{
        .id = core::StableId::random(),
        .kind = persistence::ListKind::scratch,
        .name = QStringLiteral("Scratch %1").arg(number).toUtf8().toStdString(),
        .pinned = false,
        .dirty = false,
        .items = {},
    };
    addLocalListTab(std::move(document), true);
    persistLocalTabs();
    if (ui.rename_tab_action != nullptr) {
        refreshUi();
    }
}

void MainWindow::addLocalListTab(persistence::ListDocument document, const bool select) {
    auto& ui = *implementation_;
    const auto id = QString::fromStdString(document.id.to_string());
    const auto name = displayText(document.name);
    std::vector<mpd::Track> tracks;
    tracks.reserve(document.items.size());
    for (const auto& item : document.items) {
        tracks.push_back(trackFromListItem(item));
    }

    auto* model = new quick::MpdQueueModel(ui.tabs);
    model->replaceTracks(std::move(tracks));
    auto* view = makeTrackView(model, ui.tabs, true);
    view->setProperty("trackknife-local-list-tab", true);
    view->setProperty("trackknife-list-document-id", id);
    view->setProperty("trackknife-list-name", name);
    view->setProperty("trackknife-list-dirty", document.dirty);
    view->setProperty("trackknife-list-pinned", document.pinned);
    view->setObjectName(ui.scratch_view == nullptr
                            ? QStringLiteral("track-view-library")
                            : QStringLiteral("track-view-working-%1").arg(id.left(8)));
    view->setAccessibleName(QStringLiteral("%1 track list").arg(name));
    view->setDragEnabled(true);
    view->setAcceptDrops(true);
    view->setDropIndicatorShown(true);
    view->setDragDropOverwriteMode(false);
    view->setDragDropMode(QAbstractItemView::DragDrop);
    view->setDefaultDropAction(Qt::MoveAction);
    if (const auto state = ui.view_presets.value(QStringLiteral("local:%1").arg(id));
        !state.isEmpty()) {
        static_cast<void>(view->horizontalHeader()->restoreState(state));
    }
    auto* queue_view = static_cast<QueueTableView*>(view);
    queue_view->setActivateCallback(
        [this](const QModelIndex&) { implementation_->activate_action->trigger(); });
    queue_view->setReorderCallback([this, id](const QVariantList& rows, const int insertion_row) {
        reorderLocalRows(id, rows, insertion_row);
    });
    queue_view->setExternalDropCallback(
        [this, id](QAbstractItemView* source, const QVariantList& rows, const int insertion_row,
                   const Qt::DropAction action) {
            auto* table = qobject_cast<QTableView*>(source);
            return table != nullptr &&
                   transferRows(table, rows, id, action == Qt::MoveAction, insertion_row);
        });
    if (ui.scratch_view == nullptr) {
        ui.scratch_view = view;
    }
    const auto index =
        ui.tabs->addTab(view, name + (document.dirty ? QStringLiteral(" *") : QString{}));
    auto* close = new QToolButton(ui.tabs->tabBar());
    close->setObjectName(QStringLiteral("working-list-tab-close"));
    close->setAutoRaise(true);
    close->setIcon(
        themedIcon(QStringLiteral("window-close"), style(), QStyle::SP_TitleBarCloseButton));
    close->setToolTip(QStringLiteral("Close local list"));
    close->setVisible(!document.pinned);
    ui.tabs->tabBar()->setTabButton(index, QTabBar::RightSide, close);
    connect(close, &QToolButton::clicked, view, [this, view] {
        implementation_->tabs->setCurrentWidget(view);
        closeCurrentTab();
    });
    connect(view->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex&, const QModelIndex&) { refreshSelectionDetails(); });
    connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this](const QItemSelection&, const QItemSelection&) { refreshUi(); });
    installTrackContextMenu(view);
    ui.local_documents.insert(id, std::move(document));
    refreshLocalList(view, view->property("trackknife-list-dirty").toBool());
    if (select) {
        ui.center_stack->setCurrentWidget(ui.tabs);
        ui.tabs->setCurrentWidget(view);
        view->setFocus(Qt::ShortcutFocusReason);
    }
}

void MainWindow::createNamedList() {
    bool accepted = false;
    const auto name =
        QInputDialog::getText(this, QStringLiteral("New working list"), QStringLiteral("Name:"),
                              QLineEdit::Normal, QString{}, &accepted)
            .trimmed();
    if (!accepted || name.isEmpty()) {
        return;
    }
    addLocalListTab(
        persistence::ListDocument{
            .id = core::StableId::random(),
            .kind = persistence::ListKind::saved,
            .name = utf8Bytes(name),
            .pinned = false,
            .dirty = false,
            .items = {},
        },
        true);
    persistLocalTabs();
    refreshUi();
}

void MainWindow::duplicateCurrentTab() {
    auto& ui = *implementation_;
    auto* page = ui.tabs->currentWidget();
    if (page == nullptr || !page->property("trackknife-local-list-tab").toBool()) {
        return;
    }
    const auto source =
        ui.local_documents.find(page->property("trackknife-list-document-id").toString());
    if (source == ui.local_documents.end()) {
        return;
    }
    auto duplicate = *source;
    duplicate.id = core::StableId::random();
    duplicate.name = utf8Bytes(QStringLiteral("%1 copy").arg(displayText(source->name)));
    duplicate.pinned = false;
    duplicate.dirty = true;
    addLocalListTab(std::move(duplicate), true);
    persistLocalTabs();
    refreshUi();
}

void MainWindow::toggleCurrentTabPinned() {
    auto& ui = *implementation_;
    auto* page = ui.tabs->currentWidget();
    if (page == nullptr || !page->property("trackknife-local-list-tab").toBool()) {
        return;
    }
    const auto id = page->property("trackknife-list-document-id").toString();
    auto found = ui.local_documents.find(id);
    if (found == ui.local_documents.end()) {
        return;
    }
    found->pinned = !found->pinned;
    page->setProperty("trackknife-list-pinned", found->pinned);
    refreshLocalList(page, found->dirty);
    persistLocalTabs();
    refreshUi();
}

void MainWindow::saveCurrentWorkingList() {
    auto& ui = *implementation_;
    auto* page = ui.tabs->currentWidget();
    if (page == nullptr || !page->property("trackknife-local-list-tab").toBool()) {
        return;
    }
    const auto id = page->property("trackknife-list-document-id").toString();
    auto found = ui.local_documents.find(id);
    if (found == ui.local_documents.end()) {
        return;
    }
    if (found->kind == persistence::ListKind::scratch) {
        bool accepted = false;
        const auto name =
            QInputDialog::getText(this, QStringLiteral("Save working list"),
                                  QStringLiteral("Name:"), QLineEdit::Normal,
                                  page->property("trackknife-list-name").toString(), &accepted)
                .trimmed();
        if (!accepted || name.isEmpty()) {
            return;
        }
        found->name = utf8Bytes(name);
        found->kind = persistence::ListKind::saved;
        page->setProperty("trackknife-list-name", name);
    }
    refreshLocalList(page, false);
    persistLocalTabs();
    refreshUi();
}

void MainWindow::refreshLocalList(QWidget* page, const bool dirty) {
    auto& ui = *implementation_;
    if (page == nullptr) {
        return;
    }
    const auto id = page->property("trackknife-list-document-id").toString();
    auto found = ui.local_documents.find(id);
    if (found == ui.local_documents.end()) {
        return;
    }
    found->dirty = dirty;
    page->setProperty("trackknife-list-dirty", dirty);
    page->setProperty("trackknife-list-pinned", found->pinned);
    const auto name = displayText(found->name);
    page->setProperty("trackknife-list-name", name);
    auto* view = qobject_cast<QTableView*>(page);
    auto* model = view != nullptr ? qobject_cast<quick::MpdQueueModel*>(view->model()) : nullptr;
    if (model != nullptr) {
        std::vector<mpd::Track> tracks;
        tracks.reserve(found->items.size());
        for (const auto& item : found->items) {
            tracks.push_back(trackFromListItem(item));
        }
        model->replaceTracks(std::move(tracks));
    }
    const auto index = ui.tabs->indexOf(page);
    if (index < 0) {
        return;
    }
    ui.tabs->setTabText(index, name + (dirty ? QStringLiteral(" *") : QString{}));
    const auto kind = found->kind == persistence::ListKind::scratch
                          ? QStringLiteral("Persistent scratch list")
                          : QStringLiteral("Named Trackknife working list");
    ui.tabs->setTabToolTip(index, QStringLiteral("%1%2%3").arg(
                                      kind, found->pinned ? QStringLiteral(" · pinned") : QString{},
                                      dirty ? QStringLiteral(" · modified") : QString{}));
    if (auto* close = ui.tabs->tabBar()->tabButton(index, QTabBar::RightSide)) {
        close->setVisible(!found->pinned);
    }
}

void MainWindow::reorderLocalRows(const QString& document_id, const QVariantList& rows,
                                  const int insertion_row) {
    auto& ui = *implementation_;
    auto found = ui.local_documents.find(document_id);
    if (found == ui.local_documents.end() || rows.isEmpty()) {
        return;
    }
    std::vector<int> ordered;
    ordered.reserve(static_cast<std::size_t>(rows.size()));
    for (const auto& row : rows) {
        const auto value = row.toInt();
        if (value >= 0 && static_cast<std::size_t>(value) < found->items.size()) {
            ordered.push_back(value);
        }
    }
    std::ranges::sort(ordered);
    ordered.erase(std::unique(ordered.begin(), ordered.end()), ordered.end());
    if (ordered.empty()) {
        return;
    }
    std::vector<persistence::ListItem> moving;
    moving.reserve(ordered.size());
    for (const auto row : ordered) {
        moving.push_back(found->items[static_cast<std::size_t>(row)]);
    }
    auto adjusted = std::clamp(insertion_row, 0, static_cast<int>(found->items.size()));
    adjusted -= static_cast<int>(
        std::ranges::count_if(ordered, [adjusted](const int row) { return row < adjusted; }));
    for (auto iterator = ordered.rbegin(); iterator != ordered.rend(); ++iterator) {
        found->items.erase(found->items.begin() + *iterator);
    }
    const auto target =
        found->items.begin() + std::clamp(adjusted, 0, static_cast<int>(found->items.size()));
    found->items.insert(target, moving.begin(), moving.end());
    QWidget* page = nullptr;
    for (int index = 0; index < ui.tabs->count(); ++index) {
        if (ui.tabs->widget(index)->property("trackknife-list-document-id").toString() ==
            document_id) {
            page = ui.tabs->widget(index);
            break;
        }
    }
    refreshLocalList(page, true);
    persistLocalTabs();
    refreshUi();
}

void MainWindow::moveLocalSelection(const int direction) {
    auto& ui = *implementation_;
    auto* view = activeLibraryTabView();
    if (view == nullptr || direction == 0) {
        return;
    }
    const auto rows = selectedRows(view);
    if (rows.isEmpty()) {
        return;
    }
    const auto first = rows.front().toInt();
    const auto last = rows.back().toInt();
    if ((direction < 0 && first <= 0) || (direction > 0 && last + 1 >= view->model()->rowCount())) {
        return;
    }
    const auto insertion = direction < 0 ? first - 1 : last + 2;
    reorderLocalRows(ui.tabs->currentWidget()->property("trackknife-list-document-id").toString(),
                     rows, insertion);
}

void MainWindow::removeLocalSelection() {
    auto& ui = *implementation_;
    auto* page = ui.tabs->currentWidget();
    auto* view = activeLibraryTabView();
    if (page == nullptr || view == nullptr) {
        return;
    }
    auto found = ui.local_documents.find(page->property("trackknife-list-document-id").toString());
    if (found == ui.local_documents.end()) {
        return;
    }
    auto rows = selectedRows(view);
    std::ranges::reverse(rows);
    for (const auto& row : rows) {
        const auto value = row.toInt();
        if (value >= 0 && static_cast<std::size_t>(value) < found->items.size()) {
            found->items.erase(found->items.begin() + value);
        }
    }
    refreshLocalList(page, true);
    persistLocalTabs();
    refreshUi();
}

void MainWindow::cropLocalSelection() {
    auto& ui = *implementation_;
    auto* page = ui.tabs->currentWidget();
    auto* view = activeLibraryTabView();
    if (page == nullptr || view == nullptr) {
        return;
    }
    auto found = ui.local_documents.find(page->property("trackknife-list-document-id").toString());
    if (found == ui.local_documents.end()) {
        return;
    }
    std::vector<persistence::ListItem> kept;
    for (const auto& row : selectedRows(view)) {
        const auto value = row.toInt();
        if (value >= 0 && static_cast<std::size_t>(value) < found->items.size()) {
            kept.push_back(found->items[static_cast<std::size_t>(value)]);
        }
    }
    found->items = std::move(kept);
    refreshLocalList(page, true);
    persistLocalTabs();
    refreshUi();
}

void MainWindow::clearLocalList() {
    auto& ui = *implementation_;
    auto* page = ui.tabs->currentWidget();
    if (page == nullptr) {
        return;
    }
    auto found = ui.local_documents.find(page->property("trackknife-list-document-id").toString());
    if (found == ui.local_documents.end() || found->items.empty()) {
        return;
    }
    found->items.clear();
    refreshLocalList(page, true);
    persistLocalTabs();
    refreshUi();
}

void MainWindow::sortLocalList() {
    auto& ui = *implementation_;
    auto* page = ui.tabs->currentWidget();
    if (page == nullptr) {
        return;
    }
    auto found = ui.local_documents.find(page->property("trackknife-list-document-id").toString());
    if (found == ui.local_documents.end()) {
        return;
    }
    const auto text = [](const persistence::ListItem& item, const std::string_view field) {
        const auto track = trackFromListItem(item);
        const auto value = track.metadata.first(field);
        return value ? QString::fromUtf8(value->data(), static_cast<qsizetype>(value->size()))
                           .toCaseFolded()
                     : QString{};
    };
    const auto number = [&text](const persistence::ListItem& item, const std::string_view field) {
        bool valid = false;
        const auto parsed = text(item, field).section(QLatin1Char('/'), 0, 0).toInt(&valid);
        return valid ? parsed : std::numeric_limits<int>::max();
    };
    std::ranges::stable_sort(found->items, [&text, &number](const auto& left, const auto& right) {
        const auto artist = [](const persistence::ListItem& item, const auto& read) {
            const auto album_artist = read(item, "AlbumArtistSort");
            if (!album_artist.isEmpty()) {
                return album_artist;
            }
            const auto credited = read(item, "AlbumArtist");
            return credited.isEmpty() ? read(item, "Artist") : credited;
        };
        return std::tuple{artist(left, text),    text(left, "Album"), number(left, "Disc"),
                          number(left, "Track"), text(left, "Title"), left.source_reference} <
               std::tuple{artist(right, text),    text(right, "Album"), number(right, "Disc"),
                          number(right, "Track"), text(right, "Title"), right.source_reference};
    });
    refreshLocalList(page, true);
    persistLocalTabs();
    refreshUi();
}

void MainWindow::reverseLocalList() {
    auto& ui = *implementation_;
    auto* page = ui.tabs->currentWidget();
    if (page == nullptr) {
        return;
    }
    auto found = ui.local_documents.find(page->property("trackknife-list-document-id").toString());
    if (found == ui.local_documents.end() || found->items.size() < 2U) {
        return;
    }
    std::ranges::reverse(found->items);
    refreshLocalList(page, true);
    persistLocalTabs();
    refreshUi();
}

void MainWindow::randomizeLocalList() {
    auto& ui = *implementation_;
    auto* page = ui.tabs->currentWidget();
    if (page == nullptr) {
        return;
    }
    auto found = ui.local_documents.find(page->property("trackknife-list-document-id").toString());
    if (found == ui.local_documents.end() || found->items.size() < 2U) {
        return;
    }
    std::random_device entropy;
    std::mt19937 generator{entropy()};
    std::ranges::shuffle(found->items, generator);
    refreshLocalList(page, true);
    persistLocalTabs();
    refreshUi();
}

void MainWindow::deduplicateLocalList() {
    auto& ui = *implementation_;
    auto* page = ui.tabs->currentWidget();
    if (page == nullptr) {
        return;
    }
    auto found = ui.local_documents.find(page->property("trackknife-list-document-id").toString());
    if (found == ui.local_documents.end()) {
        return;
    }
    std::unordered_set<std::string> seen;
    std::erase_if(found->items, [&seen](const persistence::ListItem& item) {
        std::string key;
        key.reserve(item.source_reference.size() + 48U);
        key.push_back(static_cast<char>(item.source));
        if (item.profile_id) {
            key += item.profile_id->to_string();
        }
        key.push_back('\0');
        key += item.source_reference;
        return !seen.insert(std::move(key)).second;
    });
    refreshLocalList(page, true);
    persistLocalTabs();
    refreshUi();
}

bool MainWindow::transferRows(QTableView* source, const QVariantList& rows,
                              const QString& target_document_id, const bool move,
                              const int insertion_row) {
    auto& ui = *implementation_;
    auto target = ui.local_documents.find(target_document_id);
    if (source == nullptr || target == ui.local_documents.end() || rows.isEmpty()) {
        return false;
    }
    const auto source_document_id = source->property("trackknife-list-document-id").toString();
    if (!source_document_id.isEmpty() && source_document_id == target_document_id) {
        reorderLocalRows(target_document_id, rows, insertion_row);
        return true;
    }

    std::vector<persistence::ListItem> copied;
    copied.reserve(static_cast<std::size_t>(rows.size()));
    auto source_document = ui.local_documents.find(source_document_id);
    if (source_document != ui.local_documents.end()) {
        for (const auto& row : rows) {
            const auto value = row.toInt();
            if (value >= 0 && static_cast<std::size_t>(value) < source_document->items.size()) {
                copied.push_back(source_document->items[static_cast<std::size_t>(value)]);
            }
        }
    } else if (auto* model = qobject_cast<quick::MpdQueueModel*>(source->model())) {
        auto profile_id = core::StableId::parse(ui.controller->profileId().toStdString());
        if (!profile_id) {
            showToast(QStringLiteral("Connect a server profile before saving remote tracks"));
            return false;
        }
        const auto tracks = model->tracksSnapshot();
        for (const auto& row : rows) {
            const auto value = row.toInt();
            if (value >= 0 && static_cast<std::size_t>(value) < tracks.size()) {
                copied.push_back(
                    listItemFromTrack(tracks[static_cast<std::size_t>(value)], *profile_id));
            }
        }
    }
    if (copied.empty()) {
        return false;
    }

    const auto insertion = std::clamp(insertion_row, 0, static_cast<int>(target->items.size()));
    target->items.insert(target->items.begin() + insertion, copied.begin(), copied.end());
    QWidget* target_page = nullptr;
    for (int index = 0; index < ui.tabs->count(); ++index) {
        if (ui.tabs->widget(index)->property("trackknife-list-document-id").toString() ==
            target_document_id) {
            target_page = ui.tabs->widget(index);
            break;
        }
    }
    refreshLocalList(target_page, true);

    if (move) {
        if (source_document != ui.local_documents.end()) {
            auto descending = rows;
            std::ranges::reverse(descending);
            for (const auto& row : descending) {
                const auto value = row.toInt();
                if (value >= 0 && static_cast<std::size_t>(value) < source_document->items.size()) {
                    source_document->items.erase(source_document->items.begin() + value);
                }
            }
            refreshLocalList(source, true);
        } else if (source == ui.queue_view) {
            ui.controller->removeQueueItems(rows);
        } else if (auto* page = source->parentWidget(); page != nullptr) {
            const auto playlist = page->property("trackknife-stored-playlist-name").toString();
            if (!playlist.isEmpty()) {
                ui.controller->removeStoredPlaylistItems(playlist, rows);
            }
        }
    }
    persistLocalTabs();
    refreshUi();
    return true;
}

void MainWindow::transferSelectionTo(const QString& target_document_id, const bool move) {
    auto& ui = *implementation_;
    QTableView* source = nullptr;
    if (ui.center_stack->currentWidget() == ui.tabs && ui.tabs->currentWidget() == ui.queue_view) {
        source = ui.queue_view;
    } else {
        source = activeLibraryTabView();
    }
    if (source == nullptr) {
        return;
    }
    auto rows = selectedRows(source);
    if (rows.isEmpty() && source->currentIndex().isValid()) {
        rows.push_back(source->currentIndex().row());
    }
    const auto target = ui.local_documents.find(target_document_id);
    const auto insertion =
        target == ui.local_documents.end() ? 0 : static_cast<int>(target->items.size());
    static_cast<void>(transferRows(source, rows, target_document_id, move, insertion));
}

void MainWindow::rebuildTransferMenus() {
    auto& ui = *implementation_;
    ui.copy_to_menu->clear();
    ui.move_to_menu->clear();
    auto* current = ui.tabs->currentWidget();
    const auto current_id = current != nullptr
                                ? current->property("trackknife-list-document-id").toString()
                                : QString{};
    for (int index = 0; index < ui.tabs->count(); ++index) {
        auto* page = ui.tabs->widget(index);
        const auto id = page->property("trackknife-list-document-id").toString();
        if (id.isEmpty() || id == current_id) {
            continue;
        }
        const auto name = page->property("trackknife-list-name").toString();
        auto* copy = ui.copy_to_menu->addAction(name);
        connect(copy, &QAction::triggered, this, [this, id] { transferSelectionTo(id, false); });
        auto* move = ui.move_to_menu->addAction(name);
        connect(move, &QAction::triggered, this, [this, id] { transferSelectionTo(id, true); });
    }
    if (ui.copy_to_menu->isEmpty()) {
        auto* unavailable = ui.copy_to_menu->addAction(QStringLiteral("No other local list"));
        unavailable->setEnabled(false);
        auto* unavailable_move = ui.move_to_menu->addAction(QStringLiteral("No other local list"));
        unavailable_move->setEnabled(false);
    }
}

void MainWindow::refreshTransport() {
    auto& ui = *implementation_;
    const auto connected = ui.controller->connected();
    const auto command_ready = connected && !ui.controller->commandBusy();
    ui.previous_action->setEnabled(command_ready);
    ui.previous_action->setToolTip(QStringLiteral("Previous"));
    ui.next_action->setEnabled(command_ready);
    ui.next_action->setToolTip(QStringLiteral("Next"));
    ui.play_pause_action->setEnabled(command_ready);
    ui.stop_action->setEnabled(command_ready);
    ui.play_pause_action->setText(ui.controller->playing() ? QStringLiteral("Pause")
                                                           : QStringLiteral("Play"));
    ui.play_pause_action->setIcon(
        ui.controller->playing()
            ? themedIcon(QStringLiteral("media-playback-pause"), style(), QStyle::SP_MediaPause)
            : themedIcon(QStringLiteral("media-playback-start"), style(), QStyle::SP_MediaPlay));
    ui.now_playing->setText(ui.controller->nowPlayingTitle());
    ui.now_playing->setToolTip(ui.controller->nowPlaying());
    ui.now_playing_detail->setText(ui.controller->nowPlayingDetail());
    ui.now_playing_detail->setToolTip(ui.controller->nowPlaying());
    ui.elapsed->setText(formatTime(ui.controller->elapsedMs()));
    ui.duration->setText(formatTime(ui.controller->durationMs()));
    const auto duration =
        std::clamp<qint64>(ui.controller->durationMs(), 0, std::numeric_limits<int>::max());
    ui.seek->setEnabled(command_ready && duration > 0);
    ui.seek->setRange(0, static_cast<int>(duration));
    if (!ui.seeking) {
        const QSignalBlocker blocker{ui.seek};
        ui.seek->setValue(static_cast<int>(
            std::clamp<qint64>(ui.controller->elapsedMs(), 0, std::numeric_limits<int>::max())));
    }
    ui.volume->setEnabled(command_ready && ui.controller->volume() >= 0);
    if (!ui.changing_volume && ui.controller->volume() >= 0) {
        const QSignalBlocker blocker{ui.volume};
        ui.volume->setValue(ui.controller->volume());
    }
}

QStringList MainWindow::selectedRemoteUris(const QTableView* view) const {
    if (view == nullptr || view->model() == nullptr) {
        return {};
    }
    auto rows = selectedRows(view);
    if (rows.isEmpty() && view->currentIndex().isValid()) {
        rows.push_back(view->currentIndex().row());
    }
    if (rows.isEmpty()) {
        return {};
    }

    const auto& ui = *implementation_;
    const auto document_id = view->property("trackknife-list-document-id").toString();
    if (!document_id.isEmpty()) {
        const auto document = ui.local_documents.constFind(document_id);
        if (document == ui.local_documents.cend()) {
            return {};
        }
        for (const auto& row : rows) {
            const auto value = row.toInt();
            if (value < 0 || static_cast<std::size_t>(value) >= document->items.size() ||
                document->items[static_cast<std::size_t>(value)].source !=
                    persistence::ListSource::mpd) {
                return {};
            }
        }
    }

    QStringList uris;
    if (const auto* search = qobject_cast<const quick::MpdSearchResultModel*>(view->model())) {
        for (const auto& row : rows) {
            uris.append(search->urisAt(row.toInt()));
        }
        return uris;
    }
    if (const auto* browser = qobject_cast<const quick::MpdBrowserModel*>(view->model())) {
        for (const auto& row : rows) {
            const auto value = row.toInt();
            if (browser->kindAt(value) != quick::MpdBrowserModel::EntryKind::track) {
                continue;
            }
            if (const auto uri = browser->uriAt(value)) {
                uris.push_back(QString::fromUtf8(uri->data(), static_cast<qsizetype>(uri->size())));
            }
        }
        return uris;
    }
    if (const auto* model = qobject_cast<const quick::MpdQueueModel*>(view->model())) {
        for (const auto& row : rows) {
            if (const auto uri = model->uriAt(row.toInt())) {
                uris.push_back(QString::fromUtf8(uri->data(), static_cast<qsizetype>(uri->size())));
            }
        }
    }
    return uris;
}

void MainWindow::activateCurrentSelection() {
    auto& ui = *implementation_;
    QTableView* view = nullptr;
    if (ui.search_page->isVisible()) {
        view = ui.search_view;
    } else if (ui.center_stack->currentWidget() == ui.library_page) {
        view = ui.library_view;
    } else if (ui.center_stack->currentWidget() == ui.tabs &&
               ui.tabs->currentWidget() == ui.queue_view) {
        view = ui.queue_view;
    } else {
        view = activeLibraryTabView();
    }
    if (view == nullptr || !view->currentIndex().isValid()) {
        return;
    }
    if (view == ui.queue_view) {
        ui.controller->playQueueItem(view->currentIndex().row());
        return;
    }
    if (view == ui.search_view) {
        ui.search_view->activateDefault(SearchQueueAction::replace);
        return;
    }
    if (view == ui.library_view && view->model() == ui.tag_model) {
        const auto query = view->currentIndex().data().toString();
        if (query.isEmpty()) {
            return;
        }
        if (view->property("trackknife-library-index-kind").toString() ==
            QStringLiteral("playlist")) {
            ui.controller->openStoredPlaylist(query);
            return;
        }
        ui.search->setText(query);
        ui.search_page->show();
        ui.search_page->raise();
        positionLiveSearchSurface();
        ui.controller->searchLibrary(query);
        return;
    }
    if (view == ui.library_view) {
        if (const auto* browser = qobject_cast<const quick::MpdBrowserModel*>(view->model());
            browser != nullptr && browser->kindAt(view->currentIndex().row()) !=
                                      quick::MpdBrowserModel::EntryKind::track) {
            ui.controller->activateBrowserItem(view->currentIndex().row());
            return;
        }
    }
    const auto uris = selectedRemoteUris(view);
    if (uris.isEmpty()) {
        return;
    }
    ui.controller->replaceQueueWithUris(uris);
}

void MainWindow::addCurrentSelection(const bool next) {
    auto& ui = *implementation_;
    QTableView* view = nullptr;
    if (ui.search_page->isVisible()) {
        view = ui.search_view;
    } else if (ui.center_stack->currentWidget() == ui.library_page) {
        view = ui.library_view;
    } else if (ui.center_stack->currentWidget() == ui.tabs &&
               ui.tabs->currentWidget() == ui.queue_view) {
        view = ui.queue_view;
    } else {
        view = activeLibraryTabView();
    }
    if (view == nullptr) {
        return;
    }
    if (view == ui.search_view) {
        ui.search_view->activateDefault(next ? SearchQueueAction::insert
                                             : SearchQueueAction::append);
        return;
    }
    auto rows = selectedRows(view);
    if (rows.isEmpty() && view->currentIndex().isValid()) {
        rows.push_back(view->currentIndex().row());
    }
    if (view == ui.library_view) {
        if (view->model() == ui.tag_model) {
            return;
        }
        if (ui.controller->browserShowingPlaylist()) {
            ui.controller->addBrowserPlaylistItems(rows, next);
        } else {
            ui.controller->addBrowserItems(rows, next);
        }
        return;
    }
    ui.controller->addUris(selectedRemoteUris(view), next);
}

void MainWindow::installTrackContextMenu(QTableView* view) {
    if (view == nullptr || view->property("trackknife-context-menu-installed").toBool()) {
        return;
    }
    view->setProperty("trackknife-context-menu-installed", true);
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view, &QWidget::customContextMenuRequested, this,
            [this, view](const QPoint& position) { showTrackContextMenu(view, position); });
}

void MainWindow::showTrackContextMenu(QTableView* view, const QPoint& position) {
    auto& ui = *implementation_;
    if (view == nullptr || ui.track_context_menu == nullptr || view->model() == nullptr) {
        return;
    }
    const auto index = view->indexAt(position);
    if (!index.isValid()) {
        return;
    }
    const auto selected = view->selectionModel()->isRowSelected(index.row(), QModelIndex{});
    view->selectionModel()->setCurrentIndex(index, selected ? QItemSelectionModel::NoUpdate
                                                            : QItemSelectionModel::ClearAndSelect |
                                                                  QItemSelectionModel::Rows);

    if (view != ui.library_view && view != ui.search_view) {
        for (int tab = 0; tab < ui.tabs->count(); ++tab) {
            auto* page = ui.tabs->widget(tab);
            if (page == view || (page != nullptr && page->isAncestorOf(view))) {
                ui.center_stack->setCurrentWidget(ui.tabs);
                ui.tabs->setCurrentIndex(tab);
                break;
            }
        }
    }
    refreshUi();
    rebuildTransferMenus();

    const auto queue = view == ui.queue_view;
    auto* page = ui.tabs->currentWidget();
    const auto view_is_tabbed = view != ui.library_view && view != ui.search_view;
    const auto local =
        view_is_tabbed && page != nullptr && page->property("trackknife-local-list-tab").toBool();
    const auto stored = view_is_tabbed && page != nullptr &&
                        page->property("trackknife-stored-playlist-tab").toBool();
    const auto classic_search =
        view_is_tabbed && page != nullptr && page->property("trackknife-search-tab").toBool();
    const auto tabbed_source = queue || local || stored || classic_search;

    auto* menu = ui.track_context_menu;
    menu->hide();
    menu->clear();
    menu->addAction(ui.activate_action);
    menu->addAction(ui.add_next_action);
    menu->addAction(ui.add_action);
    if (queue || local || stored) {
        menu->addSeparator();
        menu->addAction(ui.remove_action);
    }
    if (queue || local) {
        menu->addAction(ui.crop_action);
    }
    if (queue) {
        menu->addMenu(ui.priority_menu);
    }
    if (tabbed_source && !ui.local_documents.isEmpty()) {
        menu->addSeparator();
        menu->addMenu(ui.copy_to_menu);
        if (!classic_search) {
            menu->addMenu(ui.move_to_menu);
        }
    }
    menu->popup(view->viewport()->mapToGlobal(position));
}

void MainWindow::showTabContextMenu(const QPoint& position) {
    auto& ui = *implementation_;
    if (ui.tab_context_menu == nullptr) {
        return;
    }
    const auto index = ui.tabs->tabBar()->tabAt(position);
    if (index < 0) {
        return;
    }
    ui.center_stack->setCurrentWidget(ui.tabs);
    ui.tabs->setCurrentIndex(index);
    refreshUi();

    auto* page = ui.tabs->widget(index);
    const auto local = page != nullptr && page->property("trackknife-local-list-tab").toBool();
    const auto stored =
        page != nullptr && page->property("trackknife-stored-playlist-tab").toBool();
    const auto closable = page != ui.queue_view;
    auto* menu = ui.tab_context_menu;
    menu->hide();
    menu->clear();
    if (local) {
        menu->addAction(ui.rename_tab_action);
        menu->addAction(ui.pin_tab_action);
        menu->addAction(ui.duplicate_tab_action);
        menu->addSeparator();
    } else if (stored) {
        menu->addAction(ui.playlist_rename_action);
        menu->addSeparator();
    }
    menu->addAction(ui.move_tab_left_action);
    menu->addAction(ui.move_tab_right_action);
    if (closable) {
        menu->addSeparator();
        menu->addAction(ui.close_tab_action);
    }
    menu->popup(ui.tabs->tabBar()->mapToGlobal(position));
}

void MainWindow::activateServerTreeAction(const QModelIndex& index, const int action) {
    auto& ui = *implementation_;
    if (!index.isValid() || action < 0 || action > 3) {
        return;
    }
    const auto proxy_index = ui.server_library_filter_model->mapFromSource(index);
    if (proxy_index.isValid()) {
        ui.server_library_tree->setCurrentIndex(proxy_index);
    }

    if (ui.server_library_model->canFetchMore(index)) {
        ui.pending_server_tree_index = index;
        ui.pending_server_tree_action = static_cast<Impl::ServerTreeAction>(action);
        if (proxy_index.isValid()) {
            ui.server_library_tree->expand(proxy_index);
        }
        ui.server_library_model->fetchMore(index);
        return;
    }

    const auto tracks = ui.server_library_model->tracks(index);
    if (tracks.empty()) {
        showToast(QStringLiteral("This library node contains no tracks"));
        return;
    }
    if (action == static_cast<int>(Impl::ServerTreeAction::add_to_list)) {
        appendServerTreeSelectionToList(index, ui.pending_server_tree_target);
        ui.pending_server_tree_target.clear();
        return;
    }

    QStringList uris;
    uris.reserve(static_cast<qsizetype>(tracks.size()));
    for (const auto& track : tracks) {
        uris.push_back(displayText(track.uri));
    }
    if (action == static_cast<int>(Impl::ServerTreeAction::replace)) {
        ui.controller->replaceQueueWithUris(uris);
    } else {
        ui.controller->addUris(uris, action == static_cast<int>(Impl::ServerTreeAction::insert));
    }
}

void MainWindow::completePendingServerTreeAction() {
    auto& ui = *implementation_;
    if (!ui.pending_server_tree_action) {
        return;
    }
    if (!ui.pending_server_tree_index.isValid()) {
        ui.pending_server_tree_action.reset();
        ui.pending_server_tree_target.clear();
        return;
    }
    const auto index = QModelIndex{ui.pending_server_tree_index};
    const auto action = static_cast<int>(*ui.pending_server_tree_action);
    ui.pending_server_tree_action.reset();
    ui.pending_server_tree_index = QPersistentModelIndex{};
    activateServerTreeAction(index, action);
}

void MainWindow::appendServerTreeSelectionToList(const QModelIndex& index,
                                                 const QString& target_document_id) {
    auto& ui = *implementation_;
    auto target = ui.local_documents.find(target_document_id);
    const auto profile_id = core::StableId::parse(ui.controller->profileId().toStdString());
    if (target == ui.local_documents.end() || !profile_id) {
        showToast(QStringLiteral("The target list or server profile is no longer available"));
        return;
    }
    const auto tracks = ui.server_library_model->tracks(index);
    for (const auto& track : tracks) {
        target->items.push_back(listItemFromTrack(track, *profile_id));
    }
    QWidget* target_page = nullptr;
    for (int tab = 0; tab < ui.tabs->count(); ++tab) {
        auto* page = ui.tabs->widget(tab);
        if (page->property("trackknife-list-document-id").toString() == target_document_id) {
            target_page = page;
            break;
        }
    }
    refreshLocalList(target_page, true);
    persistLocalTabs();
    showToast(QStringLiteral("Added %1 track%2 to %3")
                  .arg(tracks.size())
                  .arg(tracks.size() == 1U ? QString{} : QStringLiteral("s"))
                  .arg(displayText(target->name)));
    refreshUi();
}

void MainWindow::showServerTreeContextMenu(const QPoint& position) {
    auto& ui = *implementation_;
    const auto proxy_index = ui.server_library_tree->indexAt(position).siblingAtColumn(0);
    if (!proxy_index.isValid()) {
        return;
    }
    const auto source_index = ui.server_library_filter_model->mapToSource(proxy_index);
    ui.server_library_tree->setCurrentIndex(proxy_index);

    QMenu menu{ui.server_library_tree};
    auto* append =
        menu.addAction(themedIcon(QStringLiteral("list-add"), style(), QStyle::SP_DialogOpenButton),
                       QStringLiteral("Append to live queue"));
    auto* insert =
        menu.addAction(themedIcon(QStringLiteral("go-next"), style(), QStyle::SP_ArrowRight),
                       QStringLiteral("Insert next in live queue"));
    auto* replace = menu.addAction(
        themedIcon(QStringLiteral("media-playback-start"), style(), QStyle::SP_MediaPlay),
        QStringLiteral("Replace queue and play"));
    connect(append, &QAction::triggered, this,
            [this, source_index] { activateServerTreeAction(source_index, 0); });
    connect(insert, &QAction::triggered, this,
            [this, source_index] { activateServerTreeAction(source_index, 1); });
    connect(replace, &QAction::triggered, this,
            [this, source_index] { activateServerTreeAction(source_index, 2); });

    auto* lists = menu.addMenu(QStringLiteral("Add to list"));
    for (int tab = 0; tab < ui.tabs->count(); ++tab) {
        auto* page = ui.tabs->widget(tab);
        const auto id = page->property("trackknife-list-document-id").toString();
        if (id.isEmpty()) {
            continue;
        }
        const auto name = page->property("trackknife-list-name").toString();
        auto* destination = lists->addAction(name);
        connect(destination, &QAction::triggered, this, [this, source_index, id] {
            implementation_->pending_server_tree_target = id;
            activateServerTreeAction(source_index,
                                     static_cast<int>(Impl::ServerTreeAction::add_to_list));
        });
    }
    if (lists->isEmpty()) {
        auto* unavailable = lists->addAction(QStringLiteral("No working lists"));
        unavailable->setEnabled(false);
    }

    menu.addSeparator();
    auto* expand =
        menu.addAction(ui.server_library_tree->isExpanded(proxy_index) ? QStringLiteral("Collapse")
                                                                       : QStringLiteral("Expand"));
    connect(expand, &QAction::triggered, this, [this, proxy_index] {
        auto* tree = implementation_->server_library_tree;
        tree->setExpanded(proxy_index, !tree->isExpanded(proxy_index));
    });
    menu.exec(ui.server_library_tree->viewport()->mapToGlobal(position));
}

void MainWindow::initializePersistence() {
    auto& ui = *implementation_;
    if (ui.persistence == nullptr || ui.synthetic_scratch) {
        return;
    }
    ui.persistence->initialize([this](PersistedWorkspace snapshot, const QString& error) {
        auto& state = *implementation_;
        if (!error.isEmpty()) {
            state.persistence_error = error;
            showToast(error);
            return;
        }
        state.persistence_ready = true;
        setProperty("trackknife-persistence-ready", true);
        state.profiles = std::move(snapshot.profiles);
        state.view_presets.clear();
        for (const auto& preset : snapshot.view_presets) {
            state.view_presets.insert(
                displayText(preset.binding),
                QByteArray{preset.header_state.data(),
                           static_cast<qsizetype>(preset.header_state.size())});
        }
        if (const auto queue_state = state.view_presets.value(QStringLiteral("live-queue"));
            !queue_state.isEmpty()) {
            static_cast<void>(state.queue_view->horizontalHeader()->restoreState(queue_state));
        }
        if (!snapshot.lists.empty()) {
            for (int index = state.tabs->count() - 1; index >= 0; --index) {
                auto* page = state.tabs->widget(index);
                if (page != nullptr && page->property("trackknife-local-list-tab").toBool()) {
                    state.tabs->removeTab(index);
                    page->deleteLater();
                }
            }
            state.local_documents.clear();
            state.scratch_view = nullptr;
            restoreLocalTabs(std::move(snapshot.lists));
        }
        persistLocalTabs();
        autoConnect();
        refreshUi();
    });
}

void MainWindow::restoreLocalTabs(std::vector<persistence::ListDocument> documents) {
    for (auto& document : documents) {
        addLocalListTab(std::move(document), false);
    }
}

void MainWindow::renameCurrentTab() {
    auto& ui = *implementation_;
    auto* page = ui.tabs->currentWidget();
    if (page == nullptr || !page->property("trackknife-local-list-tab").toBool()) {
        return;
    }
    const auto current_name = page->property("trackknife-list-name").toString();
    bool accepted = false;
    const auto name =
        QInputDialog::getText(this, QStringLiteral("Rename list"), QStringLiteral("Name:"),
                              QLineEdit::Normal, current_name, &accepted)
            .trimmed();
    if (!accepted || name.isEmpty() || name == current_name) {
        return;
    }
    const auto id = page->property("trackknife-list-document-id").toString();
    auto found = ui.local_documents.find(id);
    if (found == ui.local_documents.end()) {
        return;
    }
    found->name = utf8Bytes(name);
    page->setProperty("trackknife-list-name", name);
    refreshLocalList(page, true);
    persistLocalTabs();
    refreshUi();
}

void MainWindow::closeCurrentTab() {
    auto& ui = *implementation_;
    auto* page = ui.tabs->currentWidget();
    if (page == nullptr || page == ui.queue_view) {
        return;
    }
    const auto index = ui.tabs->indexOf(page);
    const auto local = page->property("trackknife-local-list-tab").toBool();
    if (local) {
        if (page->property("trackknife-list-pinned").toBool()) {
            showToast(QStringLiteral("Unpin this list before closing it"));
            return;
        }
        if (page->property("trackknife-list-dirty").toBool() &&
            QMessageBox::question(this, QStringLiteral("Close unsaved list"),
                                  QStringLiteral("Discard the unsaved contents of “%1”?")
                                      .arg(page->property("trackknife-list-name").toString())) !=
                QMessageBox::Yes) {
            return;
        }
        const auto id = page->property("trackknife-list-document-id").toString();
        ui.local_documents.remove(id);
    }
    if (ui.pending_search_model != nullptr && ui.pending_search_model->parent() == page) {
        ui.pending_search_model.clear();
        ui.pending_search_status.clear();
        ui.pending_search_query.clear();
    }
    ui.tabs->removeTab(index);
    if (ui.scratch_view == page) {
        ui.scratch_view = nullptr;
    }
    page->deleteLater();
    persistLocalTabs();
    refreshUi();
}

void MainWindow::persistLocalTabs() {
    auto& ui = *implementation_;
    if (ui.persistence == nullptr || ui.synthetic_scratch) {
        return;
    }
    std::vector<persistence::ListDocument> documents;
    documents.reserve(static_cast<std::size_t>(ui.local_documents.size()));
    for (int index = 0; index < ui.tabs->count(); ++index) {
        auto* page = ui.tabs->widget(index);
        if (page == nullptr || !page->property("trackknife-local-list-tab").toBool()) {
            continue;
        }
        const auto id = page->property("trackknife-list-document-id").toString();
        const auto found = ui.local_documents.find(id);
        if (found == ui.local_documents.end()) {
            continue;
        }
        auto document = *found;
        document.name = utf8Bytes(page->property("trackknife-list-name").toString());
        document.dirty = page->property("trackknife-list-dirty").toBool();
        *found = document;
        documents.push_back(std::move(document));
    }
    std::vector<persistence::TrackViewPreset> presets;
    const auto add_preset = [&presets](const QString& binding, const QTableView* view) {
        if (view == nullptr) {
            return;
        }
        const auto state = view->horizontalHeader()->saveState();
        presets.push_back(persistence::TrackViewPreset{
            .binding = utf8Bytes(binding),
            .header_state = std::string{state.constData(), static_cast<std::size_t>(state.size())},
        });
    };
    add_preset(QStringLiteral("live-queue"), ui.queue_view);
    for (int index = 0; index < ui.tabs->count(); ++index) {
        auto* page = ui.tabs->widget(index);
        if (page != nullptr && page->property("trackknife-local-list-tab").toBool()) {
            add_preset(QStringLiteral("local:%1")
                           .arg(page->property("trackknife-list-document-id").toString()),
                       qobject_cast<QTableView*>(page));
        }
    }
    ui.pending_documents = std::move(documents);
    ui.pending_view_presets = std::move(presets);
    if (ui.persistence_ready) {
        ui.persistence_timer->start();
    }
}

void MainWindow::flushLocalTabs() {
    auto& ui = *implementation_;
    if (ui.persistence == nullptr || !ui.persistence_ready || ui.synthetic_scratch) {
        return;
    }
    ui.persistence_timer->stop();
    const auto error =
        ui.persistence->saveWorkspaceAndWait(ui.pending_documents, ui.pending_view_presets);
    if (!error.isEmpty()) {
        ui.persistence_error = error;
    }
}

void MainWindow::openStoredPlaylistTab(const QString& name) {
    auto& ui = *implementation_;
    if (name.isEmpty()) {
        return;
    }

    QWidget* page = nullptr;
    quick::MpdQueueModel* model = nullptr;
    QLabel* status = nullptr;
    for (int index = 0; index < ui.tabs->count(); ++index) {
        auto* candidate = ui.tabs->widget(index);
        if (candidate != nullptr &&
            candidate->property("trackknife-stored-playlist-name").toString() == name) {
            page = candidate;
            model = candidate->findChild<quick::MpdQueueModel*>();
            status = candidate->findChild<QLabel*>(QStringLiteral("stored-playlist-status"));
            break;
        }
    }

    if (page == nullptr) {
        page = new QWidget(ui.tabs);
        page->setProperty("trackknife-stored-playlist-tab", true);
        page->setProperty("trackknife-stored-playlist-name", name);
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        model = new quick::MpdQueueModel(page);
        auto* view = makeTrackView(model, page);
        view->setObjectName(QStringLiteral("track-view-stored-playlist"));
        layout->addWidget(view, 1);
        status = new QLabel(page);
        status->setObjectName(QStringLiteral("stored-playlist-status"));
        status->setContentsMargins(4, 2, 4, 2);
        layout->addWidget(status);

        const auto index = ui.tabs->addTab(page, name);
        ui.tabs->setTabToolTip(index, QStringLiteral("Server playlist: %1").arg(name));
        auto* close = new QToolButton(ui.tabs->tabBar());
        close->setObjectName(QStringLiteral("stored-playlist-tab-close"));
        close->setAutoRaise(true);
        close->setIcon(
            themedIcon(QStringLiteral("window-close"), style(), QStyle::SP_TitleBarCloseButton));
        close->setToolTip(QStringLiteral("Close playlist"));
        ui.tabs->tabBar()->setTabButton(index, QTabBar::RightSide, close);
        connect(close, &QToolButton::clicked, page, [this, page] {
            auto& state = *implementation_;
            const auto tab_index = state.tabs->indexOf(page);
            if (tab_index >= 0) {
                state.tabs->removeTab(tab_index);
            }
            page->deleteLater();
            refreshUi();
        });
        installSearchView(view);
    }

    if (model == nullptr || status == nullptr) {
        showToast(QStringLiteral("Playlist tab could not be initialized"));
        return;
    }
    auto tracks = ui.controller->browserPlaylistTracksSnapshot();
    model->replaceTracks(std::move(tracks));
    status->setText(QStringLiteral("Server playlist · %1 track%2")
                        .arg(model->rowCount())
                        .arg(model->rowCount() == 1 ? QString{} : QStringLiteral("s")));
    ui.library_mode = Impl::LibraryMode::none;
    ui.center_stack->setCurrentWidget(ui.tabs);
    ui.tabs->setCurrentWidget(page);
    refreshUi();
}

void MainWindow::installSearchView(QTableView* view) {
    view->setDragEnabled(true);
    installTrackContextMenu(view);
    connect(view->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex&, const QModelIndex&) { refreshSelectionDetails(); });
    connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this](const QItemSelection&, const QItemSelection&) { refreshUi(); });
    connect(view, &QTableView::doubleClicked, this,
            [this](const QModelIndex&) { implementation_->add_action->trigger(); });
    auto* add_next = new QShortcut(QKeySequence(QStringLiteral("Shift+Return")), view);
    add_next->setContext(Qt::WidgetWithChildrenShortcut);
    connect(add_next, &QShortcut::activated, this,
            [this] { implementation_->add_next_action->trigger(); });
    auto* replace_and_play = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Return")), view);
    replace_and_play->setContext(Qt::WidgetWithChildrenShortcut);
    connect(replace_and_play, &QShortcut::activated, this,
            [this] { implementation_->activate_action->trigger(); });
}

QTableView* MainWindow::activeLibraryTabView() const {
    const auto& ui = *implementation_;
    if (ui.center_stack->currentWidget() != ui.tabs) {
        return nullptr;
    }
    auto* page = ui.tabs->currentWidget();
    if (page == nullptr) {
        return nullptr;
    }
    if (page->property("trackknife-search-tab").toBool()) {
        return page->findChild<QTableView*>(QStringLiteral("track-view-search-result"));
    }
    if (page->property("trackknife-stored-playlist-tab").toBool()) {
        return page->findChild<QTableView*>(QStringLiteral("track-view-stored-playlist"));
    }
    if (page->property("trackknife-local-list-tab").toBool()) {
        return qobject_cast<QTableView*>(page);
    }
    return nullptr;
}

void MainWindow::closeLiveSearch() {
    auto& ui = *implementation_;
    if (!ui.search_page->isVisible()) {
        return;
    }
    ui.search_page->hide();
    refreshSelectionDetails();
    refreshUi();

    QWidget* focus_target = nullptr;
    auto* destination = ui.center_stack->currentWidget();
    if (destination == ui.tabs) {
        auto* page = ui.tabs->currentWidget();
        focus_target = qobject_cast<QTableView*>(page);
        if (focus_target == nullptr && page != nullptr) {
            focus_target = page->findChild<QTableView*>();
        }
    } else if (destination == ui.library_page) {
        focus_target = ui.library_view;
    }
    (focus_target != nullptr ? focus_target : destination)->setFocus(Qt::ShortcutFocusReason);
}

void MainWindow::syncLiveSearchView() {
    auto& ui = *implementation_;
    ui.search_view->clearSpans();
    for (const auto row : ui.live_search_model->sectionRows()) {
        ui.search_view->setSpan(row, 0, 1, quick::MpdSearchResultModel::column_count);
    }
    const auto current = ui.search_view->currentIndex();
    if (!current.isValid() || ui.live_search_model->kindAt(current.row()) ==
                                  quick::MpdSearchResultModel::ResultKind::section) {
        const auto first = ui.live_search_model->firstResultRow();
        if (first >= 0) {
            ui.search_view->setCurrentIndex(ui.live_search_model->index(first, 1));
        }
    }
}

void MainWindow::refreshUi() {
    auto& ui = *implementation_;
    const auto connected = ui.controller->connected();
    const auto tree_capable = ui.controller->supportsTag(QStringLiteral("AlbumArtist")) ||
                              ui.controller->supportsTag(QStringLiteral("Artist"));
    if (!connected) {
        ui.server_tree_connected = false;
    } else if (tree_capable && (!ui.server_tree_connected ||
                                ui.server_tree_profile != ui.controller->profileId())) {
        ui.server_tree_connected = true;
        ui.server_tree_profile = ui.controller->profileId();
        ui.server_library_model->reload();
    }
    ui.server_library_tree->setEnabled(connected && tree_capable);
    const auto artwork_capable =
        connected && (ui.controller->supportsCommand(QStringLiteral("albumart")) ||
                      ui.controller->supportsCommand(QStringLiteral("readpicture")));
    ui.server_library_model->setArtworkEnabled(artwork_capable);
    ui.live_search_model->setArtworkEnabled(artwork_capable);
    auto* queue_artwork_model = qobject_cast<quick::MpdQueueModel*>(ui.controller->queueModel());
    Q_ASSERT(queue_artwork_model != nullptr);
    queue_artwork_model->setArtworkEnabled(artwork_capable);
    ui.server_folders->setEnabled(connected);
    ui.server_playlists->setEnabled(
        connected && ui.controller->supportsCommand(QStringLiteral("listplaylists")));
    const auto command_ready = connected && !ui.controller->commandBusy();
    ui.connect_action->setToolTip(ui.controller->status());
    ui.connect_action->setStatusTip(ui.controller->details());
    ui.connect_action->setEnabled(!ui.controller->busy());
    ui.disconnect_action->setEnabled(connected || ui.controller->busy());
    refreshTransport();
    auto* queue_model = qobject_cast<quick::MpdQueueModel*>(ui.controller->queueModel());
    const auto queue_count = ui.controller->queueCount();
    ui.queue_summary->setText(
        QStringLiteral("%1 track%2 (%3)")
            .arg(queue_count)
            .arg(queue_count == 1 ? QString{} : QStringLiteral("s"))
            .arg(formatLongTime(queue_model != nullptr ? queue_model->totalDurationMs() : 0)));
    ui.repeat_action->setEnabled(connected);
    ui.repeat_action->setChecked(ui.controller->repeatEnabled());
    ui.random_action->setEnabled(connected);
    ui.random_action->setChecked(ui.controller->randomEnabled());
    ui.single_button->setEnabled(connected);
    ui.single_cycle_action->setEnabled(command_ready);
    ui.single_button->setChecked(ui.controller->singleMode() > 0);
    ui.single_button->setText(ui.controller->singleMode() == 2 ? QStringLiteral("1×")
                                                               : QStringLiteral("1"));
    ui.single_button->setToolTip(
        QStringLiteral("Single: %1")
            .arg(ui.controller->singleMode() == 2   ? QStringLiteral("One-shot")
                 : ui.controller->singleMode() == 1 ? QStringLiteral("On")
                                                    : QStringLiteral("Off")));
    ui.consume_button->setEnabled(connected);
    ui.consume_cycle_action->setEnabled(command_ready);
    ui.consume_button->setChecked(ui.controller->consumeMode() > 0);
    ui.consume_button->setText(ui.controller->consumeMode() == 2 ? QStringLiteral("C×")
                                                                 : QStringLiteral("C"));
    ui.consume_button->setToolTip(
        QStringLiteral("Consume: %1")
            .arg(ui.controller->consumeMode() == 2   ? QStringLiteral("One-shot")
                 : ui.controller->consumeMode() == 1 ? QStringLiteral("On")
                                                     : QStringLiteral("Off")));
    ui.replay_gain_button->setVisible(ui.controller->supportsReplayGain());
    ui.replay_gain_button->setEnabled(connected);
    const auto replay_gain = ui.controller->replayGainMode();
    ui.replay_gain_button->setToolTip(QStringLiteral("ReplayGain: %1").arg(replay_gain.toUpper()));
    for (auto* action : ui.replay_gain_group->actions()) {
        action->setChecked(action->data().toString() == replay_gain);
    }

    ui.output_button->setEnabled(connected);
    ui.output_button->setToolTip(
        QStringLiteral("Output: %1").arg(ui.controller->activeOutputName()));
    ui.output_menu->clear();
    auto* output_model = ui.controller->outputModel();
    for (int row = 0; row < output_model->rowCount(); ++row) {
        const auto index = output_model->index(row, 0);
        const auto id = output_model->data(index, quick::MpdOutputModel::OutputIdRole).toUInt();
        const auto name = output_model->data(index, quick::MpdOutputModel::NameRole).toString();
        const auto enabled = output_model->data(index, quick::MpdOutputModel::EnabledRole).toBool();
        const auto primary = output_model->data(index, quick::MpdOutputModel::PrimaryRole).toBool();
        const auto detail = output_model->data(index, quick::MpdOutputModel::DetailRole).toString();
        auto* action = ui.output_menu->addAction(name);
        action->setObjectName(QStringLiteral("action-output-%1").arg(id));
        action->setCheckable(true);
        action->setChecked(ui.controller->supportsExclusiveOutput() ? primary : enabled);
        action->setToolTip(detail);
        connect(action, &QAction::triggered, this, [this, id, enabled] {
            if (implementation_->controller->supportsExclusiveOutput()) {
                implementation_->controller->switchOutput(id);
            } else {
                implementation_->controller->setOutputEnabled(id, !enabled);
            }
        });
    }
    if (ui.output_menu->isEmpty()) {
        auto* none = ui.output_menu->addAction(QStringLiteral("No outputs"));
        none->setEnabled(false);
    }

    ui.search_status->setText(ui.controller->libraryStatus());
    if (ui.library_mode == Impl::LibraryMode::browser) {
        ui.library_title->setText(ui.controller->browserPath().isEmpty()
                                      ? QStringLiteral("Music root")
                                      : ui.controller->browserPath());
        ui.library_status->setText(ui.controller->browserStatus());
        ui.browser_back->setVisible(!ui.controller->browserPath().isEmpty() ||
                                    ui.controller->browserShowingPlaylist());
        auto* model = ui.controller->browserShowingPlaylist()
                          ? ui.controller->browserPlaylistModel()
                          : ui.controller->browserModel();
        if (ui.library_view->model() != model) {
            setLibraryModel(model);
        }
    }

    const auto browser_visible = ui.center_stack->currentWidget() == ui.library_page;
    const auto live_search_visible = ui.search_page->isVisible();
    const auto tabs_visible = ui.center_stack->currentWidget() == ui.tabs;
    auto* current_tab = ui.tabs->currentWidget();
    const auto local_list_visible = tabs_visible && !live_search_visible &&
                                    current_tab != nullptr &&
                                    current_tab->property("trackknife-local-list-tab").toBool();
    const auto classic_search_visible = tabs_visible && !live_search_visible &&
                                        current_tab != nullptr &&
                                        current_tab->property("trackknife-search-tab").toBool();
    const auto stored_playlist_visible =
        tabs_visible && !live_search_visible && current_tab != nullptr &&
        current_tab->property("trackknife-stored-playlist-tab").toBool();
    const auto library_visible = browser_visible || live_search_visible || classic_search_visible ||
                                 stored_playlist_visible || local_list_visible;
    const auto status_actions_visible =
        browser_visible || classic_search_visible || stored_playlist_visible || local_list_visible;
    const auto live_queue_visible =
        tabs_visible && !live_search_visible && ui.tabs->currentWidget() == ui.queue_view;
    auto* active_library_view = browser_visible       ? ui.library_view
                                : live_search_visible ? static_cast<QTableView*>(ui.search_view)
                                                      : activeLibraryTabView();
    const auto has_library_selection = library_visible && active_library_view != nullptr &&
                                       active_library_view->selectionModel() != nullptr &&
                                       active_library_view->selectionModel()->hasSelection();
    const auto has_queue_selection =
        live_queue_visible && ui.queue_view->selectionModel()->hasSelection();
    const auto has_playlist_selection = stored_playlist_visible && has_library_selection;
    const auto has_local_selection = local_list_visible && has_library_selection;
    const auto active_row =
        active_library_view != nullptr ? active_library_view->currentIndex().row() : -1;
    const auto active_row_count = active_library_view != nullptr && active_library_view->model()
                                      ? active_library_view->model()->rowCount()
                                      : 0;
    if (tabs_visible && !live_queue_visible && active_library_view != nullptr) {
        const auto* model = qobject_cast<const quick::MpdQueueModel*>(active_library_view->model());
        const auto selected_count = selectedRows(active_library_view).size();
        ui.queue_summary->setText(
            QStringLiteral("%1 track%2 (%3)%4")
                .arg(active_row_count)
                .arg(active_row_count == 1 ? QString{} : QStringLiteral("s"))
                .arg(formatLongTime(model != nullptr ? model->totalDurationMs() : 0))
                .arg(selected_count > 0 ? QStringLiteral(" · %1 selected").arg(selected_count)
                                        : QString{}));
    }
    ui.add_button->setVisible(status_actions_visible);
    ui.add_next_button->setVisible(status_actions_visible);
    const auto live_search_result_selected =
        live_search_visible && ui.search_view->currentIndex().isValid() &&
        ui.live_search_model->kindAt(ui.search_view->currentIndex().row()) !=
            quick::MpdSearchResultModel::ResultKind::section;
    const auto browser_index_selected =
        browser_visible && has_library_selection && ui.library_view->model() == ui.tag_model;
    const auto selected_remote_uris = selectedRemoteUris(active_library_view);
    const auto can_enqueue_selection =
        command_ready && (live_search_result_selected ||
                          (!browser_index_selected && !selected_remote_uris.isEmpty()) ||
                          (live_queue_visible && !selectedRemoteUris(ui.queue_view).isEmpty()));
    ui.add_action->setEnabled(can_enqueue_selection);
    ui.add_next_action->setEnabled(can_enqueue_selection);
    const auto browser_container_selected =
        browser_visible &&
        qobject_cast<quick::MpdBrowserModel*>(ui.library_view->model()) != nullptr &&
        ui.library_view->currentIndex().isValid() &&
        qobject_cast<quick::MpdBrowserModel*>(ui.library_view->model())
                ->kindAt(ui.library_view->currentIndex().row()) !=
            quick::MpdBrowserModel::EntryKind::track;
    ui.activate_action->setText(live_queue_visible ? QStringLiteral("Play")
                                : (browser_index_selected || browser_container_selected)
                                    ? QStringLiteral("Open")
                                    : QStringLiteral("Replace queue and play"));
    ui.activate_action->setEnabled(
        browser_index_selected ||
        (command_ready && (has_queue_selection || live_search_result_selected ||
                           browser_container_selected || !selected_remote_uris.isEmpty())));
    ui.remove_action->setVisible(live_queue_visible || stored_playlist_visible ||
                                 local_list_visible);
    ui.move_up_action->setVisible(live_queue_visible || stored_playlist_visible ||
                                  local_list_visible);
    ui.move_down_action->setVisible(live_queue_visible || stored_playlist_visible ||
                                    local_list_visible);
    ui.list_button->setVisible(live_queue_visible || stored_playlist_visible ||
                               local_list_visible || classic_search_visible || live_search_visible);
    ui.remove_action->setEnabled(
        has_local_selection ||
        (command_ready &&
         (has_queue_selection ||
          (has_playlist_selection && ui.controller->supportsCommand("playlistdelete")))));
    const auto queue_row = ui.queue_view->currentIndex().row();
    ui.move_up_action->setEnabled(
        (has_local_selection && active_row > 0) ||
        (command_ready && ((has_queue_selection && queue_row > 0) ||
                           (has_playlist_selection && active_row > 0 &&
                            ui.controller->supportsCommand("playlistmove")))));
    ui.move_down_action->setEnabled(
        (has_local_selection && active_row + 1 < active_row_count) ||
        (command_ready && ((has_queue_selection && queue_row + 1 < ui.controller->queueCount()) ||
                           (has_playlist_selection && active_row + 1 < active_row_count &&
                            ui.controller->supportsCommand("playlistmove")))));
    ui.crop_action->setVisible(live_queue_visible || local_list_visible);
    ui.clear_action->setVisible(live_queue_visible || local_list_visible);
    ui.clear_action->setText(local_list_visible ? QStringLiteral("Clear working list")
                                                : QStringLiteral("Clear live queue"));
    ui.priority_menu->menuAction()->setVisible(live_queue_visible);
    ui.crop_action->setEnabled(has_local_selection || (has_queue_selection && command_ready));
    ui.clear_action->setEnabled(
        (local_list_visible && active_row_count > 0) ||
        (live_queue_visible && command_ready && ui.controller->queueCount() > 0));
    ui.sort_action->setVisible(local_list_visible);
    ui.reverse_action->setVisible(local_list_visible);
    ui.randomize_list_action->setVisible(local_list_visible);
    ui.deduplicate_action->setVisible(local_list_visible);
    ui.sort_action->setEnabled(local_list_visible && active_row_count > 1);
    ui.reverse_action->setEnabled(local_list_visible && active_row_count > 1);
    ui.randomize_list_action->setEnabled(local_list_visible && active_row_count > 1);
    ui.deduplicate_action->setEnabled(local_list_visible && active_row_count > 1);
    ui.load_more_search_action->setVisible(live_search_visible || classic_search_visible);
    const auto visible_search_query =
        live_search_visible
            ? ui.search->text().trimmed()
            : (classic_search_visible ? current_tab->property("trackknife-search-query").toString()
                                      : QString{});
    ui.load_more_search_action->setEnabled(command_ready && ui.controller->hasMoreSearchResults() &&
                                           visible_search_query ==
                                               ui.controller->lastSearchQuery());
    ui.copy_to_menu->menuAction()->setVisible(!ui.local_documents.isEmpty());
    ui.move_to_menu->menuAction()->setVisible(!ui.local_documents.isEmpty());
    ui.copy_to_menu->setEnabled(has_queue_selection || has_library_selection);
    ui.move_to_menu->setEnabled(has_local_selection || has_queue_selection ||
                                (has_playlist_selection && command_ready &&
                                 ui.controller->supportsCommand("playlistdelete")));
    ui.priority_menu->setEnabled(has_queue_selection && command_ready &&
                                 ui.controller->supportsCommand("prioid"));
    std::optional<unsigned> selected_priority;
    bool priorities_match = true;
    if (has_queue_selection) {
        for (const auto& row : selectedRows(ui.queue_view)) {
            const auto value = ui.queue_view->model()
                                   ->index(row.toInt(), 0)
                                   .data(quick::MpdQueueModel::PriorityRole);
            if (!value.isValid()) {
                priorities_match = false;
                break;
            }
            const auto priority = value.toUInt();
            if (!selected_priority) {
                selected_priority = priority;
            } else if (*selected_priority != priority) {
                priorities_match = false;
                break;
            }
        }
    }
    for (auto* action : ui.priority_menu->actions()) {
        action->setChecked(priorities_match && selected_priority &&
                           action->data().toUInt() == *selected_priority);
    }
    ui.playlist_load_action->setVisible(stored_playlist_visible);
    ui.playlist_clear_action->setVisible(stored_playlist_visible);
    ui.playlist_rename_action->setVisible(stored_playlist_visible);
    ui.playlist_delete_action->setVisible(stored_playlist_visible);
    ui.playlist_load_action->setEnabled(command_ready && ui.controller->supportsCommand("load"));
    ui.playlist_clear_action->setEnabled(command_ready && active_row_count > 0 &&
                                         ui.controller->supportsCommand("playlistclear"));
    ui.playlist_rename_action->setEnabled(command_ready &&
                                          ui.controller->supportsCommand("rename"));
    ui.playlist_delete_action->setEnabled(command_ready && ui.controller->supportsCommand("rm"));
    ui.save_queue_playlist_action->setEnabled(command_ready &&
                                              ui.controller->supportsCommand("save"));
    ui.rename_tab_action->setEnabled(local_list_visible);
    ui.duplicate_tab_action->setEnabled(local_list_visible);
    ui.pin_tab_action->setEnabled(local_list_visible);
    ui.pin_tab_action->setChecked(local_list_visible &&
                                  current_tab->property("trackknife-list-pinned").toBool());
    ui.save_working_list_action->setEnabled(local_list_visible);
    const auto current_tab_index = tabs_visible ? ui.tabs->currentIndex() : -1;
    ui.move_tab_left_action->setEnabled(current_tab_index > 0);
    ui.move_tab_right_action->setEnabled(current_tab_index >= 0 &&
                                         current_tab_index + 1 < ui.tabs->count());
    ui.close_tab_action->setEnabled(
        tabs_visible && current_tab != nullptr && current_tab != ui.queue_view &&
        (!local_list_visible || !current_tab->property("trackknife-list-pinned").toBool()));
}

void MainWindow::refreshSelectionDetails() {
    auto& ui = *implementation_;
    QTableView* view = nullptr;
    if (ui.search_page->isVisible()) {
        view = ui.search_view;
    } else if (ui.center_stack->currentWidget() == ui.library_page) {
        view = ui.library_view;
    } else if (ui.tabs->currentWidget() != nullptr &&
               (ui.tabs->currentWidget()->property("trackknife-search-tab").toBool() ||
                ui.tabs->currentWidget()->property("trackknife-stored-playlist-tab").toBool())) {
        view = activeLibraryTabView();
    } else {
        view = qobject_cast<QTableView*>(ui.tabs->currentWidget());
    }

    if (view == nullptr || view->model() == nullptr || !view->currentIndex().isValid()) {
        ui.details->setText(QStringLiteral("Select a track"));
        return;
    }

    const auto row = view->currentIndex().row();
    QStringList lines;
    for (int column = 0; column < view->model()->columnCount(); ++column) {
        const auto heading = view->model()->headerData(column, Qt::Horizontal).toString();
        const auto value = view->model()->index(row, column).data().toString();
        if (!heading.isEmpty() && !value.isEmpty()) {
            lines.push_back(QStringLiteral("%1: %2").arg(heading, value));
        }
    }
    ui.details->setText(lines.isEmpty() ? QStringLiteral("No details")
                                        : lines.join(QLatin1Char('\n')));
}

void MainWindow::setLibraryModel(QAbstractItemModel* model) {
    auto& ui = *implementation_;
    if (ui.library_view->model() == model) {
        return;
    }

    ui.library_view->setModel(model);
    connect(ui.library_view->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex&, const QModelIndex&) { refreshSelectionDetails(); });
    connect(ui.library_view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this](const QItemSelection&, const QItemSelection&) { refreshUi(); });
    refreshSelectionDetails();
}

void MainWindow::openFormatSandbox() {
    auto* existing = findChild<FormatSandboxDialog*>();
    if (existing != nullptr) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return;
    }
    auto* dialog = new FormatSandboxDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void MainWindow::openMpdConnectionDialog() {
    auto& ui = *implementation_;
    auto* existing = findChild<MpdConnectionDialog*>();
    if (existing != nullptr) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return;
    }
    auto profiles = ui.profiles;
    auto* dialog = new MpdConnectionDialog(this, profiles, ui.controller->profileId());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(
        dialog, &MpdConnectionDialog::connectionRequested, this,
        [this, profiles = std::move(profiles)](
            const QString& profile_id, const QString& profile_name, const QString& host,
            const int port, const QString& password, const QString& music_root,
            const bool auto_connect) mutable {
            auto& state = *implementation_;
            auto parsed_id = core::StableId::parse(profile_id.toStdString());
            if (!parsed_id) {
                showToast(QStringLiteral("Connection profile has an invalid identity"));
                return;
            }
            persistence::ConnectionProfile updated{
                .id = *parsed_id,
                .name = utf8Bytes(profile_name),
                .host = utf8Bytes(host),
                .port = static_cast<unsigned>(port),
                .local_music_root =
                    music_root.isEmpty()
                        ? std::nullopt
                        : std::optional<std::string>{QFile::encodeName(music_root).toStdString()},
                .auto_connect = auto_connect,
            };
            auto existing_profile =
                std::find_if(profiles.begin(), profiles.end(),
                             [&updated](const auto& saved) { return saved.id == updated.id; });
            if (auto_connect) {
                for (auto& saved : profiles) {
                    saved.auto_connect = false;
                }
            }
            if (existing_profile == profiles.end()) {
                profiles.push_back(std::move(updated));
            } else {
                *existing_profile = std::move(updated);
            }
            if (state.persistence != nullptr && state.persistence_ready) {
                state.persistence->saveProfiles(profiles, [this, profiles, profile_id, host, port,
                                                           password,
                                                           music_root](const QString& error) {
                    if (!error.isEmpty()) {
                        showToast(error);
                        return;
                    }
                    implementation_->profiles = profiles;
                    implementation_->controller->probeProfile(profile_id, host, port, password,
                                                              music_root);
                });
                return;
            }
            state.profiles = profiles;
            state.controller->probeProfile(profile_id, host, port, password, music_root);
        });
    dialog->show();
}

void MainWindow::restoreWorkspace() {
    QSettings settings;
    const auto geometry = settings.value(QStringLiteral("workspace/geometry")).toByteArray();
    const auto state = settings.value(QStringLiteral("workspace/state")).toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }
    if (!state.isEmpty() && !restoreState(state, layout_version)) {
        resetWorkspace();
        return;
    }
    const auto library_width =
        settings.value(QStringLiteral("workspace/library-dock-width"), 0).toInt();
    if (library_width > 0) {
        QTimer::singleShot(0, this, [this, library_width] {
            auto& ui = *implementation_;
            if (ui.library_dock->isFloating()) {
                return;
            }
            const auto maximum_width = std::max(180, width() - 360);
            resizeDocks({ui.library_dock}, {std::clamp(library_width, 180, maximum_width)},
                        Qt::Horizontal);
        });
    }
}

void MainWindow::autoConnect() {
    auto& ui = *implementation_;
    if (!ui.persistence_ready) {
        return;
    }
    if (ui.profiles.empty()) {
        openMpdConnectionDialog();
        return;
    }
    const auto profile = std::find_if(ui.profiles.begin(), ui.profiles.end(),
                                      [](const auto& saved) { return saved.auto_connect; });
    if (profile == ui.profiles.end()) {
        return;
    }
    const auto root = profile->local_music_root
                          ? QFile::decodeName(QByteArray{
                                profile->local_music_root->data(),
                                static_cast<qsizetype>(profile->local_music_root->size())})
                          : QString{};
    ui.controller->probeProfile(QString::fromStdString(profile->id.to_string()),
                                displayText(profile->host), static_cast<int>(profile->port),
                                QString{}, root);
}

void MainWindow::resetWorkspace() {
    QSettings settings;
    settings.remove(QStringLiteral("workspace"));
    auto& ui = *implementation_;
    ui.library_dock->setFloating(false);
    ui.details_dock->setFloating(false);
    ui.jobs_dock->setFloating(false);
    addDockWidget(Qt::LeftDockWidgetArea, ui.library_dock);
    addDockWidget(Qt::RightDockWidgetArea, ui.details_dock);
    addDockWidget(Qt::BottomDockWidgetArea, ui.jobs_dock);
    ui.library_dock->show();
    ui.details_dock->hide();
    ui.jobs_dock->hide();
    addToolBar(Qt::TopToolBarArea, ui.transport);
    addToolBarBreak(Qt::TopToolBarArea);
    addToolBar(Qt::TopToolBarArea, ui.progress_toolbar);
    ui.transport->show();
    ui.progress_toolbar->show();
    resize(1280, 800);
}

} // namespace trackknife::ui
