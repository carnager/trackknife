// SPDX-License-Identifier: GPL-3.0-only

#include "bench/track_list_find_bar.hpp"

#include "bench/local_list_model.hpp"
#include "quick/mpd_queue_model.hpp"
#include "trackknife/core/unicode.hpp"

#include <QAction>
#include <QElapsedTimer>
#include <QEvent>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QTableView>
#include <QTimer>
#include <QtConcurrentRun>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace trackknife::bench {
namespace {

constexpr std::size_t row_text_limit = 64U * 1024U;
constexpr std::size_t batch_text_limit = 256U * 1024U;
constexpr std::size_t batch_row_limit = 128U;

struct Candidate {
    int row{};
    // Last field is the authority-owned source: only local paths are escaped.
    std::vector<std::string> fields;
    bool local_path{false};
    bool oversized{false};
};

} // namespace

TrackListFindBar::TrackListFindBar(QWidget* parent) : QToolBar(tr("Find in list"), parent) {
    setObjectName(QStringLiteral("bench-list-find-bar"));
    setMovable(false);
    setFloatable(false);
    query_ = new QLineEdit(this);
    query_->setObjectName(QStringLiteral("bench-list-find-query"));
    query_->setPlaceholderText(tr("Find in current list"));
    query_->setAccessibleName(tr("Find in current list"));
    query_->setToolTip(
        tr("Search title, artist, album, album artist, date, track number, or source path/URI"));
    query_->setClearButtonEnabled(true);
    query_->setMaxLength(1024);
    query_->setMinimumWidth(180);
    query_->installEventFilter(this);
    addWidget(query_);
    auto* previous = addAction(tr("Previous"));
    previous->setObjectName(QStringLiteral("action-list-find-previous-button"));
    previous->setToolTip(tr("Previous match (Shift+Enter or Shift+F3)"));
    connect(previous, &QAction::triggered, this, [this] { findNext(true); });
    auto* next = addAction(tr("Next"));
    next->setObjectName(QStringLiteral("action-list-find-next-button"));
    next->setToolTip(tr("Next match (Enter or F3)"));
    connect(next, &QAction::triggered, this, [this] { findNext(); });
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("bench-list-find-status"));
    status_->setContentsMargins(8, 0, 8, 0);
    addWidget(status_);
    auto* close = addAction(tr("Close"));
    close->setObjectName(QStringLiteral("action-list-find-close"));
    close->setToolTip(tr("Close find (Escape)"));
    close->setShortcut(QKeySequence(Qt::Key_Escape));
    close->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(close, &QAction::triggered, this, &TrackListFindBar::dismiss);
    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(150);
    connect(debounce_, &QTimer::timeout, this, [this] { start(false, true); });
    connect(query_, &QLineEdit::textChanged, this, [this] {
        cancel();
        status_->clear();
        if (!query_->text().isEmpty()) {
            status_->setText(tr("Searching…"));
            debounce_->start();
        }
    });
    pool_.setMaxThreadCount(1);
    connect(&watcher_, &QFutureWatcherBase::finished, this, &TrackListFindBar::finish);
    hide();
}

TrackListFindBar::~TrackListFindBar() {
    cancel();
    pool_.waitForDone();
}

void TrackListFindBar::cancel() {
    ++generation_;
    searching_ = false;
    cancellation_.request_cancellation();
    debounce_->stop();
}

