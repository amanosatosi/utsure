#pragma once

#include <QImage>
#include <QRect>
#include <QSize>
#include <QWidget>

class QHBoxLayout;
class QKeyEvent;
class QPaintEvent;
class QMouseEvent;
class QResizeEvent;
class QWidget;

class PreviewSurfaceWidget final : public QWidget {
    Q_OBJECT

public:
    explicit PreviewSurfaceWidget(QWidget *parent = nullptr);

    void set_placeholder(const QString &headline, const QString &message = QString{});
    void set_frame_image(const QImage &image);
    void clear_frame();
    void set_controls_enabled(bool enabled);
    void set_playing(bool playing);

    [[nodiscard]] bool has_frame() const noexcept;
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;
    [[nodiscard]] bool hasHeightForWidth() const override;
    [[nodiscard]] int heightForWidth(int width) const override;
    [[nodiscard]] QHBoxLayout *top_right_overlay_layout() const noexcept;

signals:
    void surface_clicked();
    void frame_step_requested(int direction);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    [[nodiscard]] QRect content_rect() const;
    void refresh_overlay_visibility();
    void layout_overlay_controls();

    QImage frame_image_{};
    QString placeholder_headline_{"PREVIEW OFFLINE"};
    QString placeholder_message_{};
    QWidget *top_right_overlay_container_{nullptr};
    QHBoxLayout *top_right_overlay_layout_{nullptr};
    bool controls_enabled_{false};
};
