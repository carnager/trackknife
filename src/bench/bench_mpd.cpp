// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"
#include "bench/settings_dialog.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "quick/mpd_output_model.hpp"
#include "quick/mpd_probe_controller.hpp"
#include "quick/mpd_queue_model.hpp"
#include "quick/mpd_search_result_model.hpp"
#include "ui/mpd_connection_dialog.hpp"
#include "ui/server_library_tree_model.hpp"
#include "ui/server_library_tree_view.hpp"
#include "uicommon/list_persistence_service.hpp"
#include "uicommon/queue_table_view.hpp"
#include "uicommon/track_row_roles.hpp"

#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QSlider>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <ranges>
#include <utility>

namespace trackknife::bench {
namespace {

enum class MpdSearchQueueAction : std::uint8_t { append, next, replace };

class MpdSearchLineEdit final : public QLineEdit {
  public:
    explicit MpdSearchLineEdit(QWidget* parent) : QLineEdit(parent) {}

    void setResultFocusCallback(std::function<void()> callback) {
        result_focus_callback_ = std::move(callback);
    }
    void setCloseCallback(std::function<void()> callback) { close_callback_ = std::move(callback); }
    void setActionCallback(std::function<void(MpdSearchQueueAction)> callback) {
        action_callback_ = std::move(callback);
    }

  protected:
    void keyPressEvent(QKeyEvent* event) override {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            event->modifiers() == Qt::ControlModifier && action_callback_) {
            action_callback_(MpdSearchQueueAction::replace);
            event->accept();
            return;
        }
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            event->modifiers() == Qt::NoModifier && action_callback_) {
            action_callback_(MpdSearchQueueAction::append);
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
    std::function<void(MpdSearchQueueAction)> action_callback_;
};

class MpdSearchTableView final : public QTableView {
  public:
    explicit MpdSearchTableView(QWidget* parent) : QTableView(parent) {}

    void setSearchField(QLineEdit* field) { search_field_ = field; }
    void setCloseCallback(std::function<void()> callback) { close_callback_ = std::move(callback); }
    void setActionCallback(std::function<void(int, MpdSearchQueueAction)> callback) {
        action_callback_ = std::move(callback);
    }

    void focusFirstResult() {
        const auto* results = qobject_cast<const quick::MpdSearchResultModel*>(model());
        const auto row = results != nullptr ? results->firstResultRow() : -1;
        if (row < 0) {
            return;
        }
        selectionModel()->setCurrentIndex(model()->index(row, 1),
                                          QItemSelectionModel::ClearAndSelect |
                                              QItemSelectionModel::Rows);
        setFocus();
        scrollTo(currentIndex());
    }

