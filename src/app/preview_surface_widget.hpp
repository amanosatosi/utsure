#pragma once

#include <QImage>
#include <QRect>
#include <QWidget>

class QPaintEvent;

class PreviewSurfaceWidget final : public QWidget {
public:
    explicit PreviewSurfaceWidget(QWidget *parent = nullptr);

    void set_placeholder(const QString &headline, const QString &message = QString{});
    void set_frame_image(const QImage &image);
    void clear_frame();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    [[nodiscard]] QRect content_rect() const;

    QImage frame_image_{};
    QString placeholder_headline_{"PREVIEW OFFLINE"};
    QString placeholder_message_{};
};
