// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QTableView>

#include <functional>

class QDropEvent;
class QUrl;

namespace trackknife::ui {

// Track-list view with album-header selection, Enter activation, and typed
// drag/drop callbacks. Drops are executed by the owning window's callbacks —
// reorder within the view, copy/move from another view, or local URL ingestion
// — never by the model.
class QueueTableView final : public QTableView {
  public:
    // Qt keeps DropIndicatorPosition protected; the insertion-row mapping is
    // part of this view's public contract (and its tests), so re-expose it.
    using QAbstractItemView::AboveItem;
    using QAbstractItemView::BelowItem;
    using QAbstractItemView::DropIndicatorPosition;
    using QAbstractItemView::OnItem;
    using QAbstractItemView::OnViewport;

    explicit QueueTableView(QWidget* parent);

    void setReorderCallback(std::function<void(const QVariantList&, int)> callback);
    void setExternalDropCallback(
        std::function<bool(QTableView*, const QVariantList&, int, Qt::DropAction)> callback);
    void setActivateCallback(std::function<void(const QModelIndex&)> callback);
    void setLocalUrlDropCallback(std::function<bool(const QList<QUrl>&, int)> callback);

    // Maps a drop position to the row content should be inserted before.
    // Above/on the hovered row inserts before it, below inserts after it, and
    // empty space appends.
    [[nodiscard]] int dropInsertionRow(const QPoint& position,
                                       DropIndicatorPosition indicator) const;

  protected:
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

  private:
    [[nodiscard]] bool handlesDrag(const QDropEvent* event) const;

    std::function<void(const QVariantList&, int)> reorder_callback_;
    std::function<bool(QTableView*, const QVariantList&, int, Qt::DropAction)>
        external_drop_callback_;
    std::function<bool(const QList<QUrl>&, int)> local_url_drop_callback_;
    std::function<void(const QModelIndex&)> activate_callback_;
};

} // namespace trackknife::ui