    void activateDefault(const MpdSearchQueueAction action) {
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
            event->modifiers() == Qt::ControlModifier) {
            activateDefault(MpdSearchQueueAction::replace);
            event->accept();
            return;
        }
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            event->modifiers() == Qt::NoModifier) {
            activateDefault(actionForColumn(currentIndex().column()));
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
        if (event->key() == Qt::Key_Right && currentIndex().isValid()) {
            const auto current_action =
                currentIndex().column() < quick::MpdSearchResultModel::first_action_column
                    ? quick::MpdSearchResultModel::first_action_column
                    : currentIndex().column();
            const auto next_action =
                std::min(current_action + 1, quick::MpdSearchResultModel::column_count - 1);
            setCurrentIndex(currentIndex().siblingAtColumn(next_action));
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Left && currentIndex().isValid()) {
            const auto current_action =
                std::max(currentIndex().column(), quick::MpdSearchResultModel::first_action_column);
            const auto previous_action =
                std::max(current_action - 1, quick::MpdSearchResultModel::first_action_column);
            setCurrentIndex(currentIndex().siblingAtColumn(previous_action));
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

    [[nodiscard]] static MpdSearchQueueAction actionForColumn(const int column) {
        if (column == quick::MpdSearchResultModel::first_action_column + 1) {
            return MpdSearchQueueAction::next;
        }
        if (column == quick::MpdSearchResultModel::first_action_column + 2) {
            return MpdSearchQueueAction::replace;
        }
        return MpdSearchQueueAction::append;
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
    std::function<void(int, MpdSearchQueueAction)> action_callback_;
};

class MpdSearchAlbumDelegate final : public QStyledItemDelegate {
  public:
    explicit MpdSearchAlbumDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        const auto kind = static_cast<quick::MpdSearchResultModel::ResultKind>(
            index.data(quick::MpdSearchResultModel::ResultKindRole).toInt());
        if (kind != quick::MpdSearchResultModel::ResultKind::album) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        auto item = option;
        initStyleOption(&item, index);
        const auto text = item.text;
        item.text.clear();
        item.icon = {};
        item.features &= ~QStyleOptionViewItem::HasDecoration;
        const auto* widget = item.widget;
        auto* item_style = widget != nullptr ? widget->style() : QApplication::style();
        item_style->drawControl(QStyle::CE_ItemViewItem, &item, painter, widget);

        constexpr int cover_extent = 24;
        constexpr int horizontal_padding = 3;
        const auto cover_rect =
            QRect{item.rect.left() + horizontal_padding, item.rect.center().y() - cover_extent / 2,
                  cover_extent, cover_extent};
        const auto decoration = index.data(Qt::DecorationRole);
        if (decoration.canConvert<QImage>()) {
            const auto image = decoration.value<QImage>();
            if (!image.isNull()) {
                const auto scaled =
                    image.scaled(cover_rect.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
                painter->drawImage(
                    QStyle::alignedRect(item.direction, Qt::AlignCenter, scaled.size(), cover_rect),
                    scaled);
            }
        } else if (decoration.canConvert<QIcon>()) {
            decoration.value<QIcon>().paint(painter, cover_rect, Qt::AlignCenter);
        }

        const auto text_rect =
            item.rect.adjusted(cover_extent + horizontal_padding * 2, 0, -horizontal_padding, 0);
        const auto foreground = item.palette.color(item.state.testFlag(QStyle::State_Selected)
                                                       ? QPalette::HighlightedText
                                                       : QPalette::Text);
        painter->save();
        painter->setFont(item.font);
        painter->setPen(foreground);
        painter->drawText(
            text_rect, Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetrics{item.font}.elidedText(text, Qt::ElideRight, text_rect.width()));
        painter->restore();
    }
};

class MpdSearchActionDelegate final : public QStyledItemDelegate {
  public:
    MpdSearchActionDelegate(QIcon icon, QObject* parent)
        : QStyledItemDelegate(parent), icon_(std::move(icon)),
          view_(qobject_cast<QTableView*>(parent)) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        auto item = option;
        initStyleOption(&item, index);
        item.text.clear();
        item.icon = {};
        item.features &= ~QStyleOptionViewItem::HasDecoration;
        item.state &= ~QStyle::State_HasFocus;
        const auto* widget = item.widget;
        auto* item_style = widget != nullptr ? widget->style() : QApplication::style();
        item_style->drawControl(QStyle::CE_ItemViewItem, &item, painter, widget);
        const auto icon_rect =
            QStyle::alignedRect(item.direction, Qt::AlignCenter, QSize{12, 12}, item.rect);
        auto active = false;
        if (view_ != nullptr && view_->hasFocus()) {
            const auto current = view_->currentIndex();
            const auto active_column =
                current.column() >= quick::MpdSearchResultModel::first_action_column
                    ? current.column()
                    : quick::MpdSearchResultModel::first_action_column;
            active = current.row() == index.row() && active_column == index.column();
        }
        if (active) {
            const auto focus_rect =
                QStyle::alignedRect(item.direction, Qt::AlignCenter, QSize{18, 18}, item.rect)
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
    QTableView* view_{nullptr};
};

} // namespace

void BenchMainWindow::buildMpdStatusControls() {
    auto* separator = new QFrame(statusBar());
    mpd_status_separator_ = separator;
    separator->setObjectName(QStringLiteral("bench-mpd-status-separator"));
    separator->setFrameShape(QFrame::VLine);
    separator->setFrameShadow(QFrame::Sunken);
    statusBar()->addPermanentWidget(separator);

    const auto add_action_button = [this](QAction* action, const QString& object_name) {
        auto* button = new QToolButton(statusBar());
        button->setObjectName(object_name);
        button->setAutoRaise(true);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setDefaultAction(action);
        statusBar()->addPermanentWidget(button);
        return button;
    };

    mpd_repeat_action_ =
        new QAction(QIcon::fromTheme(QStringLiteral("media-playlist-repeat"),
                                     style()->standardIcon(QStyle::SP_BrowserReload)),
                    QStringLiteral("Repeat"), this);
    mpd_repeat_action_->setObjectName(QStringLiteral("action-mpd-repeat"));
    mpd_repeat_action_->setCheckable(true);
    mpd_repeat_button_ = add_action_button(mpd_repeat_action_, QStringLiteral("bench-mpd-repeat"));
    connect(mpd_repeat_action_, &QAction::triggered, mpd_controller_,
            &quick::MpdProbeController::setRepeatEnabled);

    mpd_random_action_ =
        new QAction(QIcon::fromTheme(QStringLiteral("media-playlist-shuffle"),
                                     style()->standardIcon(QStyle::SP_BrowserReload)),
                    QStringLiteral("Random"), this);
    mpd_random_action_->setObjectName(QStringLiteral("action-mpd-random"));
    mpd_random_action_->setCheckable(true);
    mpd_random_button_ = add_action_button(mpd_random_action_, QStringLiteral("bench-mpd-random"));
    connect(mpd_random_action_, &QAction::triggered, mpd_controller_,
            &quick::MpdProbeController::setRandomEnabled);

    mpd_single_action_ = new QAction(QStringLiteral("Cycle MPD single mode"), this);
    mpd_single_action_->setObjectName(QStringLiteral("action-mpd-single"));
    mpd_single_action_->setCheckable(true);
    mpd_single_button_ = new QToolButton(statusBar());
    mpd_single_button_->setObjectName(QStringLiteral("bench-mpd-single"));
    mpd_single_button_->setAutoRaise(true);
    mpd_single_button_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    mpd_single_button_->setDefaultAction(mpd_single_action_);
    statusBar()->addPermanentWidget(mpd_single_button_);
    connect(mpd_single_action_, &QAction::triggered, this, [this] {
        const auto mode = mpd_controller_->singleMode();
        mpd_controller_->setSingleMode(mode < 0 || mode >= 2 ? 0 : mode + 1);
    });

    mpd_consume_action_ = new QAction(QStringLiteral("Cycle MPD consume mode"), this);
    mpd_consume_action_->setObjectName(QStringLiteral("action-mpd-consume"));
    mpd_consume_action_->setCheckable(true);
    mpd_consume_button_ = new QToolButton(statusBar());
    mpd_consume_button_->setObjectName(QStringLiteral("bench-mpd-consume"));
    mpd_consume_button_->setAutoRaise(true);
    mpd_consume_button_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    mpd_consume_button_->setDefaultAction(mpd_consume_action_);
    statusBar()->addPermanentWidget(mpd_consume_button_);
    connect(mpd_consume_action_, &QAction::triggered, this, [this] {
        const auto mode = mpd_controller_->consumeMode();
        mpd_controller_->setConsumeMode(mode < 0 || mode >= 2 ? 0 : mode + 1);
    });

    mpd_append_selection_action_ = new QAction(
        QIcon::fromTheme(QStringLiteral("list-add"), style()->standardIcon(QStyle::SP_ArrowRight)),
        QStringLiteral("Append selection to queue"), this);
    mpd_append_selection_action_->setObjectName(QStringLiteral("action-mpd-append-selection"));
    connect(mpd_append_selection_action_, &QAction::triggered, this,
            [this] { mpd_controller_->addUris(selectedMpdQueueUris(), false); });
    mpd_add_next_selection_action_ = new QAction(
        QIcon::fromTheme(QStringLiteral("go-next"), style()->standardIcon(QStyle::SP_ArrowForward)),
        QStringLiteral("Add selection next"), this);
    mpd_add_next_selection_action_->setObjectName(QStringLiteral("action-mpd-add-next-selection"));
    connect(mpd_add_next_selection_action_, &QAction::triggered, this,
            [this] { mpd_controller_->addUris(selectedMpdQueueUris(), true); });
    mpd_load_local_action_ = new QAction(QIcon::fromTheme(QStringLiteral("folder-open")),
                                         QStringLiteral("Load as local files"), this);
    mpd_load_local_action_->setObjectName(QStringLiteral("action-mpd-load-local"));
    connect(mpd_load_local_action_, &QAction::triggered, this,
            [this] { loadMpdUrisAsLocalFiles(selectedMpdQueueUris()); });
    mpd_go_to_artist_action_ = new QAction(QStringLiteral("Go to artist"), this);
    mpd_go_to_artist_action_->setObjectName(QStringLiteral("action-mpd-go-to-artist"));
    connect(mpd_go_to_artist_action_, &QAction::triggered, this, [this] {
        const auto index = mpd_queue_view_->currentIndex();
        if (index.isValid()) {
            goToMpdLibraryEntry(index.siblingAtColumn(0)
                                    .data(static_cast<int>(ui::track_album_artist_role))
                                    .toString(),
                                {});
        }
    });
    mpd_go_to_album_action_ = new QAction(QStringLiteral("Go to album"), this);
    mpd_go_to_album_action_->setObjectName(QStringLiteral("action-mpd-go-to-album"));
    connect(mpd_go_to_album_action_, &QAction::triggered, this, [this] {
        const auto index = mpd_queue_view_->currentIndex();
        if (index.isValid()) {
            goToMpdLibraryEntry(
                index.siblingAtColumn(0)
                    .data(static_cast<int>(ui::track_album_artist_role))
                    .toString(),
                index.siblingAtColumn(ui::track_album_column).data(Qt::DisplayRole).toString());
        }
    });
    mpd_crop_selection_action_ = new QAction(QStringLiteral("Crop queue to selection"), this);
    mpd_crop_selection_action_->setObjectName(QStringLiteral("action-mpd-crop-selection"));
    connect(mpd_crop_selection_action_, &QAction::triggered, this,
            [this] { mpd_controller_->cropQueueToItems(selectedMpdQueueRows()); });

    mpd_priority_menu_ = new QMenu(QStringLiteral("Priority"), this);
    mpd_priority_menu_->setObjectName(QStringLiteral("bench-mpd-priority-menu"));
    mpd_priority_menu_->menuAction()->setObjectName(QStringLiteral("action-mpd-queue-priority"));
    auto* priority_group = new QActionGroup(mpd_priority_menu_);
    priority_group->setExclusive(true);
    const std::array priority_choices{
        std::pair{QStringLiteral("Normal"), 0},    std::pair{QStringLiteral("Low"), 64},
        std::pair{QStringLiteral("Medium"), 128},  std::pair{QStringLiteral("High"), 192},
        std::pair{QStringLiteral("Maximum"), 255},
    };
    for (const auto& [label, priority] : priority_choices) {
        auto* action =
            mpd_priority_menu_->addAction(QStringLiteral("%1 (%2)").arg(label).arg(priority));
        action->setObjectName(QStringLiteral("action-mpd-queue-priority-%1").arg(priority));
        action->setCheckable(true);
        action->setData(priority);
        priority_group->addAction(action);
        connect(action, &QAction::triggered, this, [this, priority] {
            mpd_controller_->setQueuePriority(selectedMpdQueueRows(), priority);
        });
    }

    mpd_replaygain_button_ = new QToolButton(statusBar());
    mpd_replaygain_button_->setObjectName(QStringLiteral("bench-mpd-replaygain"));
    mpd_replaygain_button_->setIcon(QIcon::fromTheme(
        QStringLiteral("view-media-equalizer"), style()->standardIcon(QStyle::SP_MediaVolume)));
    mpd_replaygain_button_->setText(QStringLiteral("RG: —"));
    mpd_replaygain_button_->setAccessibleName(QStringLiteral("MPD ReplayGain mode"));
    mpd_replaygain_button_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    mpd_replaygain_button_->setAutoRaise(true);
    mpd_replaygain_button_->setPopupMode(QToolButton::InstantPopup);
    auto* replaygain_menu = new QMenu(mpd_replaygain_button_);
    replaygain_menu->setObjectName(QStringLiteral("bench-mpd-replaygain-menu"));
    mpd_replaygain_group_ = new QActionGroup(replaygain_menu);
    mpd_replaygain_group_->setExclusive(true);
    const std::array modes{
        std::pair{QStringLiteral("Off"), QStringLiteral("off")},
        std::pair{QStringLiteral("Track"), QStringLiteral("track")},
        std::pair{QStringLiteral("Album"), QStringLiteral("album")},
        std::pair{QStringLiteral("Automatic"), QStringLiteral("auto")},
    };
    for (const auto& [label, value] : modes) {
        auto* action = replaygain_menu->addAction(label);
        action->setObjectName(QStringLiteral("action-mpd-replaygain-%1").arg(value));
        action->setCheckable(true);
        action->setData(value);
        mpd_replaygain_group_->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, value] { mpd_controller_->setReplayGainMode(value); });
    }
    mpd_replaygain_button_->setMenu(replaygain_menu);
    statusBar()->addPermanentWidget(mpd_replaygain_button_);
    refreshMpdStatusControls();
}

void BenchMainWindow::refreshMpdStatusControls() {
    if (mpd_repeat_action_ == nullptr) {
        return;
    }
    const auto visible = isMpdContext();
    const auto connected = mpd_controller_->connected();
    const auto command_ready = connected && !mpd_controller_->commandBusy();
    mpd_status_separator_->setVisible(visible);
    mpd_repeat_button_->setVisible(visible);
    mpd_repeat_action_->setVisible(visible);
    mpd_repeat_action_->setEnabled(connected);
    mpd_repeat_action_->setChecked(mpd_controller_->repeatEnabled());
    mpd_repeat_action_->setToolTip(
        QStringLiteral("Repeat: %1")
            .arg(mpd_controller_->repeatEnabled() ? QStringLiteral("On") : QStringLiteral("Off")));
    mpd_random_button_->setVisible(visible);
    mpd_random_action_->setVisible(visible);
    mpd_random_action_->setEnabled(connected);
    mpd_random_action_->setChecked(mpd_controller_->randomEnabled());
    mpd_random_action_->setToolTip(
        QStringLiteral("Random: %1")
            .arg(mpd_controller_->randomEnabled() ? QStringLiteral("On") : QStringLiteral("Off")));

    mpd_single_button_->setVisible(visible);
    mpd_single_action_->setEnabled(command_ready);
    mpd_single_action_->setChecked(mpd_controller_->singleMode() > 0);
    mpd_single_action_->setText(mpd_controller_->singleMode() == 2 ? QStringLiteral("1×")
                                                                   : QStringLiteral("1"));
    mpd_single_action_->setToolTip(
        QStringLiteral("Single: %1")
            .arg(mpd_controller_->singleMode() == 2   ? QStringLiteral("One-shot")
                 : mpd_controller_->singleMode() == 1 ? QStringLiteral("On")
                                                      : QStringLiteral("Off")));

    mpd_consume_button_->setVisible(visible);
    mpd_consume_action_->setEnabled(command_ready);
    mpd_consume_action_->setChecked(mpd_controller_->consumeMode() > 0);
    mpd_consume_action_->setText(mpd_controller_->consumeMode() == 2 ? QStringLiteral("C×")
                                                                     : QStringLiteral("C"));
    mpd_consume_action_->setToolTip(
        QStringLiteral("Consume: %1")
            .arg(mpd_controller_->consumeMode() == 2   ? QStringLiteral("One-shot")
                 : mpd_controller_->consumeMode() == 1 ? QStringLiteral("On")
                                                       : QStringLiteral("Off")));

    const auto replaygain_visible = visible && mpd_controller_->supportsReplayGain();
    mpd_replaygain_button_->setVisible(replaygain_visible);
    mpd_replaygain_button_->setEnabled(command_ready);
    const auto replaygain = mpd_controller_->replayGainMode();
    QString replaygain_label = QStringLiteral("Unavailable");
    for (auto* action : mpd_replaygain_group_->actions()) {
        const auto selected = action->data().toString() == replaygain;
        action->setChecked(selected);
        if (selected) {
            replaygain_label = action->text();
        }
    }
    mpd_replaygain_button_->setText(QStringLiteral("RG: %1").arg(replaygain_label));
    mpd_replaygain_button_->setToolTip(QStringLiteral("ReplayGain mode: %1").arg(replaygain_label));
    mpd_replaygain_button_->setAccessibleDescription(
        QStringLiteral("Current ReplayGain mode is %1; activate to choose another mode")
            .arg(replaygain_label));

    if (mpd_search_field_ != nullptr) {
        mpd_search_field_->setVisible(visible);
        mpd_search_field_->setEnabled(connected);
        mpd_search_field_->setToolTip(connected ? QStringLiteral("Search the MPD server library")
                                                : QStringLiteral("Connect to search MPD"));
        resizeMpdSearchField();
    }
    if (!visible && mpd_search_surface_ != nullptr) {
        closeMpdSearch(false);
    }
    if (mpd_search_more_button_ != nullptr) {
        const auto query = mpd_search_field_->text().trimmed();
        const auto more =
            mpd_controller_->hasMoreSearchResults() && query == mpd_controller_->lastSearchQuery();
        mpd_search_more_button_->setVisible(more);
        mpd_search_more_button_->setEnabled(more && command_ready);
        if (mpd_search_surface_->isVisible()) {
            mpd_search_status_->setText(mpd_controller_->libraryStatus());
        }
    }
}

void BenchMainWindow::buildMpdWorkspace() {
    mpd_controller_ = new quick::MpdProbeController(this);
    server_library_model_ = new ui::ServerLibraryTreeModel(this);
    server_library_model_->setArtworkEnabled(true);

    auto* queue_model = qobject_cast<quick::MpdQueueModel*>(mpd_controller_->queueModel());
    Q_ASSERT(queue_model != nullptr);
    queue_model->setArtworkEnabled(true);

    auto* view = new ui::QueueTableView(tabs_);
    mpd_queue_view_ = view;
    view->setObjectName(QStringLiteral("bench-mpd-queue"));
    view->setProperty("bench-mpd-queue", true);
    view->setAccessibleName(QStringLiteral("MPD Queue"));
    view->setModel(queue_model);
    view->setAlternatingRowColors(true);
    view->setShowGrid(false);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setWordWrap(false);
    view->setTextElideMode(Qt::ElideRight);
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
    view->setDragDropMode(QAbstractItemView::InternalMove);
    view->setDefaultDropAction(Qt::MoveAction);
    view->setActivateCallback([controller = mpd_controller_](const QModelIndex& index) {
        if (index.isValid()) {
            controller->playQueueItem(index.row());
        }
    });
    view->setReorderCallback(
        [controller = mpd_controller_](const QVariantList& rows, const int insertion_row) {
            controller->moveQueueItems(rows, insertion_row);
        });
    view->setExternalDropCallback([this](QAbstractItemView* source, const QVariantList&,
                                         const int insertion_row, const Qt::DropAction) {
        if (source != server_library_view_ || source->selectionModel() == nullptr) {
            return false;
        }
        QStringList uris;
        QSet<QString> seen;
        const auto indexes = source->selectionModel()->selectedRows(0);
        for (const auto& index : indexes) {
            for (const auto& track : server_library_model_->tracks(index)) {
                const auto uri = displayText(track.uri);
                if (!seen.contains(uri)) {
                    seen.insert(uri);
                    uris.push_back(uri);
                }
            }
        }
        if (uris.isEmpty()) {
            if (indexes.size() == 1 && server_library_model_->canFetchMore(indexes.front())) {
                pending_mpd_library_index_ = indexes.front();
                pending_mpd_library_action_ = MpdLibraryAction::insert;
                pending_mpd_library_insertion_row_ = insertion_row;
                server_library_view_->expand(indexes.front());
                server_library_model_->fetchMore(indexes.front());
                return true;
            }
            statusBar()->showMessage(QStringLiteral("This library entry contains no tracks"),
                                     3'000);
            return false;
        }
        mpd_controller_->addUrisAt(uris, insertion_row);
        return true;
    });
    connect(view, &QTableView::doubleClicked, this,
            [controller = mpd_controller_](const QModelIndex& index) {
                if (index.isValid()) {
                    controller->playQueueItem(index.row());
                }
            });
    connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] { refreshSelectionStatus(); });
    connect(queue_model, &QAbstractItemModel::modelReset, this,
            [this] { refreshSelectionStatus(); });
    connect(queue_model, &QAbstractItemModel::rowsInserted, this,
            [this] { refreshSelectionStatus(); });
    connect(queue_model, &QAbstractItemModel::rowsRemoved, this,
            [this] { refreshSelectionStatus(); });

