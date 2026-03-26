#pragma once

#include <QRectF>
#include <QSize>
#include <QWidget>

class QMouseEvent;
class QPaintEvent;

class TrimTimelineWidget final : public QWidget {
    Q_OBJECT

public:
    explicit TrimTimelineWidget(QWidget *parent = nullptr);

    [[nodiscard]] qint64 duration_us() const noexcept;
    [[nodiscard]] qint64 current_time_us() const noexcept;
    [[nodiscard]] qint64 trim_in_us() const noexcept;
    [[nodiscard]] qint64 trim_out_us() const noexcept;

    void set_duration_us(qint64 duration_us);
    void set_current_time_us(qint64 current_time_us);
    void set_trim_range_us(qint64 trim_in_us, qint64 trim_out_us);

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

signals:
    void seek_requested(qint64 time_us);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private:
    [[nodiscard]] QRectF track_rect() const;
    [[nodiscard]] qreal position_for_time(qint64 time_us) const;
    [[nodiscard]] qint64 time_for_position(qreal x) const;
    void emit_seek_for_position(qreal x);

    qint64 duration_us_{10000000};
    qint64 current_time_us_{0};
    qint64 trim_in_us_{0};
    qint64 trim_out_us_{10000000};
};
