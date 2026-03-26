#include "preview_surface_widget.hpp"

#include <QPainter>
#include <QPaintEvent>
#include <QRect>
#include <QSize>
#include <QSizePolicy>

namespace {

constexpr int kSurfaceMargin = 10;

}  // namespace

PreviewSurfaceWidget::PreviewSurfaceWidget(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void PreviewSurfaceWidget::set_placeholder(const QString &headline, const QString &message) {
    frame_image_ = QImage{};
    placeholder_headline_ = headline;
    placeholder_message_ = message;
    update();
}

void PreviewSurfaceWidget::set_frame_image(const QImage &image) {
    frame_image_ = image;
    update();
}

void PreviewSurfaceWidget::clear_frame() {
    frame_image_ = QImage{};
    update();
}

void PreviewSurfaceWidget::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.fillRect(rect(), QColor("#111111"));

    const QRect target_rect = content_rect();
    if (!frame_image_.isNull()) {
        const QSize scaled_size = frame_image_.size().scaled(target_rect.size(), Qt::KeepAspectRatio);
        const QRect image_rect(
            QPoint(
                target_rect.left() + ((target_rect.width() - scaled_size.width()) / 2),
                target_rect.top() + ((target_rect.height() - scaled_size.height()) / 2)
            ),
            scaled_size
        );
        painter.drawImage(image_rect, frame_image_);
        return;
    }

    painter.setPen(QColor("#eeeeee"));
    QFont headline_font = painter.font();
    headline_font.setPointSize(16);
    headline_font.setBold(true);
    painter.setFont(headline_font);

    QRect headline_rect = target_rect;
    if (!placeholder_message_.trimmed().isEmpty()) {
        headline_rect.adjust(0, -16, 0, -12);
    }
    painter.drawText(headline_rect, Qt::AlignCenter | Qt::TextWordWrap, placeholder_headline_);

    if (!placeholder_message_.trimmed().isEmpty()) {
        painter.setPen(QColor("#bbbbbb"));
        QFont message_font = painter.font();
        message_font.setPointSize(11);
        message_font.setBold(false);
        painter.setFont(message_font);
        QRect message_rect = target_rect.adjusted(12, (target_rect.height() / 2) - 4, -12, -8);
        painter.drawText(message_rect, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, placeholder_message_);
    }
}

QRect PreviewSurfaceWidget::content_rect() const {
    return rect().adjusted(kSurfaceMargin, kSurfaceMargin, -kSurfaceMargin, -kSurfaceMargin);
}
