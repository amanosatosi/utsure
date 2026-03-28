#include "preview_surface_widget.hpp"

#include <QEnterEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QPainter>
#include <QPaintEvent>
#include <QRect>
#include <QSize>
#include <QSizePolicy>
#include <QStyle>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QToolButton>

namespace {

constexpr int kSurfaceMargin = 10;
constexpr int kOverlayMargin = 14;
constexpr int kOverlaySpacing = 8;
constexpr int kOverlayButtonSize = 34;
constexpr int kDefaultPreviewWidth = 640;
constexpr int kDefaultPreviewHeight = 360;
constexpr int kMinimumPreviewWidth = 320;
constexpr int kMinimumPreviewHeight = 180;

}  // namespace

PreviewSurfaceWidget::PreviewSurfaceWidget(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    QSizePolicy size_policy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    size_policy.setHeightForWidth(true);
    setSizePolicy(size_policy);

    controls_container_ = new QWidget(this);
    controls_container_->setAttribute(Qt::WA_StyledBackground, true);
    auto *controls_layout = new QHBoxLayout(controls_container_);
    controls_layout->setContentsMargins(0, 0, 0, 0);
    controls_layout->setSpacing(kOverlaySpacing);

    play_pause_button_ = new QToolButton(controls_container_);
    play_pause_button_->setObjectName("PreviewOverlayButton");
    play_pause_button_->setCursor(Qt::PointingHandCursor);
    play_pause_button_->setFixedSize(kOverlayButtonSize, kOverlayButtonSize);
    play_pause_button_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    controls_layout->addWidget(play_pause_button_);

    stop_button_ = new QToolButton(controls_container_);
    stop_button_->setObjectName("PreviewOverlayButton");
    stop_button_->setCursor(Qt::PointingHandCursor);
    stop_button_->setFixedSize(kOverlayButtonSize, kOverlayButtonSize);
    stop_button_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    controls_layout->addWidget(stop_button_);

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
    if (event->button() == Qt::LeftButton) {
        emit surface_clicked();
    }

    QWidget::mousePressEvent(event);
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
    if (controls_container_ == nullptr) {
        return;
    }

    layout_overlay_controls();
    controls_container_->setVisible(controls_enabled_ && hovered_);
    controls_container_->raise();
}

void PreviewSurfaceWidget::layout_overlay_controls() {
    if (controls_container_ == nullptr) {
        return;
    }

    controls_container_->adjustSize();
    const QSize overlay_size = controls_container_->sizeHint();
    const int x = std::max(
        kOverlayMargin,
        (width() - overlay_size.width()) / 2
    );
    const int y = std::max(
        kOverlayMargin,
        height() - overlay_size.height() - kOverlayMargin
    );
    controls_container_->setGeometry(x, y, overlay_size.width(), overlay_size.height());
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