    mpd_view_layout_ = defaultTrackViewLayout();
    applyTrackViewLayout(mpd_queue_view_, mpd_view_layout_, mpd_view_layout_);
    connect(view->horizontalHeader(), &QHeaderView::sectionMoved, this,
            [this](const int, const int, const int) {
                if (applying_track_view_layout_) {
                    return;
                }
                mpd_view_layout_ = captureTrackViewLayout(mpd_queue_view_, mpd_view_layout_);
                mpd_view_layout_persistence_protected_ = false;
                preserved_mpd_view_layout_.clear();
                schedulePersist();
                refreshTrackViewActions();
            });
    connect(view->horizontalHeader(), &QHeaderView::sectionResized, this,
            [this](const int, const int, const int) {
                if (applying_track_view_layout_) {
                    return;
                }
                mpd_view_layout_ = captureTrackViewLayout(mpd_queue_view_, mpd_view_layout_);
                mpd_view_layout_persistence_protected_ = false;
                preserved_mpd_view_layout_.clear();
                schedulePersist();
            });
    const auto queue_index = tabs_->addTab(view, QStringLiteral("MPD Queue"));
    tabs_->setTabToolTip(queue_index,
                         QStringLiteral("Authoritative queue on the connected MPD server"));
    if (auto* close = tabs_->tabBar()->tabButton(queue_index, QTabBar::RightSide)) {
        close->hide();
    }
    buildMpdSearch();

