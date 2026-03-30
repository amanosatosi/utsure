#include "preview_surface_widget.hpp"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QRect>
#include <QResizeEvent>
#include <QSize>
#include <QSizePolicy>

#include <algorithm>

namespace {

constexpr int kSurfaceMargin = 10;
constexpr int kOverlayMargin = 14;
constexpr int kDefaultPreviewWidth = 568;
constexpr int kDefaultPreviewHeight = 320;
constexpr int kMinimumPreviewWidth = 288;
constexpr int kMinimumPreviewHeight = 162;

}  // namespace

PreviewSurfaceWidget::PreviewSurfaceWidget(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);

    QSizePolicy size_policy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    size_policy.setHeightForWidth(true);
    setSizePolicy(size_policy);

    top_right_overlay_container_ = new QWidget(this);
    auto *top_right_layout = new QHBoxLayout(top_right_overlay_container_);
    top_right_layout->setContentsMargins(0, 0, 0, 0);
    top_right_layout->setSpacing(0);
    top_right_overlay_layout_ = top_right_layout;

    refresh_overlay_visibility();
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

void PreviewSurfaceWidget::set_controls_enabled(const bool enabled) {
    controls_enabled_ = enabled;
    refresh_overlay_visibility();
}

void PreviewSurfaceWidget::set_playing(const bool /*playing*/) {}

bool PreviewSurfaceWidget::has_frame() const noexcept {
    return !frame_image_.isNull();
}

QSize PreviewSurfaceWidget::sizeHint() const {
    return QSize(kDefaultPreviewWidth, kDefaultPreviewHeight);
}

QSize PreviewSurfaceWidget::minimumSizeHint() const {
    return QSize(kMinimumPreviewWidth, kMinimumPreviewHeight);
}

bool PreviewSurfaceWidget::hasHeightForWidth() const {
    return true;
}

int PreviewSurfaceWidget::heightForWidth(const int width) const {
    if (width <= 0) {
        return kMinimumPreviewHeight;
    }

    return std::max(kMinimumPreviewHeight, static_cast<int>((static_cast<qreal>(width) * 9.0) / 16.0));
}

QHBoxLayout *PreviewSurfaceWidget::top_right_overlay_layout() const noexcept {
    return top_right_overlay_layout_;
}

void PreviewSurfaceWidget::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.fillRect(rect(), QColor("#050508"));

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

    painter.setPen(QColor("#f4f4f8"));
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
        painter.setPen(QColor("#8b8b99"));
        QFont message_font = painter.font();
        message_font.setPointSize(11);
        message_font.setBold(false);
        painter.setFont(message_font);
        QRect message_rect = target_rect.adjusted(12, (target_rect.height() / 2) - 4, -12, -8);
        painter.drawText(message_rect, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, placeholder_message_);
    }
}

void PreviewSurfaceWidget::mousePressEvent(QMouseEvent *event) {
    setFocus(Qt::MouseFocusReason);

    if (event->button() == Qt::LeftButton) {
        const QWidget *clicked_child = childAt(event->position().toPoint());
        if (clicked_child == nullptr || clicked_child == this) {
            emit surface_clicked();
        }
        event->accept();
        return;
    }

    QWidget::mousePressEvent(event);
}

void PreviewSurfaceWidget::keyPressEvent(QKeyEvent *event) {
    if (controls_enabled_ && event != nullptr) {
        if (event->key() == Qt::Key_Left) {
            emit frame_step_requested(-1);
            event->accept();
            return;
        }

        if (event->key() == Qt::Key_Right) {
            emit frame_step_requested(1);
            event->accept();
            return;
        }
    }

    QWidget::keyPressEvent(event);
}

void PreviewSurfaceWidget::resizeEvent(QResizeEvent *event) {
    refresh_overlay_visibility();
    QWidget::resizeEvent(event);
}

QRect PreviewSurfaceWidget::content_rect() const {
    return rect().adjusted(kSurfaceMargin, kSurfaceMargin, -kSurfaceMargin, -kSurfaceMargin);
}

void PreviewSurfaceWidget::refresh_overlay_visibility() {
    if (top_right_overlay_container_ == nullptr) {
        return;
    }

    layout_overlay_controls();
    top_right_overlay_container_->setVisible(controls_enabled_);
    top_right_overlay_container_->raise();
}

void PreviewSurfaceWidget::layout_overlay_controls() {
    if (top_right_overlay_container_ == nullptr) {
        return;
    }

    const QRect preview_rect = content_rect();
    top_right_overlay_container_->adjustSize();
    const QSize overlay_size = top_right_overlay_container_->sizeHint();
    const int overlay_x = std::max(
        preview_rect.left() + kOverlayMargin,
        preview_rect.right() - overlay_size.width() - kOverlayMargin + 1
    );
    const int overlay_y = preview_rect.top() + kOverlayMargin;
    top_right_overlay_container_->setGeometry(overlay_x, overlay_y, overlay_size.width(), overlay_size.height());
}
