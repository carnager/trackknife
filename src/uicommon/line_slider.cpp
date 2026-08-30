// SPDX-License-Identifier: GPL-3.0-only

#include "uicommon/line_slider.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QStyle>

#include <algorithm>

namespace trackknife::ui {

LineSlider::LineSlider(QWidget* parent) : QSlider(Qt::Horizontal, parent) {}

QSize LineSlider::sizeHint() const {
    auto size = QSlider::sizeHint();
    size.setHeight(18);
    return size;
}

void LineSlider::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);

    auto track_color = palette().color(QPalette::Text);
    track_color.setAlphaF(isEnabled() ? 0.28F : 0.16F);
    auto progress_color = palette().color(QPalette::Highlight);
    progress_color.setAlphaF(isEnabled() ? 1.0F : 0.35F);

    constexpr qreal thickness = 4.0;
    const auto area = contentsRect();
    const QRectF track{static_cast<qreal>(area.left()), (height() - thickness) / 2.0,
                       static_cast<qreal>(area.width()), thickness};
    painter.fillRect(track, track_color);

    const auto range = maximum() - minimum();
    if (range <= 0) {
        return;
    }
    const auto fraction = static_cast<qreal>(value() - minimum()) / range;
    painter.fillRect(QRectF{track.left(), track.top(), track.width() * fraction, thickness},
                     progress_color);
}

void LineSlider::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QSlider::mousePressEvent(event);
        return;
    }
    setSliderDown(true);
    setPositionFromMouse(event->position().x());
    event->accept();
}

void LineSlider::mouseMoveEvent(QMouseEvent* event) {
    if (!isSliderDown()) {
        QSlider::mouseMoveEvent(event);
        return;
    }
    setPositionFromMouse(event->position().x());
    event->accept();
}

void LineSlider::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !isSliderDown()) {
        QSlider::mouseReleaseEvent(event);
        return;
    }
    setPositionFromMouse(event->position().x());
    setSliderDown(false);
    event->accept();
}

void LineSlider::setPositionFromMouse(const qreal x) {
    const auto area = contentsRect();
    const auto span = std::max(1, area.width() - 1);
    const auto position = std::clamp(qRound(x) - area.left(), 0, span);
    const auto upside_down = invertedAppearance() != (layoutDirection() == Qt::RightToLeft);
    setSliderPosition(
        QStyle::sliderValueFromPosition(minimum(), maximum(), position, span, upside_down));
}

} // namespace trackknife::ui
