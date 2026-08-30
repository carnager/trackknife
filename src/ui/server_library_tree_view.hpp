// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QIcon>
#include <QPersistentModelIndex>
#include <QStyledItemDelegate>
#include <QTreeView>

#include <array>
#include <functional>
#include <vector>

namespace trackknife::ui {

// Shared server-library presentation used by both the compatibility client and
// Trackbench's MPD authority. The three inline actions are append, add next,
// and replace-and-play, in that order.
class ServerLibraryTreeView final : public QTreeView {
  public:
    explicit ServerLibraryTreeView(QWidget* parent = nullptr);

    void setActionCallback(std::function<void(const QModelIndex&, int)> callback);

    [[nodiscard]] QModelIndex hoverIndex() const;
    [[nodiscard]] int hoverAction() const noexcept;
    void completePendingExpansions();
    void cancelPendingExpansions();

    [[nodiscard]] static QRect actionRect(const QRect& row_rect, int action);

  protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;
    bool viewportEvent(QEvent* event) override;
    void startDrag(Qt::DropActions supported_actions) override;

  private:
    void toggleBranch(const QModelIndex& index);
    [[nodiscard]] int actionAt(const QModelIndex& index, const QPoint& position) const;

    QPersistentModelIndex hover_index_;
    QPersistentModelIndex pressed_index_;
    QPoint pressed_position_;
    std::vector<QPersistentModelIndex> pending_expansions_;
    int hover_action_{-1};
    int pressed_action_{-1};
    bool pressed_expanded_{false};
    bool drag_started_{false};
    std::function<void(const QModelIndex&, int)> action_callback_;
};

class ServerLibraryTreeDelegate final : public QStyledItemDelegate {
  public:
    ServerLibraryTreeDelegate(ServerLibraryTreeView* view, std::array<QIcon, 3> action_icons);

    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const override;
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;

  private:
    ServerLibraryTreeView* view_;
    std::array<QIcon, 3> action_icons_;
};

} // namespace trackknife::ui
