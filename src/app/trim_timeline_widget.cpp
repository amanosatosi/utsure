#include "trim_timeline_widget.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>

#include <algorithm>

namespace {

constexpr auto kTrackHeight = 30.0;
constexpr auto kTrackMargin = 8.0;
constexpr auto kHandleWidth = 3.0;
constexpr auto kTickSpacing = 50.0;

qint64 clamp_time(const qint64 value, const qint64 duration_us) {
    return std::clamp<qint64>(value, 0, std::max<qint64>(duration_us, 0));
}

}  // namespace

TrimTimelineWidget::TrimTimelineWidget(QWidget *parent) : QWidget(parent) {
    setMinimumHeight(46);
    setMouseTracking(true);
}

qint64 TrimTimelineWidget::duration_us() const noexcept {
    return duration_us_;
}

qint64 TrimTimelineWidget::current_time_us() const noexcept {
    return current_time_us_;
}

qint64 TrimTimelineWidget::trim_in_us() const noexcept {
    return trim_in_us_;
}

qint64 TrimTimelineWidget::trim_out_us() const noexcept {
    return trim_out_us_;
}

void TrimTimelineWidget::set_duration_us(const qint64 duration_us) {
    duration_us_ = std::max<qint64>(duration_us, 1);
    trim_in_us_ = clamp_time(trim_in_us_, duration_us_);
    trim_out_us_ = clamp_time(std::max(trim_out_us_, trim_in_us_), duration_us_);
    current_time_us_ = clamp_time(current_time_us_, duration_us_);
    update();
}

void TrimTimelineWidget::set_current_time_us(const qint64 current_time_us) {
    current_time_us_ = clamp_time(current_time_us, duration_us_);
    update();
}

void TrimTimelineWidget::set_trim_range_us(qint64 trim_in_us, qint64 trim_out_us) {
    trim_in_us = clamp_time(trim_in_us, duration_us_);
    trim_out_us = clamp_time(trim_out_us, duration_us_);
    if (trim_out_us < trim_in_us) {
        std::swap(trim_in_us, trim_out_us);
    }

    trim_in_us_ = trim_in_us;
    trim_out_us_ = trim_out_us;
    current_time_us_ = std::clamp(current_time_us_, trim_in_us_, duration_us_);
    update();
}

QSize TrimTimelineWidget::sizeHint() const {
    return QSize(480, 46);
}

QSize TrimTimelineWidget::minimumSizeHint() const {
    return QSize(220, 46);
}

void TrimTimelineWidget::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF rect = track_rect();
    painter.setPen(QPen(QColor("#cfcfcf"), 1.0));
    painter.setBrush(QColor("#ffffff"));
    painter.drawRoundedRect(rect, 6.0, 6.0);

    painter.save();
    painter.setClipRect(rect);
    painter.setPen(QPen(QColor(0, 0, 0, 20), 1.0));
    for (qreal x = rect.left(); x < rect.right(); x += kTickSpacing) {
        painter.drawLine(QPointF(x, rect.top()), QPointF(x, rect.bottom()));
    }
    painter.restore();

    const qreal in_x = position_for_time(trim_in_us_);
    const qreal out_x = position_for_time(trim_out_us_);
    const qreal current_x = position_for_time(current_time_us_);

    const QRectF range_rect(in_x, rect.top(), std::max<qreal>(out_x - in_x, 0.0), rect.height());
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 62, 165, 30));
    painter.drawRect(range_rect);

    painter.setBrush(QColor("#ff3ea5"));
    painter.drawRect(QRectF(in_x - (kHandleWidth / 2.0), rect.top(), kHandleWidth, rect.height()));
    painter.setBrush(QColor("#19b7ff"));
    painter.drawRect(QRectF(out_x - (kHandleWidth / 2.0), rect.top(), kHandleWidth, rect.height()));

    painter.setBrush(QColor("#111111"));
    painter.drawRect(QRectF(current_x - 1.0, rect.top(), 2.0, rect.height()));

    QPolygonF head{};
    head << QPointF(current_x, rect.top() - 6.0)
         << QPointF(current_x - 6.0, rect.top())
         << QPointF(current_x + 6.0, rect.top());
    painter.drawPolygon(head);
}

void TrimTimelineWidget::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        emit_seek_for_position(event->position().x());
    }

    QWidget::mousePressEvent(event);
}

void TrimTimelineWidget::mouseMoveEvent(QMouseEvent *event) {
    if (event->buttons().testFlag(Qt::LeftButton)) {
        emit_seek_for_position(event->position().x());
    }

    QWidget::mouseMoveEvent(event);
}

QRectF TrimTimelineWidget::track_rect() const {
    return QRectF(
        kTrackMargin,
        (static_cast<qreal>(height()) - kTrackHeight) / 2.0,
        std::max<qreal>(width() - (kTrackMargin * 2.0), 10.0),
        kTrackHeight
    );
}

qreal TrimTimelineWidget::position_for_time(const qint64 time_us) const {
    const QRectF rect = track_rect();
    if (duration_us_ <= 0) {
        return rect.left();
    }

    const auto ratio = static_cast<qreal>(clamp_time(time_us, duration_us_)) /
        static_cast<qreal>(duration_us_);
    return rect.left() + (ratio * rect.width());
}

qint64 TrimTimelineWidget::time_for_position(const qreal x) const {
    const QRectF rect = track_rect();
    if (duration_us_ <= 0 || rect.width() <= 0.0) {
        return 0;
    }

    const auto ratio = std::clamp((x - rect.left()) / rect.width(), 0.0, 1.0);
    return static_cast<qint64>(ratio * static_cast<qreal>(duration_us_));
}

void TrimTimelineWidget::emit_seek_for_position(const qreal x) {
    emit seek_requested(time_for_position(x));
}