    server_library_view_ = new ui::ServerLibraryTreeView(source_stack_);
    server_library_view_->setObjectName(QStringLiteral("bench-mpd-library"));
    server_library_view_->setAccessibleName(QStringLiteral("MPD server library"));
    server_library_view_->setModel(server_library_model_);
    server_library_view_->setHeaderHidden(true);
    server_library_view_->setUniformRowHeights(false);
    server_library_view_->setIconSize(QSize{32, 32});
    server_library_view_->setIndentation(18);
    server_library_view_->setAnimated(true);
    server_library_view_->setExpandsOnDoubleClick(false);
    server_library_view_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    server_library_view_->setDragEnabled(true);
    server_library_view_->setDragDropMode(QAbstractItemView::DragOnly);
    server_library_view_->setDefaultDropAction(Qt::CopyAction);
    const std::array library_action_icons{
        QIcon::fromTheme(QStringLiteral("list-add"),
                         style()->standardIcon(QStyle::SP_DialogOpenButton)),
        QIcon::fromTheme(QStringLiteral("go-next"), style()->standardIcon(QStyle::SP_ArrowRight)),
        QIcon::fromTheme(QStringLiteral("media-playback-start"),
                         style()->standardIcon(QStyle::SP_MediaPlay)),
    };
    server_library_view_->setItemDelegate(
        new ui::ServerLibraryTreeDelegate(server_library_view_, library_action_icons));
    server_library_view_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(server_library_view_, &QWidget::customContextMenuRequested, this,
            &BenchMainWindow::showMpdLibraryContextMenu);
    server_library_view_->setActionCallback([this](const QModelIndex& index, const int action) {
        activateMpdLibraryAction(index, action);
    });
    source_stack_->addWidget(server_library_view_);

