#include "preview_surface_widget.hpp"

#include <QEnterEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QRect>
#include <QSize>
#include <QSizePolicy>
#include <QStyle>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kSurfaceMargin = 10;
constexpr int kOverlayMargin = 14;
constexpr int kOverlaySpacing = 6;
constexpr int kOverlayButtonSize = 30;
constexpr int kDefaultPreviewWidth = 568;
constexpr int kDefaultPreviewHeight = 320;
constexpr int kMinimumPreviewWidth = 288;
constexpr int kMinimumPreviewHeight = 162;

}  // namespace

PreviewSurfaceWidget::PreviewSurfaceWidget(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setMouseTracking(true);
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

    bottom_overlay_container_ = new QWidget(this);
    bottom_overlay_container_->setObjectName("PreviewBottomOverlay");
    bottom_overlay_container_->setAttribute(Qt::WA_StyledBackground, true);
    auto *bottom_overlay_layout = new QVBoxLayout(bottom_overlay_container_);
    bottom_overlay_layout->setContentsMargins(10, 8, 10, 8);
    bottom_overlay_layout->setSpacing(kOverlaySpacing);

    overlay_content_container_ = new QWidget(bottom_overlay_container_);
    auto *content_layout = new QVBoxLayout(overlay_content_container_);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(kOverlaySpacing);
    overlay_content_layout_ = content_layout;

    transport_controls_widget_ = new QWidget(bottom_overlay_container_);
    auto *transport_controls_layout = new QHBoxLayout(transport_controls_widget_);
    transport_controls_layout->setContentsMargins(0, 0, 0, 0);
    transport_controls_layout->setSpacing(4);

    play_pause_button_ = new QToolButton(transport_controls_widget_);
    play_pause_button_->setObjectName("PreviewOverlayButton");
    play_pause_button_->setCursor(Qt::PointingHandCursor);
    play_pause_button_->setFixedSize(kOverlayButtonSize, kOverlayButtonSize);
    play_pause_button_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    play_pause_button_->setFocusPolicy(Qt::NoFocus);

    stop_button_ = new QToolButton(transport_controls_widget_);
    stop_button_->setObjectName("PreviewOverlayButton");
    stop_button_->setCursor(Qt::PointingHandCursor);
    stop_button_->setFixedSize(kOverlayButtonSize, kOverlayButtonSize);
    stop_button_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    stop_button_->setFocusPolicy(Qt::NoFocus);

    transport_controls_layout->addWidget(play_pause_button_);
    transport_controls_layout->addWidget(stop_button_);
    bottom_overlay_layout->addWidget(overlay_content_container_);

    connect(play_pause_button_, &QToolButton::clicked, this, &PreviewSurfaceWidget::play_pause_requested);
    connect(stop_button_, &QToolButton::clicked, this, &PreviewSurfaceWidget::stop_requested);

    refresh_control_icons();
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
    if (controls_enabled_ == enabled) {
        refresh_overlay_visibility();
        return;
    }

    controls_enabled_ = enabled;
    play_pause_button_->setEnabled(enabled);
    stop_button_->setEnabled(enabled);
    refresh_overlay_visibility();
}

void PreviewSurfaceWidget::set_playing(const bool playing) {
    if (playing_ == playing) {
        return;
    }

    playing_ = playing;
    refresh_control_icons();
}

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

QVBoxLayout *PreviewSurfaceWidget::overlay_content_layout() const noexcept {
    return overlay_content_layout_;
}

