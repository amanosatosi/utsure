#include "forced_queue_notification.hpp"

#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QEasingCurve>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPixmap>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QScreen>
#include <QShowEvent>
#include <QSoundEffect>
#include <QSvgRenderer>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#endif

namespace utsure::app {
namespace {

constexpr int kPanelWidth = 540;
constexpr int kPanelHeight = 270;
constexpr int kWorkAreaMargin = 18;
constexpr int kFullyVisibleDurationMs = 3000;
constexpr int kFadeDurationMs = 5000;
static_assert(kPanelWidth >= 500 && kPanelWidth <= 560);
static_assert(kPanelHeight >= 250 && kPanelHeight <= 290);

bool is_success(const QueueTerminalNotificationData &data) {
    return data.outcome == QueueTerminalNotificationOutcome::succeeded;
}

QString sound_status_name(const QSoundEffect::Status status) {
    switch (status) {
    case QSoundEffect::Null:
        return "Null";
    case QSoundEffect::Loading:
        return "Loading";
    case QSoundEffect::Ready:
        return "Ready";
    case QSoundEffect::Error:
        return "Error";
    }
    return "Unknown";
}

QPixmap render_svg_pixmap(const QString &resource_path, const QSize &logical_size) {
    QSvgRenderer renderer(resource_path);
    if (!renderer.isValid()) {
        return {};
    }

    constexpr qreal kDevicePixelRatio = 2.0;
    QPixmap pixmap(logical_size * kDevicePixelRatio);
    pixmap.setDevicePixelRatio(kDevicePixelRatio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    renderer.render(&painter, QRectF(QPointF(0.0, 0.0), QSizeF(logical_size)));
    return pixmap;
}

class StatusIconWidget final : public QWidget {
public:
    explicit StatusIconWidget(QWidget *parent) : QWidget(parent) {
        setFixedSize(54, 54);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

    void set_success(const bool success) {
        success_ = success;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QColor fill = success_ ? QColor("#123B22") : QColor("#40191B");
        const QColor outline = success_ ? QColor("#7FEA8B") : QColor("#FF7C7C");
        const QColor mark = success_ ? QColor("#9BFFA6") : QColor("#FFA0A0");
        painter.setPen(QPen(outline, 2.5));
        painter.setBrush(fill);
        painter.drawEllipse(QRectF(2.0, 2.0, 50.0, 50.0));
        painter.setPen(QPen(mark, 4.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (success_) {
            QPainterPath check;
            check.moveTo(14.0, 28.0);
            check.lineTo(23.0, 37.0);
            check.lineTo(40.0, 18.0);
            painter.drawPath(check);
        } else {
            painter.drawLine(QPointF(17.0, 17.0), QPointF(37.0, 37.0));
            painter.drawLine(QPointF(37.0, 17.0), QPointF(17.0, 37.0));
        }
    }

private:
    bool success_{true};
};

QString normalized_parent_directory(const QString &output_path) {
    const QString clean_path = QDir::cleanPath(QDir::fromNativeSeparators(output_path.trimmed()));
    return clean_path.isEmpty() ? QString{} : QFileInfo(clean_path).absolutePath();
}

}  // namespace

ForcedQueueNotification::ForcedQueueNotification(QWidget *owner_window)
    : QWidget(
          nullptr,
          Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus
      ),
      owner_window_(owner_window) {
    setObjectName("ForcedQueueNotification");
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_QuitOnClose, false);
    setFocusPolicy(Qt::NoFocus);
    setFixedSize(kPanelWidth, kPanelHeight);

    auto *root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(18, 14, 18, 14);
    root_layout->setSpacing(6);

    auto *header = new QWidget(this);
    header->setFixedHeight(38);
    auto *header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(0, 0, 0, 0);
    header_layout->setSpacing(8);
    auto *logo_label = new QLabel(header);
    logo_label->setFixedSize(36, 36);
    logo_label->setPixmap(render_svg_pixmap(":/icons/logo.svg", logo_label->size()));
    logo_label->setScaledContents(false);
    logo_label->setAlignment(Qt::AlignCenter);
    auto *brand_label = new QLabel("UTSURE", header);
    brand_label->setObjectName("ForcedNotificationBrand");
    header_layout->addWidget(logo_label);
    header_layout->addWidget(brand_label);
    header_layout->addStretch(1);

    forced_badge_ = new QWidget(header);
    forced_badge_->setObjectName("ForcedNotificationBadge");
    forced_badge_->setFixedSize(142, 30);
    auto *badge_layout = new QHBoxLayout(forced_badge_);
    badge_layout->setContentsMargins(10, 0, 10, 0);
    badge_layout->setSpacing(5);
    auto *bolt_label = new QLabel(QStringLiteral("\u26A1"), forced_badge_);
    bolt_label->setObjectName("ForcedNotificationBolt");
    auto *badge_label = new QLabel("Forced Notification", forced_badge_);
    badge_label->setObjectName("ForcedNotificationBadgeText");
    badge_layout->addWidget(bolt_label);
    badge_layout->addWidget(badge_label);
    header_layout->addWidget(forced_badge_, 0, Qt::AlignVCenter);

    close_button_ = new QToolButton(header);
    close_button_->setObjectName("ForcedNotificationClose");
    close_button_->setText(QStringLiteral("\u00D7"));
    close_button_->setFixedSize(28, 28);
    close_button_->setCursor(Qt::PointingHandCursor);
    close_button_->setFocusPolicy(Qt::NoFocus);
    header_layout->addWidget(close_button_, 0, Qt::AlignVCenter);
    root_layout->addWidget(header);

    auto *content = new QWidget(this);
    auto *content_layout = new QHBoxLayout(content);
    content_layout->setContentsMargins(5, 0, 5, 0);
    content_layout->setSpacing(10);
    status_icon_ = new StatusIconWidget(content);
    content_layout->addWidget(status_icon_, 0, Qt::AlignTop);

    auto *text_column = new QWidget(content);
    auto *text_layout = new QVBoxLayout(text_column);
    text_layout->setContentsMargins(0, 1, 0, 0);
    text_layout->setSpacing(3);
    title_label_ = new QLabel(text_column);
    title_label_->setObjectName("ForcedNotificationTitle");
    result_label_ = new QLabel(text_column);
    result_label_->setObjectName("ForcedNotificationResult");
    result_label_->setWordWrap(true);
    auto *separator = new QWidget(text_column);
    separator->setObjectName("ForcedNotificationSeparator");
    separator->setFixedHeight(1);
    information_label_ = new QLabel(text_column);
    information_label_->setObjectName("ForcedNotificationInformation");
    information_label_->setWordWrap(true);
    information_label_->setTextFormat(Qt::RichText);
    text_layout->addWidget(title_label_);
    text_layout->addWidget(result_label_);
    text_layout->addSpacing(3);
    text_layout->addWidget(separator);
    text_layout->addSpacing(3);
    text_layout->addWidget(information_label_, 1, Qt::AlignTop);
    content_layout->addWidget(text_column, 1);

    mascot_label_ = new QLabel(content);
    mascot_label_->setObjectName("ForcedNotificationMascot");
    mascot_label_->setFixedSize(130, 128);
    mascot_label_->setAlignment(Qt::AlignCenter | Qt::AlignBottom);
    content_layout->addWidget(mascot_label_, 0, Qt::AlignTop);
    root_layout->addWidget(content, 1);

    auto *button_row = new QHBoxLayout();
    button_row->setContentsMargins(2, 0, 2, 0);
    button_row->setSpacing(8);
    primary_button_ = new QPushButton(this);
    secondary_button_ = new QPushButton(this);
    dismiss_button_ = new QPushButton("Dismiss", this);
    for (QPushButton *button : {primary_button_, secondary_button_, dismiss_button_}) {
        button->setFixedHeight(36);
        button->setMinimumWidth(104);
        button->setMaximumWidth(132);
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
    }
    primary_button_->setObjectName("ForcedNotificationPrimary");
    secondary_button_->setObjectName("ForcedNotificationSecondary");
    dismiss_button_->setObjectName("ForcedNotificationSecondary");
    button_row->addWidget(primary_button_, 1);
    button_row->addWidget(secondary_button_, 1);
    button_row->addStretch(1);
    button_row->addWidget(dismiss_button_, 1);
    root_layout->addLayout(button_row);

    success_sound_ = new QSoundEffect(this);
    success_sound_->setLoopCount(1);
    success_sound_->setVolume(0.85F);
    success_sound_->setSource(QUrl(QStringLiteral("qrc:/audio/\u6C7A\u5B9A\u30DC\u30BF\u30F3\u3092\u62BC\u305941.wav")));
    failure_sound_ = new QSoundEffect(this);
    failure_sound_->setLoopCount(1);
    failure_sound_->setVolume(0.85F);
    failure_sound_->setSource(QUrl(QStringLiteral("qrc:/audio/\u30D3\u30FC\u30D7\u97F34.wav")));
    visible_timer_ = new QTimer(this);
    visible_timer_->setSingleShot(true);
    fade_animation_ = new QPropertyAnimation(this, "windowOpacity", this);
    fade_animation_->setDuration(kFadeDurationMs);
    fade_animation_->setStartValue(1.0);
    fade_animation_->setEndValue(0.0);
    fade_animation_->setEasingCurve(QEasingCurve::InOutQuad);

    connect(close_button_, &QToolButton::clicked, this, [this]() { dismiss(); });
    connect(dismiss_button_, &QPushButton::clicked, this, [this]() { dismiss(); });
    connect(primary_button_, &QPushButton::clicked, this, [this]() { handle_primary_action(); });
    connect(secondary_button_, &QPushButton::clicked, this, [this]() { handle_secondary_action(); });
    connect(success_sound_, &QSoundEffect::statusChanged, this, [this]() {
        if (success_sound_->status() == QSoundEffect::Error) {
            log_sound_failure_once(true, success_sound_, "QSoundEffect entered Error status");
        }
    });
    connect(failure_sound_, &QSoundEffect::statusChanged, this, [this]() {
        if (failure_sound_->status() == QSoundEffect::Error) {
            log_sound_failure_once(false, failure_sound_, "QSoundEffect entered Error status");
        }
    });
}

ForcedQueueNotification::~ForcedQueueNotification() {
    stop_lifetime();
    stop_sounds();
}

void ForcedQueueNotification::set_open_logs_handler(std::function<void()> handler) {
    open_logs_handler_ = std::move(handler);
}

void ForcedQueueNotification::set_log_handler(std::function<void(const QString &)> handler) {
    log_handler_ = std::move(handler);
}

void ForcedQueueNotification::present(const QueueTerminalNotificationData &data) {
#ifndef _WIN32
    Q_UNUSED(data);
    return;
#else
    if (data.run_id == 0 || data.run_id == last_presented_run_id_) {
        return;
    }

    stop_lifetime();
    setWindowOpacity(1.0);
    last_presented_run_id_ = data.run_id;
    current_data_ = data;
    apply_theme();
    update_content();
    position_inside_work_area();
    apply_windows_no_activate(true);
    play_sound_once(is_success(data));
    start_lifetime(data.run_id);
#endif
}

void ForcedQueueNotification::dismiss() {
    stop_lifetime();
    stop_sounds();
    hide();
    setWindowOpacity(1.0);
}

void ForcedQueueNotification::paintEvent(QPaintEvent *) {
    const bool success = !current_data_.has_value() || is_success(*current_data_);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF panel_rect = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
    QLinearGradient background(panel_rect.topLeft(), panel_rect.bottomRight());
    background.setColorAt(0.0, success ? QColor("#0F1B18") : QColor("#211112"));
    background.setColorAt(1.0, success ? QColor("#14231B") : QColor("#2B1416"));
    painter.setPen(QPen(success ? QColor("#65C96F") : QColor("#E35D5D"), 1.5));
    painter.setBrush(background);
    painter.drawRoundedRect(panel_rect, 18.0, 18.0);

    painter.save();
    QPainterPath clip;
    clip.addRoundedRect(panel_rect, 18.0, 18.0);
    painter.setClipPath(clip);
    QPainterPath curve;
    curve.moveTo(-10.0, 220.0);
    curve.cubicTo(115.0, 185.0, 280.0, 290.0, 560.0, 205.0);
    curve.lineTo(560.0, 285.0);
    curve.lineTo(-10.0, 285.0);
    curve.closeSubpath();
    painter.setPen(Qt::NoPen);
    QColor curve_fill = success ? QColor("#2E7D32") : QColor("#8A2424");
    curve_fill.setAlphaF(0.25);
    painter.setBrush(curve_fill);
    painter.drawPath(curve);
    painter.setPen(QPen(success ? QColor(143, 227, 154, 46) : QColor(255, 139, 139, 46), 1.25));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(curve);
    painter.restore();
}

void ForcedQueueNotification::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    position_inside_work_area();
    apply_windows_no_activate(false);
}

void ForcedQueueNotification::closeEvent(QCloseEvent *event) {
    dismiss();
    event->accept();
}

bool ForcedQueueNotification::nativeEvent(const QByteArray &event_type, void *message, qintptr *result) {
#ifdef _WIN32
    Q_UNUSED(event_type);
    auto *native_message = static_cast<MSG *>(message);
    if (native_message != nullptr && native_message->message == WM_MOUSEACTIVATE) {
        if (result != nullptr) {
            *result = MA_NOACTIVATE;
        }
        return true;
    }
#else
    Q_UNUSED(event_type);
    Q_UNUSED(message);
    Q_UNUSED(result);
#endif
    return false;
}

void ForcedQueueNotification::apply_theme() {
    if (!current_data_.has_value()) {
        return;
    }
    const bool success = is_success(*current_data_);
    static_cast<StatusIconWidget *>(status_icon_)->set_success(success);
    const QString accent = success ? "#9BFFA6" : "#FFA0A0";
    const QString badge_background = success ? "#1E5C2E" : "#6E2428";
    const QString primary_start = success ? "#45B85C" : "#C94646";
    const QString primary_end = success ? "#79D66F" : "#E56A5B";
    setStyleSheet(QString(R"(
        QWidget { background: transparent; color: #F5F3F5; }
        QLabel#ForcedNotificationBrand { color: #F7F2E8; font-size: 18px; font-weight: 800; letter-spacing: 1px; }
        QWidget#ForcedNotificationBadge { background: %1; border-radius: 15px; }
        QLabel#ForcedNotificationBolt { color: %2; font-size: 12px; font-weight: 800; }
        QLabel#ForcedNotificationBadgeText { color: #F7F2F3; font-size: 10px; font-weight: 700; }
        QToolButton#ForcedNotificationClose { color: #EFE8EA; border: none; border-radius: 14px; font-size: 20px; }
        QToolButton#ForcedNotificationClose:hover { background: rgba(255, 255, 255, 28); }
        QLabel#ForcedNotificationTitle { color: %2; font-family: "Yu Gothic UI", "Meiryo UI"; font-size: 27px; font-weight: 800; }
        QLabel#ForcedNotificationResult { color: #F2EDEF; font-size: 12px; font-weight: 600; }
        QWidget#ForcedNotificationSeparator { background: rgba(255, 255, 255, 38); }
        QLabel#ForcedNotificationInformation { color: #D3CBCD; font-size: 11px; }
        QPushButton { border-radius: 9px; padding: 0 10px; font-size: 11px; font-weight: 750; }
        QPushButton#ForcedNotificationPrimary { color: #101713; border: none; background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 %3, stop:1 %4); }
        QPushButton#ForcedNotificationPrimary:hover { border: 2px solid %2; }
        QPushButton#ForcedNotificationSecondary { color: #F4EEF0; background: #2C292D; border: 1px solid #504A50; }
        QPushButton#ForcedNotificationSecondary:hover { border-color: %2; }
    )").arg(badge_background, accent, primary_start, primary_end));
    update();
}

void ForcedQueueNotification::update_content() {
    if (!current_data_.has_value()) {
        return;
    }

    const auto &data = *current_data_;
    const bool success = is_success(data);
    title_label_->setText(
        success
            ? QStringLiteral("\u7D42\u308F\u3063\u305F\u306E\u3060")
            : QStringLiteral("\u30C0\u30E1\u3060\u3063\u305F\u306E\u3060")
    );
    if (success) {
        const QString job_word = data.successful_job_count == 1 ? "job" : "jobs";
        result_label_->setText(
            QString("Queue finished<br><span style='color:#C9D8CC'>%1 %2 completed successfully</span>")
                .arg(data.successful_job_count)
                .arg(job_word)
        );
        update_mascot(QStringLiteral(":/images/\u305A\u3093\u3060\u3082\u3093\u30FC\u732B\u3060\u3063\u30531.png"));
    } else {
        const QString summary = data.failure_summary.trimmed().isEmpty()
            ? QString("One or more encode jobs failed")
            : data.failure_summary.trimmed();
        result_label_->setText(QString("Queue failed<br><span style='color:#DDC6C8'>%1</span>").arg(summary.toHtmlEscaped()));
        update_mascot(QStringLiteral(":/images/\u305A\u3093\u3060\u3082\u3093\u30FC\u843D\u3061\u8FBC\u308012.png"));
    }
    result_label_->setTextFormat(Qt::RichText);

    const QString shared_directory = shared_output_directory();
    if (success && data.successful_job_count == 1 && data.completed_output_paths.size() == 1) {
        const QString output_path = data.completed_output_paths.front();
        information_label_->setText(
            QString("<b style='color:#F4EEF0'>Output:</b>&nbsp;&nbsp;%1<br><b style='color:#F4EEF0'>Saved to:</b>&nbsp;&nbsp;%2")
                .arg(QFileInfo(output_path).fileName().toHtmlEscaped(), QDir::toNativeSeparators(shared_directory).toHtmlEscaped())
        );
        primary_button_->setText("Open File");
        primary_button_->setVisible(true);
        secondary_button_->setText("Open Folder");
        secondary_button_->setVisible(!shared_directory.isEmpty());
    } else if (success) {
        const QString output_word = data.successful_job_count == 1 ? "output" : "outputs";
        const bool all_output_locations_known =
            data.completed_output_paths.size() == data.successful_job_count;
        const QString location = !all_output_locations_known
            ? QString("Output locations unavailable")
            : shared_directory.isEmpty()
                ? QString("Outputs saved to multiple folders")
                : QString("Saved to: %1").arg(QDir::toNativeSeparators(shared_directory));
        information_label_->setText(
            QString("<b style='color:#F4EEF0'>Queue:</b>&nbsp;&nbsp;%1 %2 completed<br><b style='color:#F4EEF0'>Location:</b>&nbsp;&nbsp;%3")
                .arg(data.successful_job_count)
                .arg(output_word, location.toHtmlEscaped())
        );
        primary_button_->setText("Open Folder");
        primary_button_->setVisible(!shared_directory.isEmpty());
        secondary_button_->setVisible(false);
    } else {
        information_label_->setText(
            QString("<b style='color:#F4EEF0'>Completed successfully:</b>&nbsp;&nbsp;%1 of %2<br><b style='color:#F4EEF0'>Details:</b>&nbsp;&nbsp;See logs for failure details")
                .arg(data.successful_job_count)
                .arg(data.total_job_count)
        );
        primary_button_->setText("Open Logs");
        primary_button_->setVisible(true);
        secondary_button_->setVisible(false);
    }
}

void ForcedQueueNotification::update_mascot(const QString &resource_path) {
    const QPixmap source(resource_path);
    if (source.isNull()) {
        mascot_label_->clear();
        log_message(QString("[warning] Forced notification mascot resource failed to load: %1").arg(resource_path));
        return;
    }
    mascot_label_->setPixmap(source.scaled(mascot_label_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void ForcedQueueNotification::position_inside_work_area() {
    QScreen *target_screen = nullptr;
    if (QWidget *owner = owner_window_.data(); owner != nullptr) {
        target_screen = owner->screen();
        if (target_screen == nullptr) {
            target_screen = QGuiApplication::screenAt(owner->frameGeometry().center());
        }
    }
    if (target_screen == nullptr) {
        target_screen = QGuiApplication::primaryScreen();
    }
    if (target_screen == nullptr) {
        return;
    }

    const QRect available = target_screen->availableGeometry();
    move(
        std::max(available.left() + kWorkAreaMargin, available.right() - width() - kWorkAreaMargin + 1),
        std::max(available.top() + kWorkAreaMargin, available.bottom() - height() - kWorkAreaMargin + 1)
    );
}

void ForcedQueueNotification::apply_windows_no_activate(const bool show_window) {
#ifdef _WIN32
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    LONG_PTR extended_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    extended_style |= WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
    extended_style &= ~static_cast<LONG_PTR>(WS_EX_APPWINDOW);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, extended_style);
    if (show_window) {
        show();
    }
    SetWindowPos(
        hwnd,
        HWND_TOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | (show_window ? SWP_SHOWWINDOW : 0)
    );
#else
    Q_UNUSED(show_window);
#endif
}

void ForcedQueueNotification::play_sound_once(const bool success) {
    if (!current_data_.has_value()) {
        return;
    }

    stop_sounds();
    QSoundEffect *sound_effect = success ? success_sound_ : failure_sound_;
    if (sound_effect == nullptr) {
        log_sound_failure_once(success, nullptr, "QSoundEffect object is unavailable");
        return;
    }
    const QString resource_path = QStringLiteral(":") + sound_effect->source().path();
    if (!QFile::exists(resource_path)) {
        log_sound_failure_once(success, sound_effect, "Qt resource does not exist");
        return;
    }
    if (sound_effect->status() == QSoundEffect::Error) {
        log_sound_failure_once(success, sound_effect, "QSoundEffect is in Error status");
        return;
    }
    sound_effect->play();
}

void ForcedQueueNotification::log_sound_failure_once(
    const bool success,
    const QSoundEffect *sound_effect,
    const QString &reason
) {
    if (!current_data_.has_value() || is_success(*current_data_) != success ||
        audio_failure_logged_run_id_ == current_data_->run_id) {
        return;
    }
    audio_failure_logged_run_id_ = current_data_->run_id;
    const QUrl source = sound_effect == nullptr ? QUrl{} : sound_effect->source();
    const QString status = sound_effect == nullptr ? QString("Unavailable") : sound_status_name(sound_effect->status());
    const bool resource_exists = source.isValid() && QFile::exists(QStringLiteral(":") + source.path());
    log_message(
        QString("[warning] Forced notification audio failed: state=%1 url=%2 status=%3 resource_exists=%4 reason=%5")
            .arg(success ? "success" : "failure")
            .arg(source.toString())
            .arg(status)
            .arg(resource_exists ? "true" : "false")
            .arg(reason)
    );
}

void ForcedQueueNotification::start_lifetime(const quint64 run_id) {
    if (visible_timer_ == nullptr || fade_animation_ == nullptr) {
        return;
    }
    lifetime_run_id_ = run_id;
    visible_timer_->stop();
    disconnect(visible_timer_, nullptr, this, nullptr);
    connect(visible_timer_, &QTimer::timeout, this, [this, run_id]() {
        if (lifetime_run_id_ == run_id && current_data_.has_value() && current_data_->run_id == run_id) {
            begin_fade(run_id);
        }
    });
    visible_timer_->start(kFullyVisibleDurationMs);
}

void ForcedQueueNotification::begin_fade(const quint64 run_id) {
    if (fade_animation_ == nullptr || lifetime_run_id_ != run_id ||
        !current_data_.has_value() || current_data_->run_id != run_id) {
        return;
    }
    fade_animation_->stop();
    disconnect(fade_animation_, nullptr, this, nullptr);
    fade_animation_->setDuration(kFadeDurationMs);
    fade_animation_->setStartValue(1.0);
    fade_animation_->setEndValue(0.0);
    connect(fade_animation_, &QPropertyAnimation::finished, this, [this, run_id]() {
        if (lifetime_run_id_ == run_id && current_data_.has_value() && current_data_->run_id == run_id) {
            dismiss();
        }
    });
    fade_animation_->start();
}

void ForcedQueueNotification::stop_lifetime() {
    lifetime_run_id_ = 0;
    if (visible_timer_ != nullptr) {
        visible_timer_->stop();
        disconnect(visible_timer_, nullptr, this, nullptr);
    }
    if (fade_animation_ != nullptr) {
        fade_animation_->stop();
        disconnect(fade_animation_, nullptr, this, nullptr);
    }
}

void ForcedQueueNotification::stop_sounds() {
    if (success_sound_ != nullptr) {
        success_sound_->stop();
    }
    if (failure_sound_ != nullptr) {
        failure_sound_->stop();
    }
}

void ForcedQueueNotification::handle_primary_action() {
    if (!current_data_.has_value()) {
        return;
    }

    const auto &data = *current_data_;
    stop_lifetime();
    if (!is_success(data)) {
        if (open_logs_handler_) {
            open_logs_handler_();
        }
        dismiss();
        return;
    }

    const QString target = data.completed_output_paths.size() == 1
        ? data.completed_output_paths.front()
        : shared_output_directory();
    if (!target.isEmpty() && !QDesktopServices::openUrl(QUrl::fromLocalFile(target))) {
        log_message(QString("[warning] Forced notification action could not open: %1").arg(target));
    }
    dismiss();
}

void ForcedQueueNotification::handle_secondary_action() {
    stop_lifetime();
    const QString target = shared_output_directory();
    if (!target.isEmpty() && !QDesktopServices::openUrl(QUrl::fromLocalFile(target))) {
        log_message(QString("[warning] Forced notification action could not open folder: %1").arg(target));
    }
    dismiss();
}

void ForcedQueueNotification::log_message(const QString &message) const {
    if (log_handler_) {
        log_handler_(message);
    }
}

QString ForcedQueueNotification::shared_output_directory() const {
    if (!current_data_.has_value() || current_data_->completed_output_paths.isEmpty()) {
        return {};
    }
    if (is_success(*current_data_) &&
        current_data_->completed_output_paths.size() != current_data_->successful_job_count) {
        return {};
    }

    const QString first_directory = normalized_parent_directory(current_data_->completed_output_paths.front());
    if (first_directory.isEmpty()) {
        return {};
    }
    for (const QString &path : current_data_->completed_output_paths) {
        if (normalized_parent_directory(path).compare(first_directory, Qt::CaseInsensitive) != 0) {
            return {};
        }
    }
    return first_directory;
}

}  // namespace utsure::app