void TrackListFindBar::setView(QTableView* view) {
    if (view_ == view)
        return;
    cancel();
    hide();
    for (const auto& connection : connections_)
        disconnect(connection);
    connections_.clear();
    if (view_)
        view_->removeEventFilter(this);
    view_ = view;
    model_ = view == nullptr ? nullptr : view->model();
    if (!qobject_cast<LocalListModel*>(model_.data()) &&
        !qobject_cast<quick::MpdQueueModel*>(model_.data()))
        model_ = nullptr;
    status_->clear();
    if (model_ == nullptr)
        return;
    view_->installEventFilter(this);
    const auto changed = [this] { invalidate(); };
    connections_.push_back(connect(model_, &QAbstractItemModel::rowsInserted, this, changed));
    connections_.push_back(connect(model_, &QAbstractItemModel::rowsRemoved, this, changed));
    connections_.push_back(connect(model_, &QAbstractItemModel::rowsMoved, this, changed));
    connections_.push_back(connect(model_, &QAbstractItemModel::modelReset, this, changed));
    connections_.push_back(connect(model_, &QAbstractItemModel::layoutChanged, this, changed));
    connections_.push_back(
        connect(model_, &QAbstractItemModel::dataChanged, this,
                [this](const QModelIndex&, const QModelIndex&, const QList<int>& roles) {
                    if (roles.empty() || roles.contains(Qt::DisplayRole))
                        invalidate();
                }));
    const auto selection_changed = [this] {
        if (searching_) {
            cancel();
            status_->setText(tr("Selection changed — search again"));
        }
    };
    connections_.push_back(connect(view_->selectionModel(), &QItemSelectionModel::currentChanged,
                                   this, selection_changed));
    connections_.push_back(connect(view_->selectionModel(), &QItemSelectionModel::selectionChanged,
                                   this, selection_changed));
    connections_.push_back(connect(model_, &QObject::destroyed, this, [this] {
        cancel();
        hide();
    }));
}

void TrackListFindBar::invalidate() {
    cancel();
    if (!isHidden() && !query_->text().isEmpty())
        status_->setText(tr("List changed — search again"));
}

void TrackListFindBar::open() {
    if (!view_ || !model_)
        return;
    show();
    query_->setFocus(Qt::ShortcutFocusReason);
    query_->selectAll();
}

void TrackListFindBar::dismiss() {
    cancel();
    hide();
    if (view_)
        view_->setFocus(Qt::ShortcutFocusReason);
}

void TrackListFindBar::findNext(const bool backwards) {
    if (!model_)
        return;
    if (query_->text().isEmpty()) {
        open();
        return;
    }
    show();
    start(backwards, false);
}

bool TrackListFindBar::eventFilter(QObject* watched, QEvent* event) {
    if (watched == view_ && !isHidden() && event->type() == QEvent::KeyPress &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        dismiss();
        return true;
    }
    if (watched == query_ && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            dismiss();
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            findNext(key->modifiers().testFlag(Qt::ShiftModifier));
            return true;
        }
    }
    return QToolBar::eventFilter(watched, event);
}

void TrackListFindBar::start(const bool backwards, const bool include_current) {
    cancel();
    if (!view_ || !model_ || isHidden() || query_->text().isEmpty())
        return;
    total_ = model_->rowCount();
    if (total_ == 0) {
        status_->setText(tr("List is empty"));
        return;
    }
    direction_ = backwards ? -1 : 1;
    cursor_ = view_->currentIndex().row();
    wrapped_ = false;
    if (cursor_ < 0) {
        cursor_ = backwards ? total_ - 1 : 0;
    } else if (!include_current) {
        cursor_ += direction_;
        if (cursor_ < 0 || cursor_ >= total_) {
            cursor_ = backwards ? total_ - 1 : 0;
            wrapped_ = true;
        }
    }
    visited_ = 0;
    cancellation_ = core::CancellationSource{};
    searching_ = true;
    status_->setText(tr("Searching…"));
    pump();
}

