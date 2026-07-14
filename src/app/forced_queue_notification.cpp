#include "forced_queue_notification.hpp"

#include <QAudioOutput>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QMediaPlayer>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QShowEvent>
#include <QSvgRenderer>
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

constexpr int kPanelWidth = 880;
constexpr int kPanelHeight = 430;
constexpr int kWorkAreaMargin = 24;

bool is_success(const QueueTerminalNotificationData &data) {
    return data.outcome == QueueTerminalNotificationOutcome::succeeded;
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
        setFixedSize(86, 86);
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
        painter.setPen(QPen(outline, 4.0));
        painter.setBrush(fill);
        painter.drawEllipse(QRectF(3.0, 3.0, 80.0, 80.0));
        painter.setPen(QPen(mark, 7.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (success_) {
            QPainterPath check;
            check.moveTo(23.0, 44.0);
            check.lineTo(37.0, 58.0);
            check.lineTo(64.0, 29.0);
            painter.drawPath(check);
        } else {
            painter.drawLine(QPointF(27.0, 27.0), QPointF(59.0, 59.0));
            painter.drawLine(QPointF(59.0, 27.0), QPointF(27.0, 59.0));
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
    root_layout->setContentsMargins(34, 24, 34, 24);
    root_layout->setSpacing(10);

    auto *header = new QWidget(this);
    header->setFixedHeight(62);
    auto *header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(0, 0, 0, 0);
    header_layout->setSpacing(12);
    auto *logo_label = new QLabel(header);
    logo_label->setFixedSize(58, 58);
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
    forced_badge_->setFixedSize(195, 44);
    auto *badge_layout = new QHBoxLayout(forced_badge_);
    badge_layout->setContentsMargins(16, 0, 16, 0);
    badge_layout->setSpacing(8);
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
    close_button_->setFixedSize(36, 36);
    close_button_->setCursor(Qt::PointingHandCursor);
    close_button_->setFocusPolicy(Qt::NoFocus);
    header_layout->addWidget(close_button_, 0, Qt::AlignVCenter);
    root_layout->addWidget(header);

    auto *content = new QWidget(this);
    auto *content_layout = new QHBoxLayout(content);
    content_layout->setContentsMargins(10, 0, 10, 0);
    content_layout->setSpacing(20);
    status_icon_ = new StatusIconWidget(content);
    content_layout->addWidget(status_icon_, 0, Qt::AlignTop);

    auto *text_column = new QWidget(content);
    auto *text_layout = new QVBoxLayout(text_column);
    text_layout->setContentsMargins(0, 2, 0, 0);
    text_layout->setSpacing(5);
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
    text_layout->addSpacing(8);
    text_layout->addWidget(separator);
    text_layout->addSpacing(7);
    text_layout->addWidget(information_label_, 1, Qt::AlignTop);
    content_layout->addWidget(text_column, 1);

    mascot_label_ = new QLabel(content);
    mascot_label_->setObjectName("ForcedNotificationMascot");
    mascot_label_->setFixedSize(220, 210);
    mascot_label_->setAlignment(Qt::AlignCenter | Qt::AlignBottom);
    content_layout->addWidget(mascot_label_, 0, Qt::AlignTop);
    root_layout->addWidget(content, 1);

    auto *button_row = new QHBoxLayout();
    button_row->setContentsMargins(3, 0, 3, 0);
    button_row->setSpacing(15);
    primary_button_ = new QPushButton(this);
    secondary_button_ = new QPushButton(this);
    dismiss_button_ = new QPushButton("Dismiss", this);
    for (QPushButton *button : {primary_button_, secondary_button_, dismiss_button_}) {
        button->setFixedHeight(52);
        button->setMinimumWidth(190);
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

    media_player_ = new QMediaPlayer(this);
    audio_output_ = new QAudioOutput(this);
    audio_output_->setVolume(0.85F);
    media_player_->setAudioOutput(audio_output_);

    connect(close_button_, &QToolButton::clicked, this, [this]() { dismiss(); });
    connect(dismiss_button_, &QPushButton::clicked, this, [this]() { dismiss(); });
    connect(primary_button_, &QPushButton::clicked, this, [this]() { handle_primary_action(); });
    connect(secondary_button_, &QPushButton::clicked, this, [this]() { handle_secondary_action(); });
    connect(media_player_, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &error_text) {
        if (!current_data_.has_value() || audio_failure_logged_run_id_ == current_data_->run_id) {
            return;
        }
        audio_failure_logged_run_id_ = current_data_->run_id;
        log_message(QString("[warning] Forced notification audio playback failed: %1").arg(error_text));
    });
}

ForcedQueueNotification::~ForcedQueueNotification() {
    if (media_player_ != nullptr) {
        media_player_->stop();
        media_player_->setSource(QUrl{});
    }
    audio_resource_.reset();
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

    last_presented_run_id_ = data.run_id;
    current_data_ = data;
    apply_theme();
    update_content();
    position_inside_work_area();
    apply_windows_no_activate(true);
    play_sound_once(
        is_success(data)
            ? QStringLiteral(":/audio/\u6C7A\u5B9A\u30DC\u30BF\u30F3\u3092\u62BC\u305941.mp3")
            : QStringLiteral(":/audio/\u30D3\u30FC\u30D7\u97F34.mp3")
    );
#endif
}

void ForcedQueueNotification::dismiss() {
    if (media_player_ != nullptr) {
        media_player_->stop();
    }
    hide();
}

void ForcedQueueNotification::paintEvent(QPaintEvent *) {
    const bool success = !current_data_.has_value() || is_success(*current_data_);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF panel_rect = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
    QLinearGradient background(panel_rect.topLeft(), panel_rect.bottomRight());
    background.setColorAt(0.0, success ? QColor("#0F1B18") : QColor("#211112"));
    background.setColorAt(1.0, success ? QColor("#14231B") : QColor("#2B1416"));
    painter.setPen(QPen(success ? QColor("#65C96F") : QColor("#E35D5D"), 2.5));
    painter.setBrush(background);
    painter.drawRoundedRect(panel_rect, 30.0, 30.0);

    painter.save();
    QPainterPath clip;
    clip.addRoundedRect(panel_rect, 30.0, 30.0);
    painter.setClipPath(clip);
    QPainterPath curve;
    curve.moveTo(-20.0, 360.0);
    curve.cubicTo(185.0, 295.0, 445.0, 470.0, 900.0, 340.0);
    curve.lineTo(900.0, 455.0);
    curve.lineTo(-20.0, 455.0);
    curve.closeSubpath();
    painter.setPen(Qt::NoPen);
    QColor curve_fill = success ? QColor("#2E7D32") : QColor("#8A2424");
    curve_fill.setAlphaF(0.25);
    painter.setBrush(curve_fill);
    painter.drawPath(curve);
    painter.setPen(QPen(success ? QColor(143, 227, 154, 46) : QColor(255, 139, 139, 46), 2.0));
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
        QLabel#ForcedNotificationBrand { color: #F7F2E8; font-size: 28px; font-weight: 800; letter-spacing: 2px; }
        QWidget#ForcedNotificationBadge { background: %1; border-radius: 22px; }
        QLabel#ForcedNotificationBolt { color: %2; font-size: 18px; font-weight: 800; }
        QLabel#ForcedNotificationBadgeText { color: #F7F2F3; font-size: 14px; font-weight: 700; }
        QToolButton#ForcedNotificationClose { color: #EFE8EA; border: none; border-radius: 18px; font-size: 28px; }
        QToolButton#ForcedNotificationClose:hover { background: rgba(255, 255, 255, 28); }
        QLabel#ForcedNotificationTitle { color: %2; font-family: "Yu Gothic UI", "Meiryo UI"; font-size: 40px; font-weight: 800; }
        QLabel#ForcedNotificationResult { color: #F2EDEF; font-size: 18px; font-weight: 600; }
        QWidget#ForcedNotificationSeparator { background: rgba(255, 255, 255, 38); }
        QLabel#ForcedNotificationInformation { color: #D3CBCD; font-size: 15px; line-height: 1.35; }
        QPushButton { border-radius: 14px; padding: 0 18px; font-size: 16px; font-weight: 750; }
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

void ForcedQueueNotification::play_sound_once(const QString &resource_path) {
    if (!current_data_.has_value() || media_player_ == nullptr) {
        return;
    }

    media_player_->stop();
    media_player_->setSource(QUrl{});
    audio_resource_ = std::make_unique<QFile>(resource_path);
    if (!audio_resource_->open(QIODevice::ReadOnly)) {
        if (audio_failure_logged_run_id_ != current_data_->run_id) {
            audio_failure_logged_run_id_ = current_data_->run_id;
            log_message(QString("[warning] Forced notification audio resource failed to open: %1").arg(resource_path));
        }
        return;
    }

    media_player_->setSourceDevice(audio_resource_.get(), QUrl(QString("qrc%1").arg(resource_path)));
    media_player_->play();
}

void ForcedQueueNotification::handle_primary_action() {
    if (!current_data_.has_value()) {
        return;
    }

    const auto &data = *current_data_;
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
}

void ForcedQueueNotification::handle_secondary_action() {
    const QString target = shared_output_directory();
    if (!target.isEmpty() && !QDesktopServices::openUrl(QUrl::fromLocalFile(target))) {
        log_message(QString("[warning] Forced notification action could not open folder: %1").arg(target));
    }
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
