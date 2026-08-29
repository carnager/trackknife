// SPDX-License-Identifier: GPL-3.0-only

#include "ui/queue_table_view.hpp"

#include "ui/queue_item_delegate.hpp"

#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>

#include <algorithm>
#include <utility>

namespace trackknife::ui {

QueueTableView::QueueTableView(QWidget* parent) : QTableView(parent) {}

void QueueTableView::setReorderCallback(std::function<void(const QVariantList&, int)> callback) {
    reorder_callback_ = std::move(callback);
}

void QueueTableView::setExternalDropCallback(
    std::function<bool(QTableView*, const QVariantList&, int, Qt::DropAction)> callback) {
    external_drop_callback_ = std::move(callback);
}

void QueueTableView::setActivateCallback(std::function<void(const QModelIndex&)> callback) {
    activate_callback_ = std::move(callback);
}

void QueueTableView::setLocalUrlDropCallback(
    std::function<bool(const QList<QUrl>&, int)> callback) {
    local_url_drop_callback_ = std::move(callback);
}

int QueueTableView::dropInsertionRow(const QPoint& position,
                                     const DropIndicatorPosition indicator) const {
    const auto hovered = indexAt(position);
    if (!hovered.isValid()) {
        return model()->rowCount();
    }
    auto row = hovered.row();
    if (indicator == QAbstractItemView::BelowItem) {
        ++row;
    }
    return row;
}

void QueueTableView::keyPressEvent(QKeyEvent* event) {
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
        event->modifiers() == Qt::NoModifier && currentIndex().isValid() && activate_callback_) {
        activate_callback_(currentIndex());
        event->accept();
        return;
    }
    QTableView::keyPressEvent(event);
}

void QueueTableView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        const auto index = indexAt(event->position().toPoint());
        const auto* queue_delegate = qobject_cast<const QueueItemDelegate*>(itemDelegate());
        const auto item_rect = visualRect(index);
        if (queue_delegate != nullptr &&
            queue_delegate->isAlbumHeaderHit(index,
                                             event->position().toPoint().y() - item_rect.top())) {
            const auto [first, last] = queue_delegate->albumRowRange(index);
            const QItemSelection album{model()->index(first, 0),
                                       model()->index(last, model()->columnCount() - 1)};
            selectionModel()->select(album, QItemSelectionModel::ClearAndSelect |
                                                QItemSelectionModel::Rows);
            selectionModel()->setCurrentIndex(model()->index(first, 1),
                                              QItemSelectionModel::NoUpdate);
            event->accept();
            return;
        }
    }
    QTableView::mousePressEvent(event);
}

bool QueueTableView::handlesDrag(const QDropEvent* event) const {
    if (local_url_drop_callback_ != nullptr && event->mimeData()->hasUrls()) {
        return true;
    }
    if (event->source() != this && external_drop_callback_ != nullptr &&
        qobject_cast<QTableView*>(event->source()) != nullptr) {
        return true;
    }
    return event->source() == this && reorder_callback_ != nullptr;
}

// The base implementations run first so Qt keeps tracking and painting the
// drop indicator; drags this view handles itself are then force-accepted
// regardless of the model's own drop verdict.
void QueueTableView::dragEnterEvent(QDragEnterEvent* event) {
    QTableView::dragEnterEvent(event);
    if (handlesDrag(event)) {
        event->acceptProposedAction();
    }
}

void QueueTableView::dragMoveEvent(QDragMoveEvent* event) {
    QTableView::dragMoveEvent(event);
    if (handlesDrag(event)) {
        event->acceptProposedAction();
    }
}

void QueueTableView::dropEvent(QDropEvent* event) {
    if (local_url_drop_callback_ != nullptr && event->mimeData()->hasUrls()) {
        const auto insertion_row =
            dropInsertionRow(event->position().toPoint(), dropIndicatorPosition());
        if (local_url_drop_callback_(event->mimeData()->urls(), insertion_row)) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
            return;
        }
    }
    auto* source_view = qobject_cast<QTableView*>(event->source());
    if (source_view != nullptr && source_view != this && external_drop_callback_ != nullptr) {
        auto selected = source_view->selectionModel()->selectedRows(0);
        std::ranges::sort(selected, {}, &QModelIndex::row);
        QVariantList rows;
        rows.reserve(selected.size());
        for (const auto& index : selected) {
            rows.push_back(index.row());
        }
        const auto insertion_row =
            dropInsertionRow(event->position().toPoint(), dropIndicatorPosition());
        auto action = event->dropAction();
        if (action == Qt::IgnoreAction) {
            action = event->proposedAction();
        }
        if (external_drop_callback_(source_view, rows, insertion_row, action)) {
            event->setDropAction(action == Qt::MoveAction ? Qt::MoveAction : Qt::CopyAction);
            event->accept();
            return;
        }
    }
    if (event->source() != this || !reorder_callback_ || selectionModel() == nullptr) {
        QTableView::dropEvent(event);
        return;
    }
    auto selected = selectionModel()->selectedRows(0);
    std::ranges::sort(selected, {}, &QModelIndex::row);
    QVariantList rows;
    rows.reserve(selected.size());
    for (const auto& index : selected) {
        rows.push_back(index.row());
    }

    reorder_callback_(rows, dropInsertionRow(event->position().toPoint(), dropIndicatorPosition()));
    event->setDropAction(Qt::MoveAction);
    event->accept();
}

} // namespace trackknife::ui