void TrackListFindBar::pump() {
    if (!searching_ || worker_busy_ || !model_ || !view_)
        return;
    if (visited_ == total_) {
        searching_ = false;
        status_->setText(tr("No matches"));
        return;
    }
    std::vector<Candidate> candidates;
    candidates.reserve(batch_row_limit);
    std::size_t batch_bytes = 0;
    QElapsedTimer capture_time;
    capture_time.start();
    const auto* local = qobject_cast<LocalListModel*>(model_.data());
    const auto* remote = qobject_cast<quick::MpdQueueModel*>(model_.data());
    while (visited_ < total_ && candidates.size() < batch_row_limit && capture_time.elapsed() < 4) {
        std::vector<std::string_view> fields;
        bool oversized = false;
        if (local) {
            const auto& row = local->rows()[static_cast<std::size_t>(cursor_)];
            fields = {row.title, row.artist,       row.album,   row.album_artist,
                      row.date,  row.track_number, row.raw_path};
        } else if (remote) {
            const auto* track = remote->trackAt(cursor_);
            if (!track)
                return;
            // Bound even a pathological number of empty/unrelated server tags.
            oversized = track->metadata.fields().size() > 1024U;
            if (!oversized) {
                for (const auto& tag : track->metadata.fields()) {
                    constexpr std::array<std::string_view, 6> names{
                        "Title", "Artist", "Album", "AlbumArtist", "Date", "Track"};
                    const auto matching_name = [&tag](const std::string_view name) {
                        return name.size() == tag.name.size() &&
                               std::ranges::equal(name, tag.name, [](const char a, const char b) {
                                   const auto lower = [](const char c) {
                                       return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
                                   };
                                   return lower(a) == lower(b);
                               });
                    };
                    if (std::ranges::any_of(names, matching_name))
                        fields.push_back(tag.value);
                }
            }
            fields.push_back(track->uri);
        }
        std::size_t bytes = 0;
        for (const auto field : fields) {
            if (field.size() > row_text_limit - bytes) {
                oversized = true;
                break;
            }
            bytes += field.size();
        }
        if (!candidates.empty() && bytes > batch_text_limit - batch_bytes)
            break;
        Candidate candidate{
            .row = cursor_, .fields = {}, .local_path = local != nullptr, .oversized = oversized};
        if (!oversized) {
            candidate.fields.reserve(fields.size());
            for (const auto field : fields)
                candidate.fields.emplace_back(field);
        }
        candidates.push_back(std::move(candidate));
        batch_bytes += bytes;
        ++visited_;
        // Do not span the wrap boundary in a batch: its feedback belongs
        // only to matches actually found on the far side of that boundary.
        cursor_ += direction_;
        if (cursor_ < 0 || cursor_ >= total_)
            break;
    }
    const auto generation = generation_;
    const auto cancellation = cancellation_.token();
    auto query = query_->text().toUtf8().toStdString();
    worker_busy_ = true;
    watcher_.setFuture(
        QtConcurrent::run(&pool_, [candidates = std::move(candidates), query = std::move(query),
                                   generation, cancellation] {
            BatchResult result{.generation = generation, .row = -1, .error = {}};
            const auto needle = core::unicodeSimpleLower(query);
            if (!needle) {
                result.error = QStringLiteral("Invalid search text");
                return result;
            }
            for (const auto& candidate : candidates) {
                if (cancellation.is_cancellation_requested())
                    return result;
                if (candidate.oversized) {
                    result.error =
                        QStringLiteral(
                            "Search stopped: track %1 exceeds find limits (64 KiB / 1024 tags)")
                            .arg(candidate.row + 1);
                    return result;
                }
                for (std::size_t field = 0; field < candidate.fields.size(); ++field) {
                    const auto text = candidate.local_path && field + 1 == candidate.fields.size()
                                          ? core::escape_raw_path(candidate.fields[field])
                                          : candidate.fields[field];
                    const auto haystack = core::unicodeSimpleLower(text);
                    if (!haystack) {
                        result.error =
                            QStringLiteral("Search stopped: invalid metadata text at track %1")
                                .arg(candidate.row + 1);
                        return result;
                    }
                    if (haystack->find(*needle) != std::string::npos) {
                        result.row = candidate.row;
                        return result;
                    }
                }
            }
            return result;
        }));
}

void TrackListFindBar::finish() {
    const auto result = watcher_.result();
    worker_busy_ = false;
    if (result.generation == generation_ && searching_ && model_ && view_) {
        if (!result.error.isEmpty()) {
            searching_ = false;
            status_->setText(result.error);
        } else if (result.row >= 0) {
            searching_ = false;
            const auto index = model_->index(result.row, ui::track_title_column);
            view_->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect |
                                                                QItemSelectionModel::Rows);
            view_->scrollTo(index, QAbstractItemView::EnsureVisible);
            status_->setText(wrapped_
                                 ? tr("Wrapped · Track %1 of %2").arg(result.row + 1).arg(total_)
                                 : tr("Track %1 of %2").arg(result.row + 1).arg(total_));
        } else {
            if (cursor_ < 0 || cursor_ >= total_) {
                cursor_ = direction_ < 0 ? total_ - 1 : 0;
                wrapped_ = true;
            }
            status_->setText(tr("Searching… %1 of %2").arg(visited_).arg(total_));
        }
    }
    if (searching_)
        QTimer::singleShot(0, this, &TrackListFindBar::pump);
}

} // namespace trackknife::bench
