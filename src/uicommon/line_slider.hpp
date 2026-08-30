// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QSlider>

class QMouseEvent;
class QPaintEvent;

namespace trackknife::ui {

// Compact slider shared by both application shells. It paints only the
// remaining rail and its filled value, leaving focus indication to the
// surrounding keyboard navigation instead of drawing a native handle/frame.
class LineSlider final : public QSlider {
  public:
    explicit LineSlider(QWidget* parent = nullptr);

    [[nodiscard]] QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

  private:
    void setPositionFromMouse(qreal x);
};

} // namespace trackknife::ui
