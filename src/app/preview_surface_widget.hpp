#pragma once

#include <QImage>
#include <QRect>
#include <QSize>
#include <QWidget>

class QEnterEvent;
class QEvent;
class QPaintEvent;
class QMouseEvent;
class QResizeEvent;
class QToolButton;
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

signals:
    void surface_clicked();
    void play_pause_requested();
    void stop_requested();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    [[nodiscard]] QRect content_rect() const;
    void refresh_overlay_visibility();
    void layout_overlay_controls();
    void refresh_control_icons();

    QImage frame_image_{};
    QString placeholder_headline_{"PREVIEW OFFLINE"};
    QString placeholder_message_{};
    QWidget *controls_container_{nullptr};
    QToolButton *play_pause_button_{nullptr};
    QToolButton *stop_button_{nullptr};
    bool controls_enabled_{false};
    bool playing_{false};
    bool hovered_{false};
};