    connect(server_library_model_, &ui::ServerLibraryTreeModel::rootRequested, mpd_controller_,
            &quick::MpdProbeController::loadServerLibraryRoot);
    connect(server_library_model_, &ui::ServerLibraryTreeModel::branchRequested, mpd_controller_,
            &quick::MpdProbeController::loadServerLibraryBranch);
    connect(server_library_model_, &ui::ServerLibraryTreeModel::artworkRequested, mpd_controller_,
            &quick::MpdProbeController::loadServerLibraryArtwork);
    connect(queue_model, &quick::MpdQueueModel::artworkRequested, mpd_controller_,
            &quick::MpdProbeController::loadServerLibraryArtwork);
    connect(mpd_controller_, &quick::MpdProbeController::serverLibraryRootLoaded,
            server_library_model_, &ui::ServerLibraryTreeModel::acceptRoot);
    connect(mpd_controller_, &quick::MpdProbeController::serverLibraryBranchLoaded,
            server_library_model_, &ui::ServerLibraryTreeModel::acceptBranch);
    connect(mpd_controller_, &quick::MpdProbeController::serverLibraryArtworkLoaded, this,
            [this, queue_model](const quint64 token, const QByteArray& bytes) {
                auto* watcher = new QFutureWatcher<QImage>(this);
                connect(watcher, &QFutureWatcher<QImage>::finished, this,
                        [this, watcher, token, queue_model] {
                            const auto image = watcher->result();
                            watcher->deleteLater();
                            server_library_model_->acceptArtwork(token, image);
                            mpd_search_model_->acceptArtwork(token, image);
                            queue_model->acceptArtwork(token, image);
                        });
                watcher->setFuture(QtConcurrent::run([bytes] {
                    auto image = QImage::fromData(bytes);
                    if (!image.isNull()) {
                        image =
                            image.scaled(160, 160, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    }
                    return image;
                }));
            });
    connect(mpd_controller_, &quick::MpdProbeController::serverDatabaseChanged,
            server_library_model_, &ui::ServerLibraryTreeModel::reload);
    connect(server_library_model_, &ui::ServerLibraryTreeModel::browseError, this,
            [this](const QString& error) {
                pending_mpd_library_action_.reset();
                pending_mpd_library_index_ = QPersistentModelIndex{};
                pending_mpd_library_insertion_row_ = -1;
                server_library_view_->cancelPendingExpansions();
                statusBar()->showMessage(
                    QStringLiteral("Could not browse the MPD library: %1").arg(error), 5'000);
            });
    connect(server_library_model_, &QAbstractItemModel::rowsInserted, this,
            [this](const QModelIndex&, const int, const int) {
                QTimer::singleShot(0, this, [this] {
                    server_library_view_->completePendingExpansions();
                    completePendingMpdLibraryAction();
                });
            });
    connect(mpd_controller_, &quick::MpdProbeController::notificationRequested, this,
            [this](const QString& message) { statusBar()->showMessage(message, 5'000); });
    connect(mpd_controller_, &quick::MpdProbeController::searchFinished, this,
            &BenchMainWindow::finishMpdSearch);
    connect(mpd_controller_, &quick::MpdProbeController::stateChanged, this, [this] {
        const auto connected = mpd_controller_->connected();
        if (connected && !mpd_was_connected_) {
            server_library_model_->reload();
        }
        mpd_was_connected_ = connected;
        refreshActiveContext();
        refreshTransport();
        refreshSelectionStatus();
        refreshMpdStatusControls();
    });
    auto* output_model = mpd_controller_->outputModel();
    const auto refresh_outputs = [this] {
        if (isMpdContext() && device_menu_ != nullptr) {
            rebuildDeviceMenu();
        }
    };
    connect(output_model, &QAbstractItemModel::modelReset, this, refresh_outputs);
    connect(output_model, &QAbstractItemModel::rowsInserted, this,
            [refresh_outputs](const QModelIndex&, const int, const int) { refresh_outputs(); });
    connect(output_model, &QAbstractItemModel::rowsRemoved, this,
            [refresh_outputs](const QModelIndex&, const int, const int) { refresh_outputs(); });
    connect(output_model, &QAbstractItemModel::dataChanged, this,
            [refresh_outputs](const QModelIndex&, const QModelIndex&, const QList<int>&) {
                refresh_outputs();
            });
}

void BenchMainWindow::activateMpdLibraryAction(const QModelIndex& index, const int action) {
    const auto load_local = action == static_cast<int>(MpdLibraryAction::load_local);
    if (!index.isValid() ||
        (!load_local && (action < static_cast<int>(MpdLibraryAction::append) ||
                         action > static_cast<int>(MpdLibraryAction::replace)))) {
        return;
    }
    server_library_view_->setCurrentIndex(index);
    if (!mpd_controller_->connected()) {
        statusBar()->showMessage(QStringLiteral("Connect to MPD to use the server library"), 3'000);
        return;
    }
    if (server_library_model_->canFetchMore(index)) {
        pending_mpd_library_index_ = index;
        pending_mpd_library_action_ = static_cast<MpdLibraryAction>(action);
        server_library_view_->expand(index);
        server_library_model_->fetchMore(index);
        return;
    }

    const auto tracks = server_library_model_->tracks(index);
    if (tracks.empty()) {
        statusBar()->showMessage(QStringLiteral("This library entry contains no tracks"), 3'000);
        return;
    }
    QStringList uris;
    uris.reserve(static_cast<qsizetype>(tracks.size()));
    for (const auto& track : tracks) {
        uris.push_back(displayText(track.uri));
    }
    const auto requested = static_cast<MpdLibraryAction>(action);
    if (requested == MpdLibraryAction::load_local) {
        loadMpdUrisAsLocalFiles(uris);
        return;
    }
    if (requested == MpdLibraryAction::replace) {
        mpd_controller_->replaceQueueWithUris(uris);
    } else {
        mpd_controller_->addUris(uris, requested == MpdLibraryAction::next);
    }
}

void BenchMainWindow::completePendingMpdLibraryAction() {
    if (!pending_mpd_library_action_) {
        return;
    }
    if (!pending_mpd_library_index_.isValid()) {
        pending_mpd_library_action_.reset();
        return;
    }
    const auto index = QModelIndex{pending_mpd_library_index_};
    const auto requested = *pending_mpd_library_action_;
    pending_mpd_library_action_.reset();
    pending_mpd_library_index_ = QPersistentModelIndex{};
    if (requested == MpdLibraryAction::insert) {
        const auto tracks = server_library_model_->tracks(index);
        QStringList uris;
        uris.reserve(static_cast<qsizetype>(tracks.size()));
        for (const auto& track : tracks) {
            uris.push_back(displayText(track.uri));
        }
        if (!uris.isEmpty() && pending_mpd_library_insertion_row_ >= 0) {
            mpd_controller_->addUrisAt(uris, pending_mpd_library_insertion_row_);
        }
        pending_mpd_library_insertion_row_ = -1;
        return;
    }
    pending_mpd_library_insertion_row_ = -1;
    activateMpdLibraryAction(index, static_cast<int>(requested));
}

void BenchMainWindow::showMpdLibraryContextMenu(const QPoint& position) {
    if (mpd_library_context_menu_ == nullptr) {
        return;
    }
    const auto index = server_library_view_->indexAt(position).siblingAtColumn(0);
    if (!index.isValid()) {
        return;
    }
    server_library_view_->selectionModel()->setCurrentIndex(
        index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    const auto target = QPersistentModelIndex{index};
    const auto command_ready = mpd_controller_->connected() && !mpd_controller_->commandBusy();
    mpd_library_context_menu_->clear();
    const std::array actions{
        std::pair{QStringLiteral("Append to live queue"), QStringLiteral("list-add")},
        std::pair{QStringLiteral("Insert next in live queue"), QStringLiteral("go-next")},
        std::pair{QStringLiteral("Replace queue and play"), QStringLiteral("media-playback-start")},
    };
    for (int action = 0; action < static_cast<int>(actions.size()); ++action) {
        const auto& [label, icon] = actions[static_cast<std::size_t>(action)];
        auto* command = mpd_library_context_menu_->addAction(QIcon::fromTheme(icon), label);
        command->setObjectName(QStringLiteral("action-mpd-library-%1").arg(action));
        command->setEnabled(command_ready);
        connect(command, &QAction::triggered, this, [this, target, action] {
            if (target.isValid()) {
                activateMpdLibraryAction(target, action);
            }
        });
    }
    mpd_library_context_menu_->addSeparator();
    auto* load_local = mpd_library_context_menu_->addAction(
        QIcon::fromTheme(QStringLiteral("folder-open")), QStringLiteral("Load as local files"));
    load_local->setObjectName(QStringLiteral("action-mpd-library-load-local"));
    load_local->setEnabled(command_ready);
    connect(load_local, &QAction::triggered, this, [this, target] {
        if (target.isValid()) {
            activateMpdLibraryAction(target, static_cast<int>(MpdLibraryAction::load_local));
        }
    });
    if (server_library_model_->hasChildren(index)) {
        mpd_library_context_menu_->addSeparator();
        auto* expand = mpd_library_context_menu_->addAction(server_library_view_->isExpanded(index)
                                                                ? QStringLiteral("Collapse")
                                                                : QStringLiteral("Expand"));
        connect(expand, &QAction::triggered, this, [this, target] {
            if (target.isValid()) {
                server_library_view_->setExpanded(target,
                                                  !server_library_view_->isExpanded(target));
            }
        });
    }
    mpd_library_context_menu_->popup(server_library_view_->viewport()->mapToGlobal(position));
}

void BenchMainWindow::buildMpdSearch() {
    auto* field = new MpdSearchLineEdit(tabs_);
    mpd_search_field_ = field;
    field->setObjectName(QStringLiteral("bench-mpd-search"));
    field->setAccessibleName(QStringLiteral("Search MPD library"));
    field->setClearButtonEnabled(true);
    field->setPlaceholderText(QStringLiteral("Search MPD…"));
    field->setMinimumWidth(0);
    field->setMaximumWidth(340);
    field->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    field->addAction(QIcon::fromTheme(QStringLiteral("edit-find"),
                                      style()->standardIcon(QStyle::SP_FileDialogContentsView)),
                     QLineEdit::LeadingPosition);
    field->show();
    field->raise();
    resizeMpdSearchField();

    auto* surface = new QFrame(this);
    surface->setObjectName(QStringLiteral("bench-mpd-search-surface"));
    surface->setFrameShape(QFrame::StyledPanel);
    surface->setFrameShadow(QFrame::Raised);
    surface->setAutoFillBackground(true);
    surface->hide();
    mpd_search_surface_ = surface;
    auto* layout = new QVBoxLayout(surface);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    mpd_search_model_ = new quick::MpdSearchResultModel(surface);
    // The controller answers unsupported/disconnected requests with an empty
    // result, so keeping this enabled also guarantees that a later reconnect
    // cannot leave the search model permanently stuck on placeholders.
    mpd_search_model_->setArtworkEnabled(true);
    mpd_search_model_->setAlbumPlaceholder(QIcon::fromTheme(
        QStringLiteral("media-optical-audio"), style()->standardIcon(QStyle::SP_FileIcon)));
    auto* results = new MpdSearchTableView(surface);
    mpd_search_view_ = results;
    results->setObjectName(QStringLiteral("bench-mpd-search-results"));
    results->setAccessibleName(QStringLiteral("MPD library search results"));
    results->setAccessibleDescription(QStringLiteral(
        "Use Up and Down for results, Left and Right for queue actions, Enter to activate, "
        "Control Enter to replace the queue, and Escape to close search."));
    results->setModel(mpd_search_model_);
    results->setSearchField(field);
    results->setSelectionBehavior(QAbstractItemView::SelectRows);
    results->setSelectionMode(QAbstractItemView::SingleSelection);
    results->setEditTriggers(QAbstractItemView::NoEditTriggers);
    results->setAlternatingRowColors(false);
    results->setShowGrid(false);
    results->setWordWrap(false);
    results->setTextElideMode(Qt::ElideRight);
    results->setMouseTracking(true);
    results->setIconSize(QSize{24, 24});
    results->verticalHeader()->hide();
    results->verticalHeader()->setDefaultSectionSize(30);
    results->verticalHeader()->setMinimumSectionSize(30);
    results->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    results->horizontalHeader()->hide();
    results->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    results->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    results->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    results->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Interactive);
    results->horizontalHeader()->setMinimumSectionSize(18);
    results->setColumnWidth(0, 150);
    results->setColumnWidth(2, 190);
    results->setColumnWidth(3, 72);
    for (int column = quick::MpdSearchResultModel::first_action_column;
         column < quick::MpdSearchResultModel::column_count; ++column) {
        results->horizontalHeader()->setSectionResizeMode(column, QHeaderView::Fixed);
        results->horizontalHeader()->resizeSection(column, 28);
    }
    auto* album_delegate = new MpdSearchAlbumDelegate(results);
    album_delegate->setObjectName(QStringLiteral("bench-mpd-search-album-delegate"));
    results->setItemDelegateForColumn(0, album_delegate);
    results->setItemDelegateForColumn(
        4,
        new MpdSearchActionDelegate(QIcon::fromTheme(QStringLiteral("list-add"),
                                                     style()->standardIcon(QStyle::SP_ArrowRight)),
                                    results));
    results->setItemDelegateForColumn(
        5, new MpdSearchActionDelegate(
               QIcon::fromTheme(QStringLiteral("go-next"),
                                style()->standardIcon(QStyle::SP_ArrowForward)),
               results));
    results->setItemDelegateForColumn(
        6,
        new MpdSearchActionDelegate(QIcon::fromTheme(QStringLiteral("media-playback-start"),
                                                     style()->standardIcon(QStyle::SP_MediaPlay)),
                                    results));
    layout->addWidget(results, 1);

    auto* footer = new QWidget(surface);
    auto* footer_layout = new QHBoxLayout(footer);
    footer_layout->setContentsMargins(6, 2, 4, 2);
    footer_layout->setSpacing(6);
    mpd_search_status_ =
        new QLabel(QStringLiteral("Type at least two characters to search"), footer);
    mpd_search_status_->setObjectName(QStringLiteral("bench-mpd-search-status"));
    mpd_search_status_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    footer_layout->addWidget(mpd_search_status_, 1);
    mpd_search_more_button_ = new QToolButton(footer);
    mpd_search_more_button_->setObjectName(QStringLiteral("bench-mpd-search-more"));
    mpd_search_more_button_->setText(QStringLiteral("More results"));
    mpd_search_more_button_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    mpd_search_more_button_->setAutoRaise(true);
    mpd_search_more_button_->hide();
    footer_layout->addWidget(mpd_search_more_button_);
    layout->addWidget(footer);

    mpd_search_timer_ = new QTimer(this);
    mpd_search_timer_->setSingleShot(true);
    mpd_search_timer_->setInterval(180);
    connect(field, &QLineEdit::textEdited, this, [this] {
        if (!isMpdContext()) {
            return;
        }
        mpd_search_surface_->show();
        mpd_search_surface_->raise();
        positionMpdSearchSurface();
        mpd_search_timer_->start();
    });
    connect(mpd_search_timer_, &QTimer::timeout, this, &BenchMainWindow::previewMpdSearch);
    connect(mpd_search_more_button_, &QToolButton::clicked, mpd_controller_,
            &quick::MpdProbeController::continueSearch);
    connect(results, &QTableView::clicked, this, [this](const QModelIndex& index) {
        if (index.column() >= quick::MpdSearchResultModel::first_action_column) {
            activateMpdSearchResult(
                index.row(), index.column() - quick::MpdSearchResultModel::first_action_column);
        }
    });
    connect(results, &QTableView::doubleClicked, this, [this](const QModelIndex& index) {
        if (index.column() < quick::MpdSearchResultModel::first_action_column) {
            activateMpdSearchResult(index.row(), 0);
        }
    });
    results->setActionCallback([this](const int row, const MpdSearchQueueAction action) {
        activateMpdSearchResult(row, static_cast<int>(action));
    });
    results->setCloseCallback([this] { closeMpdSearch(); });
    field->setResultFocusCallback([results] { results->focusFirstResult(); });
    field->setCloseCallback([this] { closeMpdSearch(); });
    field->setActionCallback(
        [results](const MpdSearchQueueAction action) { results->activateDefault(action); });
    connect(mpd_search_model_, &quick::MpdSearchResultModel::artworkRequested, mpd_controller_,
            &quick::MpdProbeController::loadServerLibraryArtwork);

    auto* focus_search = new QShortcut(QKeySequence(QStringLiteral("Ctrl+L")), this);
    connect(focus_search, &QShortcut::activated, this, [this] {
        tabs_->setCurrentWidget(mpd_queue_view_);
        mpd_search_surface_->show();
        mpd_search_surface_->raise();
        positionMpdSearchSurface();
        mpd_search_field_->setFocus();
        mpd_search_field_->selectAll();
    });

    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget*) {
        QTimer::singleShot(0, this, [this] {
            if (mpd_search_surface_ == nullptr || !mpd_search_surface_->isVisible()) {
                return;
            }
            auto* focused = QApplication::focusWidget();
            const auto inside_field = focused == mpd_search_field_;
            const auto inside_surface =
                focused != nullptr &&
                (focused == mpd_search_surface_ || mpd_search_surface_->isAncestorOf(focused));
            if (!inside_field && !inside_surface) {
                closeMpdSearch(false);
            }
        });
    });
    connect(qApp, &QGuiApplication::applicationStateChanged, this,
            [this](const Qt::ApplicationState state) {
                if (state != Qt::ApplicationActive) {
                    closeMpdSearch(false);
                }
            });
}

void BenchMainWindow::previewMpdSearch() {
    if (!isMpdContext() || mpd_search_field_ == nullptr) {
        return;
    }
    const auto query = mpd_search_field_->text().trimmed();
    if (query.size() < 2) {
        mpd_search_model_->replaceTracks({});
        syncMpdSearchView();
    }
    mpd_controller_->searchLibrary(query);
    mpd_search_status_->setText(mpd_controller_->libraryStatus());
    mpd_search_more_button_->hide();
}

void BenchMainWindow::finishMpdSearch(const QString& query, const bool success) {
    if (mpd_search_field_ == nullptr || query != mpd_search_field_->text().trimmed()) {
        return;
    }
    if (success) {
        std::vector<mpd::Track> tracks;
        if (const auto* source =
                qobject_cast<const quick::MpdQueueModel*>(mpd_controller_->libraryModel())) {
            tracks = source->tracksSnapshot();
        }
        mpd_search_model_->replaceSearchResults(mpd_controller_->libraryAlbumsSnapshot(),
                                                std::move(tracks));
    } else {
        mpd_search_model_->replaceTracks({});
    }
    syncMpdSearchView();
    mpd_search_status_->setText(success ? mpd_controller_->libraryStatus()
                                        : QStringLiteral("Search did not complete"));
    const auto more = success && mpd_controller_->hasMoreSearchResults() &&
                      query == mpd_controller_->lastSearchQuery();
    mpd_search_more_button_->setVisible(more);
    mpd_search_more_button_->setEnabled(more && !mpd_controller_->commandBusy());
}

void BenchMainWindow::syncMpdSearchView() {
    mpd_search_view_->clearSpans();
    for (const auto row : mpd_search_model_->sectionRows()) {
        mpd_search_view_->setSpan(row, 0, 1, quick::MpdSearchResultModel::column_count);
    }
    const auto current = mpd_search_view_->currentIndex();
    if (!current.isValid() || mpd_search_model_->kindAt(current.row()) ==
                                  quick::MpdSearchResultModel::ResultKind::section) {
        const auto first = mpd_search_model_->firstResultRow();
        if (first >= 0) {
            mpd_search_view_->setCurrentIndex(mpd_search_model_->index(first, 1));
        }
    }
}

void BenchMainWindow::activateMpdSearchResult(const int row, const int action) {
    if (mpd_search_model_ == nullptr || row < 0 ||
        mpd_search_model_->kindAt(row) == quick::MpdSearchResultModel::ResultKind::section) {
        return;
    }
    const auto requested = static_cast<MpdSearchQueueAction>(action);
    if (const auto album = mpd_search_model_->albumAt(row)) {
        const auto mode = requested == MpdSearchQueueAction::append ? quick::QueueAddMode::append
                          : requested == MpdSearchQueueAction::next ? quick::QueueAddMode::next
                                                                    : quick::QueueAddMode::replace;
        mpd_controller_->addAlbum(*album, mode);
        return;
    }
    const auto uris = mpd_search_model_->urisAt(row);
    if (uris.isEmpty()) {
        return;
    }
    if (requested == MpdSearchQueueAction::append) {
        mpd_controller_->addUris(uris, false);
    } else if (requested == MpdSearchQueueAction::next) {
        mpd_controller_->addUris(uris, true);
    } else {
        mpd_controller_->replaceQueueWithUris(uris);
    }
}

void BenchMainWindow::closeMpdSearch(const bool restore_queue_focus) {
    if (mpd_search_surface_ == nullptr) {
        return;
    }
    mpd_search_surface_->hide();
    if (restore_queue_focus && isMpdContext() && mpd_queue_view_ != nullptr) {
        mpd_queue_view_->setFocus(Qt::ShortcutFocusReason);
    }
}

void BenchMainWindow::resizeMpdSearchField() {
    if (tabs_ == nullptr || mpd_search_field_ == nullptr) {
        return;
    }
    // This is an explicit child overlay instead of QTabWidget's corner-widget
    // slot. The latter retains stale full-width geometry when the Track Lists
    // panel moves between persisted splitter/tab layouts on some styles.
    const auto frame = tabs_->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, nullptr, tabs_);
    const auto available = std::max(1, tabs_->width() - frame * 2);
    const auto proportional = std::max(1, tabs_->width() * 2 / 5);
    const auto field_width = std::min({340, available, proportional});
    const auto bar = tabs_->tabBar()->geometry();
    const auto field_height = std::min(mpd_search_field_->sizeHint().height(), bar.height());
    mpd_search_field_->setGeometry(tabs_->width() - frame - field_width,
                                   bar.top() + std::max(0, (bar.height() - field_height) / 2),
                                   field_width, field_height);
    mpd_search_field_->raise();

    // Reserve the overlay's horizontal area so tab scroll buttons appear
    // before tab labels can slide beneath the field.
    tabs_->tabBar()->setMaximumWidth(mpd_search_field_->isVisible()
                                         ? std::max(1, tabs_->width() - field_width - frame * 2 - 4)
                                         : QWIDGETSIZE_MAX);
}

void BenchMainWindow::positionMpdSearchSurface() {
    if (mpd_search_surface_ == nullptr || mpd_search_field_ == nullptr) {
        return;
    }
    const auto anchor = mpd_search_field_->mapTo(
        this, QPoint{mpd_search_field_->width(), mpd_search_field_->height()});
    const auto maximum_width = std::max(520, width() - 24);
    const auto surface_width = std::clamp(width() * 3 / 5, 520, maximum_width);
    const auto x =
        std::clamp(anchor.x() - surface_width, 12, std::max(12, width() - 12 - surface_width));
    const auto y = anchor.y() + 4;
    const auto available_bottom = statusBar() != nullptr ? statusBar()->geometry().top() : height();
    const auto surface_height = std::min(480, std::max(220, available_bottom - y - 12));
    mpd_search_surface_->setGeometry(x, y, surface_width, surface_height);
}

void BenchMainWindow::openMpdConnectionDialog() {
    if (auto* existing = findChild<ui::MpdConnectionDialog*>(); existing != nullptr) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return;
    }
    if (mpd_queue_view_ != nullptr) {
        tabs_->setCurrentWidget(mpd_queue_view_);
    }
    auto* dialog = new ui::MpdConnectionDialog(this, mpd_profiles_, mpd_controller_->profileId());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(
        dialog, &ui::MpdConnectionDialog::connectionRequested, this,
        [this](const QString& profile_id, const QString& profile_name, const QString& host,
               const int port, const QString& password, const QString& music_root,
               const bool auto_connect) {
            const auto parsed_id = core::StableId::parse(profile_id.toStdString());
            if (!parsed_id) {
                statusBar()->showMessage(
                    QStringLiteral("Connection profile has an invalid identity"), 5'000);
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
            if (auto_connect) {
                for (auto& profile : mpd_profiles_) {
                    profile.auto_connect = false;
                }
            }
            const auto existing_profile =
                std::ranges::find(mpd_profiles_, updated.id, &persistence::ConnectionProfile::id);
            if (existing_profile == mpd_profiles_.end()) {
                mpd_profiles_.push_back(std::move(updated));
            } else {
                *existing_profile = std::move(updated);
            }
            const auto profiles = mpd_profiles_;
            if (persistence_ == nullptr) {
                mpd_controller_->probeProfile(profile_id, host, port, password, music_root);
                return;
            }
            persistence_->saveProfiles(profiles, [this, profiles, profile_id, host, port, password,
                                                  music_root](const QString& error) {
                if (!error.isEmpty()) {
                    statusBar()->showMessage(error, 5'000);
                    return;
                }
                mpd_profiles_ = profiles;
                mpd_controller_->probeProfile(profile_id, host, port, password, music_root);
            });
        });
    dialog->show();
}

void BenchMainWindow::autoConnectMpd() {
    const auto profile =
        std::ranges::find(mpd_profiles_, true, &persistence::ConnectionProfile::auto_connect);
    if (profile == mpd_profiles_.end()) {
        return;
    }
    const auto music_root = profile->local_music_root
                                ? QFile::decodeName(QByteArray{
                                      profile->local_music_root->data(),
                                      static_cast<qsizetype>(profile->local_music_root->size())})
                                : QString{};
    mpd_controller_->probeProfile(QString::fromStdString(profile->id.to_string()),
                                  displayText(profile->host), static_cast<int>(profile->port),
                                  QString{}, music_root);
}

QVariantList BenchMainWindow::selectedMpdQueueRows() const {
    QVariantList rows;
    if (mpd_queue_view_ == nullptr || mpd_queue_view_->selectionModel() == nullptr) {
        return rows;
    }
    auto selected = mpd_queue_view_->selectionModel()->selectedRows(0);
    std::ranges::sort(selected, {}, &QModelIndex::row);
    rows.reserve(selected.size());
    for (const auto& index : selected) {
        rows.push_back(index.row());
    }
    return rows;
}

QStringList BenchMainWindow::selectedMpdQueueUris() const {
    QStringList uris;
    const auto* model = qobject_cast<const quick::MpdQueueModel*>(mpd_queue_view_->model());
    if (model == nullptr) {
        return uris;
    }
    for (const auto& value : selectedMpdQueueRows()) {
        if (const auto uri = model->uriAt(value.toInt())) {
            uris.push_back(displayText(*uri));
        }
    }
    return uris;
}

// Resolves MPD URIs below the configured music folder and opens the hits
// as ordinary local files in a fresh tab (ADR-0112) — from there tagging,
// conversion, and ReplayGain behave exactly like any local selection.
void BenchMainWindow::loadMpdUrisAsLocalFiles(const QStringList& uris) {
    if (uris.isEmpty()) {
        return;
    }
    const QSettings settings;
    const auto root =
        settings.value(QLatin1String(SettingsDialog::music_root_key)).toString().trimmed();
    if (root.isEmpty()) {
        statusBar()->showMessage(
            QStringLiteral("Set the MPD music folder in Edit → Settings… first"), 5'000);
        return;
    }
    const QDir root_directory{root};
    std::vector<std::string> paths;
    paths.reserve(static_cast<std::size_t>(uris.size()));
    int missing = 0;
    for (const auto& uri : uris) {
        const auto local = root_directory.filePath(uri);
        if (!QFileInfo::exists(local)) {
            ++missing;
            continue;
        }
        const auto encoded = QFile::encodeName(local);
        paths.emplace_back(encoded.constData(), static_cast<std::size_t>(encoded.size()));
    }
    if (paths.empty()) {
        statusBar()->showMessage(
            QStringLiteral("None of the selected tracks exist under %1").arg(root), 5'000);
        return;
    }
    addListTab(
        persistence::ListDocument{
            .id = core::StableId::random(),
            .kind = persistence::ListKind::scratch,
            .name = "Local files",
            .pinned = false,
            .dirty = false,
            .items = {},
        },
        true);
    openLocalPaths(std::move(paths));
    if (missing > 0) {
        statusBar()->showMessage(QStringLiteral("%1 track%2 not found under %3")
                                     .arg(missing)
                                     .arg(missing == 1 ? QString{} : QStringLiteral("s"))
                                     .arg(root),
                                 5'000);
    }
    schedulePersist();
}

void BenchMainWindow::refreshMpdPriorityMenu() {
    if (mpd_priority_menu_ == nullptr) {
        return;
    }
    const auto rows = selectedMpdQueueRows();
    const auto ready = !rows.isEmpty() && mpd_controller_->connected() &&
                       !mpd_controller_->commandBusy() &&
                       mpd_controller_->supportsCommand(QStringLiteral("prioid"));
    mpd_priority_menu_->setEnabled(ready);
    std::optional<unsigned> selected_priority;
    bool priorities_match = !rows.isEmpty();
    for (const auto& value : rows) {
        const auto priority = mpd_queue_view_->model()
                                  ->index(value.toInt(), 0)
                                  .data(quick::MpdQueueModel::PriorityRole);
        const auto numeric = priority.isValid() ? priority.toUInt() : 0U;
        if (!selected_priority) {
            selected_priority = numeric;
        } else if (*selected_priority != numeric) {
            priorities_match = false;
            break;
        }
    }
    for (auto* action : mpd_priority_menu_->actions()) {
        const QSignalBlocker blocker{action};
        action->setChecked(priorities_match && selected_priority &&
                           action->data().toUInt() == *selected_priority);
    }
}

void BenchMainWindow::refreshMpdTransport() {
    const auto connected = mpd_controller_->connected();
    const auto command_ready = connected && !mpd_controller_->commandBusy();
    const auto has_queue = mpd_controller_->queueCount() > 0;
    const auto active = mpd_controller_->playing();
    previous_action_->setEnabled(command_ready && has_queue);
    next_action_->setEnabled(command_ready && has_queue);
    play_pause_action_->setEnabled(command_ready);
    play_pause_action_->setText(active ? QStringLiteral("Pause") : QStringLiteral("Play"));
    play_pause_action_->setIcon(
        style()->standardIcon(active ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
    stop_action_->setEnabled(command_ready && (active || mpd_controller_->paused()));

    if (connected) {
        now_playing_->setText(mpd_controller_->nowPlayingTitle());
        now_playing_context_->setText(mpd_controller_->nowPlayingDetail());
        now_playing_->setToolTip(QStringLiteral("MPD · %1").arg(mpd_controller_->status()));
        now_playing_context_->setToolTip(mpd_controller_->details());
    } else {
        now_playing_->setText(QStringLiteral("MPD not connected"));
        now_playing_context_->setText(QStringLiteral("File → Connect to MPD…"));
        now_playing_->setToolTip(mpd_controller_->status());
        now_playing_context_->setToolTip(mpd_controller_->details());
    }

    const auto position_ms = mpd_controller_->elapsedMs();
    const auto duration_ms = mpd_controller_->durationMs();
    elapsed_->setText(formatTime(position_ms));
    duration_->setText(formatTime(duration_ms));
    const auto bounded = std::clamp<qint64>(duration_ms, 0, std::numeric_limits<int>::max());
    seek_->setEnabled(command_ready && bounded > 0);
    seek_->setRange(0, static_cast<int>(bounded));
    if (!seeking_) {
        const QSignalBlocker blocker{seek_};
        seek_->setValue(
            static_cast<int>(std::clamp<qint64>(position_ms, 0, std::numeric_limits<int>::max())));
    }
    volume_->setEnabled(command_ready && mpd_controller_->volume() >= 0);
    if (!changing_volume_ && mpd_controller_->volume() >= 0) {
        const QSignalBlocker blocker{volume_};
        volume_->setValue(mpd_controller_->volume());
    }
    device_button_->setEnabled(connected);
    device_button_->setToolTip(
        QStringLiteral("MPD output: %1").arg(mpd_controller_->activeOutputName()));
    device_button_->setAccessibleDescription(mpd_controller_->activeOutputName());
}

// "Go to Artist/Album": reveal the queue row's artist (and optionally its
// album) in the MPD library tree, fetching lazy levels as needed — the
// in-app navigation Cantata offered.
void BenchMainWindow::goToMpdLibraryEntry(const QString& artist, const QString& album) {
    if (artist.isEmpty() || server_library_model_ == nullptr || server_library_view_ == nullptr) {
        statusBar()->showMessage(QStringLiteral("This queue entry names no library artist"), 4'000);
        return;
    }
    if (server_library_model_->rowCount() == 0) {
        auto connections =
            std::make_shared<std::pair<QMetaObject::Connection, QMetaObject::Connection>>();
        const auto resume = [this, connections, artist, album] {
            if (server_library_model_->rowCount() == 0) {
                return;
            }
            disconnect(connections->first);
            disconnect(connections->second);
            completeMpdLibraryGoTo(artist, album);
        };
        connections->first =
            connect(server_library_model_, &QAbstractItemModel::rowsInserted, this, resume);
        connections->second =
            connect(server_library_model_, &QAbstractItemModel::modelReset, this, resume);
        server_library_model_->reload();
        return;
    }
    completeMpdLibraryGoTo(artist, album);
}

void BenchMainWindow::completeMpdLibraryGoTo(const QString& artist, const QString& album) {
    QModelIndex artist_index;
    for (int row = 0; row < server_library_model_->rowCount(); ++row) {
        const auto candidate = server_library_model_->index(row, 0);
        if (candidate.data(ui::ServerLibraryTreeModel::QueryValueRole).toString() == artist) {
            artist_index = candidate;
            break;
        }
    }
    if (!artist_index.isValid()) {
        statusBar()->showMessage(
            QStringLiteral("\u201C%1\u201D is not in the library tree").arg(artist), 4'000);
        return;
    }
    if (album.isEmpty()) {
        server_library_view_->expand(artist_index);
        server_library_view_->setCurrentIndex(artist_index);
        server_library_view_->scrollTo(artist_index, QAbstractItemView::PositionAtCenter);
        return;
    }
    if (server_library_model_->canFetchMore(artist_index)) {
        const QPersistentModelIndex persistent{artist_index};
        auto connection = std::make_shared<QMetaObject::Connection>();
        *connection =
            connect(server_library_model_, &QAbstractItemModel::rowsInserted, this,
                    [this, connection, persistent, artist, album](const QModelIndex& parent) {
                        if (parent != QModelIndex{persistent}) {
                            return;
                        }
                        disconnect(*connection);
                        completeMpdLibraryGoTo(artist, album);
                    });
        server_library_model_->fetchMore(artist_index);
        return;
    }
    server_library_view_->expand(artist_index);
    // Album-level grouping values are definition-specific composites
    // (release id, or album|date); match the plain album against the
    // grouping value, its album prefix, or the displayed label.
    for (int row = 0; row < server_library_model_->rowCount(artist_index); ++row) {
        const auto candidate = server_library_model_->index(row, 0, artist_index);
        const auto query_value =
            candidate.data(ui::ServerLibraryTreeModel::QueryValueRole).toString();
        const auto label = candidate.data(Qt::DisplayRole).toString();
        const auto matches = query_value == album ||
                             query_value.startsWith(album + QLatin1Char('|')) || label == album ||
                             label.startsWith(album + QStringLiteral(" ("));
        if (matches) {
            server_library_view_->setCurrentIndex(candidate);
            server_library_view_->scrollTo(candidate, QAbstractItemView::PositionAtCenter);
            return;
        }
    }
    server_library_view_->setCurrentIndex(artist_index);
    server_library_view_->scrollTo(artist_index, QAbstractItemView::PositionAtCenter);
    statusBar()->showMessage(
        QStringLiteral("\u201C%1\u201D has no album \u201C%2\u201D in the library tree")
            .arg(artist, album),
        4'000);
}

} // namespace trackknife::bench