QWidget *PreviewSurfaceWidget::transport_controls_widget() const noexcept {
    return transport_controls_widget_;
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

void PreviewSurfaceWidget::enterEvent(QEnterEvent *event) {
    hovered_ = true;
    refresh_overlay_visibility();
    QWidget::enterEvent(event);
}

void PreviewSurfaceWidget::leaveEvent(QEvent *event) {
    hovered_ = false;
    refresh_overlay_visibility();
    QWidget::leaveEvent(event);
}

void PreviewSurfaceWidget::resizeEvent(QResizeEvent *event) {
    layout_overlay_controls();
    QWidget::resizeEvent(event);
}

QRect PreviewSurfaceWidget::content_rect() const {
    return rect().adjusted(kSurfaceMargin, kSurfaceMargin, -kSurfaceMargin, -kSurfaceMargin);
}

void PreviewSurfaceWidget::refresh_overlay_visibility() {
    if (top_right_overlay_container_ == nullptr || bottom_overlay_container_ == nullptr) {
        return;
    }

    layout_overlay_controls();
    top_right_overlay_container_->setVisible(controls_enabled_);
    bottom_overlay_container_->setVisible(controls_enabled_ && hovered_);
    top_right_overlay_container_->raise();
    bottom_overlay_container_->raise();
}

void PreviewSurfaceWidget::layout_overlay_controls() {
    if (top_right_overlay_container_ == nullptr || bottom_overlay_container_ == nullptr) {
        return;
    }

    const QRect preview_rect = content_rect();

    top_right_overlay_container_->adjustSize();
    const QSize top_right_size = top_right_overlay_container_->sizeHint();
    const int top_right_x = std::max(
        preview_rect.left() + kOverlayMargin,
        preview_rect.right() - top_right_size.width() - kOverlayMargin + 1
    );
    const int top_right_y = preview_rect.top() + kOverlayMargin;
    top_right_overlay_container_->setGeometry(top_right_x, top_right_y, top_right_size.width(), top_right_size.height());

    const int available_width = std::max(0, preview_rect.width() - (kOverlayMargin * 2));
    const int preferred_width = std::clamp(
        static_cast<int>(std::lround(static_cast<double>(preview_rect.width()) * 0.72)),
        300,
        560
    );
    const int overlay_width = std::min(available_width, preferred_width);
    bottom_overlay_container_->setFixedWidth(overlay_width);
    bottom_overlay_container_->adjustSize();
    const QSize bottom_overlay_size = bottom_overlay_container_->sizeHint();
    const int overlay_x = std::max(preview_rect.left() + kOverlayMargin, (width() - overlay_width) / 2);
    const int overlay_y = std::max(
        preview_rect.top() + kOverlayMargin,
        preview_rect.bottom() - bottom_overlay_size.height() - kOverlayMargin + 1
    );
    bottom_overlay_container_->setGeometry(overlay_x, overlay_y, overlay_width, bottom_overlay_size.height());
}

void PreviewSurfaceWidget::refresh_control_icons() {
    if (play_pause_button_ == nullptr || stop_button_ == nullptr) {
        return;
    }

    const QString play_pause_text = playing_ ? "Pause" : "Play";
    const QString play_pause_tooltip = playing_ ? "Pause preview" : "Play preview";
    const QIcon play_pause_icon(playing_ ? ":/icons/pause.svg" : ":/icons/play.svg");
    play_pause_button_->setText(play_pause_text);
    play_pause_button_->setToolTip(play_pause_tooltip);
    play_pause_button_->setIcon(play_pause_icon);
    play_pause_button_->setIconSize(QSize(16, 16));
    play_pause_button_->setToolButtonStyle(play_pause_icon.isNull() ? Qt::ToolButtonTextOnly : Qt::ToolButtonIconOnly);
    play_pause_button_->setProperty("iconFallback", play_pause_icon.isNull());
    play_pause_button_->style()->unpolish(play_pause_button_);
    play_pause_button_->style()->polish(play_pause_button_);
    play_pause_button_->update();

    const QIcon stop_icon(":/icons/stop.svg");
    stop_button_->setText("Stop");
    stop_button_->setToolTip("Stop preview and return to trim in");
    stop_button_->setIcon(stop_icon);
    stop_button_->setIconSize(QSize(16, 16));
    stop_button_->setToolButtonStyle(stop_icon.isNull() ? Qt::ToolButtonTextOnly : Qt::ToolButtonIconOnly);
    stop_button_->setProperty("iconFallback", stop_icon.isNull());
    stop_button_->style()->unpolish(stop_button_);
    stop_button_->style()->polish(stop_button_);
    stop_button_->update();
}
