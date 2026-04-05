#include "main_window.hpp"

#include "encode_job_runner_controller.hpp"
#include "preview_audio_controller.hpp"
#include "preview_frame_renderer_controller.hpp"
#include "preview_surface_widget.hpp"
#include "trim_timeline_widget.hpp"
#include "utsure/core/build_info.hpp"
#include "utsure/core/job/batch_parallelism.hpp"
#include "utsure/core/job/encode_job_preflight.hpp"
#include "utsure/core/media/media_inspector.hpp"
#include "utsure/core/media/source_import_paths.hpp"
#include "utsure/core/subtitles/subtitle_auto_selection.hpp"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDebug>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHash>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QSizePolicy>
#include <QStyle>
#include <QSvgRenderer>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTime>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#include <dwmapi.h>

#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif

#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif

#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif
#endif

namespace {

struct PathFieldWidgets final {
    QWidget *container{nullptr};
    QLineEdit *line_edit{nullptr};
    QPushButton *browse_button{nullptr};
};

Q_LOGGING_CATEGORY(previewPlaybackLog, "utsure.preview.playback")

QString to_qstring(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QString bool_text(const bool value) {
    return value ? "true" : "false";
}

QString video_file_filter() {
    return "Video Files (*.avi *.mkv *.mov *.mp4 *.webm);;All Files (*)";
}

QString output_file_filter() {
    return "Video Files (*.mkv *.mp4 *.mov *.m4v *.webm);;All Files (*)";
}

QString subtitle_file_filter() {
    return "Subtitle Files (*.ass *.ssa);;All Files (*)";
}

QString subtitle_format_hint_for_path(const QString &subtitle_path) {
    QString format_hint = QFileInfo(subtitle_path).suffix().trimmed().toLower();
    if (format_hint.isEmpty()) {
        format_hint = "ass";
    }
    return format_hint;
}

QString image_file_filter() {
    return "Image Files (*.png *.jpg *.jpeg *.webp *.bmp);;All Files (*)";
}

QString format_preflight_issue(const utsure::core::job::EncodeJobPreflightIssue &issue) {
    QString line = QString("[%1] %2")
        .arg(to_qstring(utsure::core::job::to_string(issue.severity)))
        .arg(to_qstring(issue.message));

    if (!issue.actionable_hint.empty()) {
        line += QString("\nHint: %1").arg(to_qstring(issue.actionable_hint));
    }

    return line;
}

QString format_time_us(const qint64 microseconds, const bool include_milliseconds = true) {
    const auto total_milliseconds = std::max<qint64>(0, microseconds / 1000);
    const auto milliseconds = total_milliseconds % 1000;
    const auto total_seconds = total_milliseconds / 1000;
    const auto seconds = total_seconds % 60;
    const auto minutes = (total_seconds / 60) % 60;
    const auto hours = total_seconds / 3600;

    if (include_milliseconds) {
        return QString("%1:%2:%3.%4")
            .arg(hours, 2, 10, QChar('0'))
            .arg(minutes, 2, 10, QChar('0'))
            .arg(seconds, 2, 10, QChar('0'))
            .arg(milliseconds, 3, 10, QChar('0'));
    }

    return QString("%1:%2:%3")
        .arg(hours, 2, 10, QChar('0'))
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'));
}

QString format_duration_ms(const qint64 milliseconds) {
    if (milliseconds < 0) {
        return "--";
    }

    return format_time_us(milliseconds * 1000, false);
}

QString format_file_size(const qint64 bytes) {
    if (bytes < 0) {
        return "--";
    }

    constexpr qint64 kKilobyte = 1024;
    constexpr qint64 kMegabyte = 1024 * 1024;
    constexpr qint64 kGigabyte = 1024 * 1024 * 1024;

    if (bytes >= kGigabyte) {
        return QString("%1 GB")
            .arg(QString::number(static_cast<double>(bytes) / static_cast<double>(kGigabyte), 'f', 2));
    }

    if (bytes >= kMegabyte) {
        return QString("%1 MB")
            .arg(QString::number(static_cast<double>(bytes) / static_cast<double>(kMegabyte), 'f', 2));
    }

    if (bytes >= kKilobyte) {
        return QString("%1 KB")
            .arg(QString::number(static_cast<double>(bytes) / static_cast<double>(kKilobyte), 'f', 1));
    }

    return QString("%1 B").arg(bytes);
}

double rational_to_double(const utsure::core::media::Rational &value) {
    if (!value.is_valid()) {
        return 0.0;
    }

    return static_cast<double>(value.numerator) / static_cast<double>(value.denominator);
}

QString format_audio_track_display(const utsure::core::media::MediaSourceInfo &source_info) {
    if (!source_info.primary_audio_stream.has_value()) {
        return "No source audio detected";
    }

    const auto &audio_stream = *source_info.primary_audio_stream;
    QString summary = QString("%1 | %2 | %3 channels | %4 Hz")
        .arg(source_info.audio_streams.size() > 1 ? "Selected" : "Primary")
        .arg(to_qstring(audio_stream.codec_name))
        .arg(audio_stream.channel_count)
        .arg(audio_stream.sample_rate);

    if (source_info.audio_streams.size() > 1) {
        summary += QString(" | %1 tracks").arg(static_cast<int>(source_info.audio_streams.size()));
    }

    if (audio_stream.language_tag.has_value()) {
        summary += QString(" | %1").arg(to_qstring(*audio_stream.language_tag));
    } else if (audio_stream.title.has_value()) {
        summary += QString(" | %1").arg(to_qstring(*audio_stream.title));
    }

    return summary;
}

QString basename_from_path(const QString &path_text) {
    return QFileInfo(path_text).fileName();
}

QString display_text_or_fallback(const QString &text, const QString &fallback_text) {
    const QString normalized_text = text.trimmed();
    return normalized_text.isEmpty() ? fallback_text : normalized_text;
}

QString queue_source_display_name(const MainWindow::UiEncodeJob &job) {
    const QString explicit_name = job.source_name.trimmed();
    if (!explicit_name.isEmpty()) {
        return explicit_name;
    }

    const QString inferred_name = QFileInfo(job.source_path.trimmed()).fileName().trimmed();
    return inferred_name.isEmpty() ? "(no file)" : inferred_name;
}

QString queue_type_display_name(const MainWindow::UiEncodeJob &job) {
    const QString explicit_type = job.type_label.trimmed();
    if (!explicit_type.isEmpty()) {
        return explicit_type;
    }

    const QString source_suffix = QFileInfo(job.source_path.trimmed()).suffix().trimmed().toLower();
    return source_suffix.isEmpty() ? "-" : "." + source_suffix;
}

QString queue_output_display_name(const MainWindow::UiEncodeJob &job) {
    return display_text_or_fallback(basename_from_path(job.output_path), "(unset)");
}

QByteArray load_svg_resource_bytes(const QString &resource_path, QString *failure_reason = nullptr) {
    QFile resource_file(resource_path);
    if (!resource_file.exists()) {
        if (failure_reason != nullptr) {
            *failure_reason = QString("Resource not found: %1").arg(resource_path);
        }
        return {};
    }

    if (!resource_file.open(QIODevice::ReadOnly)) {
        if (failure_reason != nullptr) {
            *failure_reason = QString("Failed to open resource: %1").arg(resource_path);
        }
        return {};
    }

    QByteArray svg_bytes = resource_file.readAll();
    if (svg_bytes.isEmpty()) {
        if (failure_reason != nullptr) {
            *failure_reason = QString("Resource was empty: %1").arg(resource_path);
        }
        return {};
    }

    if (svg_bytes.startsWith("\xEF\xBB\xBF")) {
        svg_bytes.remove(0, 3);
    }

    if (svg_bytes.contains('\0')) {
        if (failure_reason != nullptr) {
            *failure_reason = QString("Resource contains NUL bytes: %1").arg(resource_path);
        }
        return {};
    }

    return svg_bytes;
}

QColor status_color_for_state(const MainWindow::UiJobState state) {
    switch (state) {
    case MainWindow::UiJobState::finished:
        return QColor("#4ecb71");
    case MainWindow::UiJobState::failed:
    case MainWindow::UiJobState::canceled:
        return QColor("#f74f4f");
    case MainWindow::UiJobState::encoding:
        return QColor("#b241ff");
    case MainWindow::UiJobState::pending:
    default:
        return QColor("#8b8b99");
    }
}

QBrush foreground_for_state(const MainWindow::UiJobState state) {
    return QBrush(status_color_for_state(state));
}

QIcon load_resource_icon(const QString &resource_path, const QSize &icon_size, QString *failure_reason = nullptr) {
    if (resource_path.isEmpty()) {
        if (failure_reason != nullptr) {
            *failure_reason = "Empty resource path.";
        }
        return {};
    }

    QString svg_failure_reason{};
    const QByteArray svg_bytes = load_svg_resource_bytes(resource_path, &svg_failure_reason);
    if (svg_bytes.isEmpty()) {
        if (failure_reason != nullptr) {
            *failure_reason = svg_failure_reason;
        }
        return {};
    }

    const QIcon direct_icon(resource_path);
    const QPixmap direct_pixmap = direct_icon.pixmap(icon_size);
    if (!direct_pixmap.isNull()) {
        return direct_icon;
    }

    QSvgRenderer renderer(svg_bytes);
    if (!renderer.isValid()) {
        if (failure_reason != nullptr) {
            *failure_reason = QString("QSvgRenderer rejected resource bytes: %1").arg(resource_path);
        }
        return {};
    }

    constexpr qreal kDevicePixelRatio = 2.0;
    QPixmap pixmap(
        static_cast<int>(std::lround(static_cast<qreal>(icon_size.width()) * kDevicePixelRatio)),
        static_cast<int>(std::lround(static_cast<qreal>(icon_size.height()) * kDevicePixelRatio))
    );
    pixmap.fill(Qt::transparent);
    pixmap.setDevicePixelRatio(kDevicePixelRatio);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    renderer.render(&painter, QRectF(QPointF(0.0, 0.0), QSizeF(icon_size)));

    return QIcon(pixmap);
}

class SourceDropOverlayWidget final : public QWidget {
public:
    explicit SourceDropOverlayWidget(QWidget *parent = nullptr) : QWidget(parent) {
        setObjectName("SourceDropOverlay");
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);

        auto *root_layout = new QVBoxLayout(this);
        root_layout->setContentsMargins(32, 32, 32, 32);
        root_layout->addStretch(1);

        auto *card = new QFrame(this);
        card->setObjectName("SourceDropOverlayCard");
        card->setMinimumWidth(560);
        card->setMaximumWidth(700);
        card->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

        auto *card_layout = new QHBoxLayout(card);
        card_layout->setContentsMargins(26, 24, 26, 24);
        card_layout->setSpacing(22);

        auto *illustration = new QFrame(card);
        illustration->setObjectName("SourceDropOverlayIllustration");
        illustration->setFixedSize(136, 136);
        auto *illustration_layout = new QVBoxLayout(illustration);
        illustration_layout->setContentsMargins(0, 0, 0, 0);

        auto *icon_label = new QLabel(illustration);
        icon_label->setObjectName("SourceDropOverlayIcon");
        icon_label->setAlignment(Qt::AlignCenter);
        const QIcon icon = load_resource_icon(":/icons/add.svg", QSize(58, 58));
        if (!icon.isNull()) {
            icon_label->setPixmap(icon.pixmap(58, 58));
        } else {
            icon_label->setText("+");
        }
        illustration_layout->addWidget(icon_label, 1);

        auto *copy_column = new QWidget(card);
        copy_column->setProperty("chromeTransparent", true);
        copy_column->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        auto *copy_layout = new QVBoxLayout(copy_column);
        copy_layout->setContentsMargins(0, 0, 0, 0);
        copy_layout->setSpacing(8);

        auto *eyebrow_label = new QLabel("SOURCE IMPORT", copy_column);
        eyebrow_label->setObjectName("SourceDropOverlayEyebrow");
        eyebrow_label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

        auto *title_label = new QLabel("Drop videos anywhere to add them to the queue", copy_column);
        title_label->setObjectName("SourceDropOverlayTitle");
        title_label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        title_label->setMinimumWidth(360);
        title_label->setWordWrap(true);

        auto *body_label = new QLabel(
            "Local files and folders are accepted. Folders are scanned one level deep and only supported video files "
            "are added.",
            copy_column
        );
        body_label->setObjectName("SourceDropOverlayBody");
        body_label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        body_label->setWordWrap(true);

        copy_layout->addWidget(eyebrow_label);
        copy_layout->addWidget(title_label);
        copy_layout->addWidget(body_label);
        copy_layout->addStretch(1);

        card_layout->addWidget(illustration, 0, Qt::AlignVCenter);
        card_layout->addWidget(copy_column, 1);

        root_layout->addWidget(card, 0, Qt::AlignCenter);
        root_layout->addStretch(1);
        hide();
    }
};

void refresh_widget_style(QWidget *widget) {
    if (widget == nullptr || widget->style() == nullptr) {
        return;
    }

    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

void apply_icon_or_text(
    QAbstractButton *button,
    const QString &resource_path,
    const QString &fallback_text,
    const QSize &icon_size,
    const int compact_width,
    const int fixed_height,
    const bool expand_for_text_fallback
) {
    if (button == nullptr) {
        return;
    }

    QString failure_reason{};
    const QIcon icon = load_resource_icon(resource_path, icon_size, &failure_reason);
    const bool has_icon = !icon.isNull();

    button->setIconSize(icon_size);
    button->setIcon(has_icon ? icon : QIcon{});
    button->setText(has_icon ? QString{} : fallback_text);
    button->setProperty("iconFallback", !has_icon);
    button->setFixedHeight(fixed_height);

    if (has_icon || !expand_for_text_fallback) {
        button->setMinimumWidth(compact_width);
        button->setMaximumWidth(compact_width);
        button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    } else {
        button->setMinimumWidth(std::max(compact_width, button->fontMetrics().horizontalAdvance(fallback_text) + 18));
        button->setMaximumWidth(QWIDGETSIZE_MAX);
        button->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    }

    if (auto *tool_button = qobject_cast<QToolButton *>(button)) {
        tool_button->setToolButtonStyle(has_icon ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextOnly);
    }

    if (!has_icon) {
        qWarning().noquote()
            << QString("Icon fallback active for '%1': %2").arg(resource_path, failure_reason);
    }

    refresh_widget_style(button);
}

void apply_native_caption_accent(QWidget *window) {
    if (window == nullptr) {
        return;
    }

#ifdef _WIN32
    const auto hwnd = reinterpret_cast<HWND>(window->winId());
    if (hwnd == nullptr) {
        return;
    }

    const COLORREF caption_color = RGB(255, 206, 46);
    const COLORREF text_color = RGB(17, 17, 17);
    const COLORREF border_color = RGB(204, 165, 36);

    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption_color, sizeof(caption_color));
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &text_color, sizeof(text_color));
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border_color, sizeof(border_color));
#else
    Q_UNUSED(window);
#endif
}

PathFieldWidgets create_path_field(QWidget *parent, const QString &placeholder_text, const QString &browse_tooltip) {
    auto *container = new QWidget(parent);
    auto *layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto *line_edit = new QLineEdit(container);
    line_edit->setPlaceholderText(placeholder_text);

    auto *browse_button = new QPushButton(container);
    browse_button->setProperty("browseButton", true);
    browse_button->setToolTip(browse_tooltip);
    browse_button->setCursor(Qt::PointingHandCursor);
    apply_icon_or_text(browse_button, ":/icons/browse.svg", "...", QSize(15, 15), 30, 30, false);

    layout->addWidget(line_edit, 1);
    layout->addWidget(browse_button);

    return PathFieldWidgets{
        .container = container,
        .line_edit = line_edit,
        .browse_button = browse_button
    };
}

QToolButton *create_toolbar_button(
    const QString &icon_path,
    const QString &fallback_text,
    const QString &tooltip,
    QWidget *parent
) {
    auto *button = new QToolButton(parent);
    button->setProperty("toolbarButton", true);
    button->setToolTip(tooltip);
    button->setCursor(Qt::PointingHandCursor);
    apply_icon_or_text(button, icon_path, fallback_text, QSize(16, 16), 30, 30, true);
    return button;
}

QWidget *wrap_in_scroll_area(QWidget *content, QWidget *parent) {
    auto *scroll_area = new QScrollArea(parent);
    scroll_area->setWidgetResizable(true);
    scroll_area->setFrameShape(QFrame::NoFrame);
    scroll_area->setWidget(content);
    return scroll_area;
}

QIcon make_busy_icon(const int phase) {
    QPixmap pixmap(18, 18);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.translate(9, 9);
    painter.rotate(static_cast<qreal>(phase) * 30.0);

    QPen pen(QColor("#b241ff"), 2.0, Qt::SolidLine, Qt::RoundCap);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawArc(QRectF(-6.0, -6.0, 12.0, 12.0), 35 * 16, 240 * 16);

    return QIcon(pixmap);
}

QString status_prefix_for_session(const QString &job_name, const QString &line) {
    return QString("[%1] %2").arg(job_name, line);
}

bool widget_contains_global_pos(const QWidget *widget, const QPoint &global_pos) {
    if (widget == nullptr || !widget->isVisible()) {
        return false;
    }

    return widget->rect().contains(widget->mapFromGlobal(global_pos));
}

}  // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle(QString("%1 %2")
                       .arg(to_qstring(utsure::core::BuildInfo::project_name()))
                       .arg(to_qstring(utsure::core::BuildInfo::project_version())));
    resize(1240, 680);
    setMinimumSize(960, 500);
    setAcceptDrops(true);

    setStyleSheet(R"(
QWidget {
    background: #0b0b0e;
    color: #e2e2e4;
    font-family: "Segoe UI";
    font-size: 12px;
}
QWidget[chromeTransparent="true"] {
    background: transparent;
}
QToolTip {
    background: #16161d;
    color: #f4f4f8;
    border: 1px solid #b241ff;
    padding: 4px 6px;
}
QFrame#ToolbarFrame,
QFrame#PanelFrame {
    background: #14141a;
    border: 1px solid #2a2a35;
    border-radius: 6px;
}
QFrame#ToolbarFrame {
    border-top: 2px solid #ffce2e;
}
QFrame#PreviewSurface {
    background: #050508;
    border: 1px solid #2a2a35;
    border-radius: 6px;
}
QWidget#SourceDropOverlay {
    background: rgba(6, 6, 9, 204);
}
QFrame#SourceDropOverlayCard {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 rgba(24, 24, 32, 248), stop:1 rgba(16, 16, 22, 244));
    border: 1px solid rgba(255, 206, 46, 110);
    border-radius: 24px;
}
QFrame#SourceDropOverlayIllustration {
    background: qradialgradient(cx:0.35, cy:0.3, radius:0.85, fx:0.3, fy:0.28, stop:0 rgba(199, 108, 255, 240), stop:1 rgba(255, 206, 46, 220));
    border: 1px solid rgba(255, 206, 46, 200);
    border-radius: 68px;
}
QLabel#SourceDropOverlayEyebrow {
    background: transparent;
    color: #ffce2e;
    font-size: 11px;
    font-weight: 800;
    letter-spacing: 1.6px;
}
QLabel#SourceDropOverlayIcon {
    background: transparent;
    color: #111111;
    font-size: 36px;
    font-weight: 900;
}
QLabel#SourceDropOverlayTitle {
    background: transparent;
    color: #f6f6fb;
    font-size: 24px;
    font-weight: 900;
}
QLabel#SourceDropOverlayBody {
    background: transparent;
    color: #bbbcc8;
    font-size: 13px;
}
QFrame#PreviewTransportBar {
    background: rgba(20, 20, 26, 232);
    border: 1px solid #4c4120;
    border-radius: 6px;
}
QWidget#PreviewTabCorner {
    background: transparent;
}
QLabel#BrandMark {
    min-width: 30px;
    min-height: 30px;
    max-width: 30px;
    max-height: 30px;
    background: transparent;
    border: none;
    padding: 0;
}
QLabel#BrandMark[logoFallback="true"] {
    border-radius: 6px;
    background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #2c1044, stop:1 #b241ff);
    border: 1px solid #1b1225;
    color: #ffce2e;
    font-family: "Yu Gothic UI", "Yu Gothic", "Meiryo UI", "Meiryo", "Segoe UI";
    font-size: 16px;
    font-weight: 900;
}
QLabel#BrandName {
    color: #b241ff;
    font-size: 17px;
    font-weight: 900;
}
QLabel#BrandSubtitle,
QLabel#MutedNote,
QLabel#DetailLabel,
QLabel#PreviewContextLabel,
QLabel#TaskLogSummaryLabel {
    color: #8b8b99;
}
QToolButton[toolbarButton="true"] {
    background: #1e1e26;
    border: 1px solid #2a2a35;
    border-radius: 4px;
    padding: 0;
    color: #e2e2e4;
}
QToolButton[toolbarButton="true"][iconFallback="true"] {
    padding: 0 10px;
}
QToolButton[toolbarButton="true"]:hover,
QPushButton[browseButton="true"]:hover,
QPushButton#TimelineButton:hover,
QPushButton:hover {
    background: #2a2a35;
}
QToolButton[toolbarButton="true"]:pressed,
QPushButton#TimelineButton:pressed,
QPushButton:pressed {
    background: #31313c;
}
QToolButton[toolbarButton="true"]:checked,
QPushButton#TimelineButton:checked {
    background: #2b2234;
    border: 1px solid #5c3e7b;
}
QToolButton[toolbarButton="true"]:disabled,
QPushButton:disabled,
QLineEdit:disabled,
QComboBox:disabled,
QSpinBox:disabled,
QCheckBox:disabled {
    color: #6b6b79;
    border-color: #23232d;
}
QPushButton[browseButton="true"] {
    background: #ffce2e;
    border: 1px solid #cca524;
    border-radius: 3px;
    color: #111111;
    padding: 0;
}
QPushButton[browseButton="true"][iconFallback="true"] {
    padding: 0 8px;
}
QPushButton {
    background: #1e1e26;
    border: 1px solid #2a2a35;
    border-radius: 4px;
    color: #e2e2e4;
    min-height: 28px;
    padding: 0 10px;
}
QPushButton#TimelineButton {
    background: #1e1e26;
    border: 1px solid #2a2a35;
    border-radius: 4px;
    color: #e2e2e4;
    min-height: 24px;
    padding: 0 7px;
}
QLineEdit,
QComboBox,
QSpinBox,
QPlainTextEdit {
    background: #0d0d12;
    border: 1px solid #3a3a48;
    border-radius: 4px;
    color: #e2e2e4;
}
QTableWidget {
    background: #14141a;
    color: #e2e2e4;
}
QLineEdit,
QComboBox,
QSpinBox {
    background: #0b0b0e;
    padding-right: 28px;
}
QLineEdit:focus,
QComboBox:focus,
QSpinBox:focus,
QPlainTextEdit:focus {
    border: 1px solid #b241ff;
}
QComboBox::drop-down {
    subcontrol-origin: padding;
    subcontrol-position: top right;
    width: 22px;
    border: none;
    border-left: 1px solid #2a2a35;
    background: #1a1a22;
    border-top-right-radius: 4px;
    border-bottom-right-radius: 4px;
}
QComboBox::down-arrow {
    image: url(:/icons/chevron-down.svg);
    width: 10px;
    height: 10px;
}
QComboBox::down-arrow:on {
    top: 1px;
}
QSpinBox::up-button,
QSpinBox::down-button {
    subcontrol-origin: border;
    width: 18px;
    background: #1a1a22;
    border-left: 1px solid #2a2a35;
}
QSpinBox::up-button {
    subcontrol-position: top right;
    border-top-right-radius: 4px;
    border-bottom: 1px solid #2a2a35;
}
QSpinBox::down-button {
    subcontrol-position: bottom right;
    border-bottom-right-radius: 4px;
}
QSpinBox::up-arrow {
    image: url(:/icons/chevron-up.svg);
    width: 8px;
    height: 8px;
}
QSpinBox::down-arrow {
    image: url(:/icons/chevron-down.svg);
    width: 8px;
    height: 8px;
}
QComboBox QAbstractItemView {
    background: #16161d;
    border: 1px solid #3a3a48;
    selection-background-color: #2c163d;
    selection-color: #f4f4f8;
    color: #e2e2e4;
}
QTabWidget::pane {
    background: #14141a;
    border: 1px solid #2a2a35;
    border-radius: 4px;
}
QTableWidget#QueueTable {
    border: none;
    border-radius: 0;
    alternate-background-color: #191920;
    selection-background-color: #2c163d;
    selection-color: #f4f4f8;
    outline: 0;
}
QTableWidget#QueueTable::item {
    border: none;
}
QTableWidget#QueueTable::item:selected,
QTableWidget#QueueTable::item:selected:active,
QTableWidget#QueueTable::item:selected:!active {
    background: #2c163d;
    color: #f4f4f8;
}
QLineEdit,
QComboBox,
QSpinBox {
    min-height: 28px;
    padding: 4px 8px;
}
QCheckBox {
    spacing: 6px;
    background: transparent;
    color: #d8d8de;
}
QCheckBox::indicator {
    width: 14px;
    height: 14px;
    border-radius: 3px;
    border: 1px solid #3a3a48;
    background: #0b0b0e;
}
QCheckBox::indicator:checked {
    background: #ffce2e;
    border: 1px solid #ffce2e;
}
QGroupBox {
    background: #15151c;
    border: 1px solid #3a3a48;
    border-radius: 6px;
    margin-top: 6px;
    padding-top: 12px;
    font-weight: 700;
    color: #eeeef2;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 8px;
    padding: 0 4px;
}
QTabBar::tab {
    background: #111116;
    border: 1px solid #2a2a35;
    border-bottom: none;
    border-top-left-radius: 6px;
    border-top-right-radius: 6px;
    padding: 6px 10px;
    margin-right: 3px;
    color: #8b8b99;
}
QTabBar::tab:hover {
    background: #1a1a22;
}
QTabBar::tab:selected {
    background: #14141a;
    color: #ffffff;
    border-top: 2px solid #b241ff;
    padding-top: 5px;
}
QHeaderView::section {
    background: #1a1a22;
    color: #f1f1f4;
    border: none;
    border-bottom: 1px solid #2a2a35;
    padding: 6px;
    font-weight: 700;
}
QSplitter::handle {
    background: #1b1b23;
}
QSplitter::handle:horizontal {
    width: 6px;
}
QSplitter::handle:vertical {
    height: 6px;
}
QLabel#PreviewTitleLabel {
    color: #f4f4f8;
    font-size: 16px;
    font-weight: 900;
    letter-spacing: 0.7px;
}
QLabel#PreviewContextLabel {
    color: #8b8b99;
}
QLabel#PreviewTrimBadge,
QLabel#PreviewTimeBadge {
    border-radius: 10px;
    padding: 2px 6px;
    font-family: Consolas, "Courier New", monospace;
}
QLabel#PreviewTrimBadge {
    background: #1a1a22;
    border: 1px solid #5d4b15;
    color: #ffce2e;
    font-weight: 700;
}
QLabel#PreviewTimeBadge {
    background: rgba(0, 0, 0, 180);
    color: #ffce2e;
    border: 1px solid rgba(255, 255, 255, 25);
}
)");

    preview_audio_controller_ = new PreviewAudioController(this);
    preview_renderer_controller_ = new PreviewFrameRendererController(this);
    busy_spinner_timer_ = new QTimer(this);
    busy_spinner_timer_->setInterval(90);
    preview_playback_timer_ = new QTimer(this);
    preview_playback_timer_->setInterval(33);
    preview_playback_timer_->setTimerType(Qt::PreciseTimer);
    refresh_parallel_batch_summary();

    auto *central_widget = new QWidget(this);
    auto *root_layout = new QVBoxLayout(central_widget);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    auto *content_widget = new QWidget(central_widget);
    auto *content_layout = new QVBoxLayout(content_widget);
    content_layout->setContentsMargins(8, 8, 8, 8);
    content_layout->setSpacing(8);

    auto *toolbar_frame = new QFrame(content_widget);
    toolbar_frame->setObjectName("ToolbarFrame");
    auto *toolbar_layout = new QGridLayout(toolbar_frame);
    toolbar_layout->setContentsMargins(10, 8, 10, 8);
    toolbar_layout->setSpacing(6);
    toolbar_layout->setColumnStretch(0, 1);
    toolbar_layout->setColumnStretch(2, 1);

    add_button_ = create_toolbar_button(":/icons/add.svg", "Add", "Add source jobs", toolbar_frame);
    remove_button_ = create_toolbar_button(":/icons/remove.svg", "Remove", "Remove selected job", toolbar_frame);
    settings_button_ = create_toolbar_button(":/icons/settings.svg", "Settings", "Settings", toolbar_frame);
    info_button_ = create_toolbar_button(":/icons/info.svg", "Info", "Info", toolbar_frame);

    auto *toolbar_left_widget = new QWidget(toolbar_frame);
    toolbar_left_widget->setProperty("chromeTransparent", true);
    auto *toolbar_left_layout = new QHBoxLayout(toolbar_left_widget);
    toolbar_left_layout->setContentsMargins(0, 0, 0, 0);
    toolbar_left_layout->setSpacing(6);

    auto *toolbar_brand_widget = new QWidget(toolbar_frame);
    toolbar_brand_widget->setProperty("chromeTransparent", true);
    auto *brand_layout = new QHBoxLayout(toolbar_brand_widget);
    brand_layout->setContentsMargins(4, 0, 4, 0);
    brand_layout->setSpacing(8);

    auto *brand_mark = new QLabel(QString::fromUtf8(u8"\u5199"), toolbar_brand_widget);
    brand_mark->setObjectName("BrandMark");
    brand_mark->setAlignment(Qt::AlignCenter);
    QString brand_logo_failure_reason{};
    const QIcon brand_logo_icon = load_resource_icon(":/icons/logo.svg", QSize(30, 30), &brand_logo_failure_reason);
    if (!brand_logo_icon.isNull()) {
        brand_mark->setPixmap(brand_logo_icon.pixmap(QSize(30, 30)));
        brand_mark->setText(QString{});
        brand_mark->setProperty("logoFallback", false);
    } else {
        brand_mark->setProperty("logoFallback", true);
        qWarning().noquote()
            << QString("Brand logo fallback active for ':/icons/logo.svg': %1").arg(brand_logo_failure_reason);
    }
    refresh_widget_style(brand_mark);

    auto *brand_name = new QLabel("utsure", toolbar_brand_widget);
    brand_name->setObjectName("BrandName");
    brand_layout->addWidget(brand_mark);
    brand_layout->addWidget(brand_name);
    toolbar_left_layout->addWidget(add_button_);
    toolbar_left_layout->addWidget(remove_button_);
    toolbar_left_layout->addWidget(settings_button_);
    toolbar_left_layout->addWidget(info_button_);

    priority_combo_ = new QComboBox(toolbar_frame);
    priority_combo_->addItem("Low", static_cast<int>(utsure::core::job::EncodeJobProcessPriority::low));
    priority_combo_->addItem(
        "Below Normal",
        static_cast<int>(utsure::core::job::EncodeJobProcessPriority::below_normal)
    );
    priority_combo_->addItem("Normal", static_cast<int>(utsure::core::job::EncodeJobProcessPriority::normal));
    priority_combo_->addItem(
        "Above Normal",
        static_cast<int>(utsure::core::job::EncodeJobProcessPriority::above_normal)
    );
    priority_combo_->addItem("High", static_cast<int>(utsure::core::job::EncodeJobProcessPriority::high));
    priority_combo_->setCurrentIndex(priority_combo_->findData(
        static_cast<int>(utsure::core::job::EncodeJobProcessPriority::below_normal)
    ));
    priority_combo_->setMinimumWidth(148);

    parallel_button_ = new QToolButton(toolbar_frame);
    parallel_button_->setProperty("toolbarButton", true);
    parallel_button_->setCursor(Qt::PointingHandCursor);
    parallel_button_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    parallel_button_->setText("Parallel");
    parallel_button_->setCheckable(true);
    parallel_button_->setMinimumWidth(82);
    parallel_button_->setMaximumWidth(QWIDGETSIZE_MAX);
    parallel_button_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    parallel_button_->setFixedHeight(30);

    start_button_ = create_toolbar_button(":/icons/play.svg", "Start", "Start checked jobs", toolbar_frame);
    stop_button_ = create_toolbar_button(":/icons/stop.svg", "Stop", "Stop active jobs", toolbar_frame);
    auto *toolbar_right_widget = new QWidget(toolbar_frame);
    toolbar_right_widget->setProperty("chromeTransparent", true);
    auto *toolbar_right_layout = new QHBoxLayout(toolbar_right_widget);
    toolbar_right_layout->setContentsMargins(0, 0, 0, 0);
    toolbar_right_layout->setSpacing(6);
    toolbar_right_layout->addStretch(1);
    toolbar_right_layout->addWidget(parallel_button_);
    toolbar_right_layout->addWidget(priority_combo_);
    toolbar_right_layout->addWidget(start_button_);
    toolbar_right_layout->addWidget(stop_button_);

    toolbar_layout->addWidget(toolbar_left_widget, 0, 0, Qt::AlignLeft | Qt::AlignVCenter);
    toolbar_layout->addWidget(toolbar_brand_widget, 0, 1, Qt::AlignCenter);
    toolbar_layout->addWidget(toolbar_right_widget, 0, 2, Qt::AlignRight | Qt::AlignVCenter);
    content_layout->addWidget(toolbar_frame);

    auto *body_splitter = new QSplitter(Qt::Vertical, content_widget);
    body_splitter->setChildrenCollapsible(false);

    auto *top_section = new QWidget(body_splitter);
    top_section->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto *top_section_layout = new QVBoxLayout(top_section);
    top_section_layout->setContentsMargins(0, 0, 0, 0);
    top_section_layout->setSpacing(6);

    auto *queue_row = new QWidget(top_section);
    queue_row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto *queue_row_layout = new QHBoxLayout(queue_row);
    queue_row_layout->setContentsMargins(0, 0, 0, 0);
    queue_row_layout->setSpacing(8);

    auto *queue_frame = new QFrame(queue_row);
    queue_frame->setObjectName("PanelFrame");
    queue_frame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto *queue_frame_layout = new QVBoxLayout(queue_frame);
    queue_frame_layout->setContentsMargins(6, 4, 6, 4);

    queue_table_ = new QTableWidget(queue_frame);
    queue_table_->setObjectName("QueueTable");
    queue_table_->setColumnCount(6);
    queue_table_->setHorizontalHeaderLabels(QStringList{
        "File",
        "Type",
        "Status",
        "EFPS",
        "Speed",
        "Output"
    });
    queue_table_->verticalHeader()->setVisible(false);
    queue_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    queue_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    queue_table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    queue_table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
    queue_table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
    queue_table_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Fixed);
    queue_table_->setColumnWidth(1, 78);
    queue_table_->setColumnWidth(2, 94);
    queue_table_->setColumnWidth(3, 64);
    queue_table_->setColumnWidth(4, 72);
    queue_table_->setColumnWidth(5, 180);
    queue_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    queue_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    queue_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    queue_table_->setShowGrid(false);
    queue_table_->setAlternatingRowColors(true);
    queue_table_->setFrameShape(QFrame::NoFrame);
    queue_table_->setFocusPolicy(Qt::NoFocus);
    queue_table_->setMinimumHeight(0);
    queue_table_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    queue_table_->horizontalHeader()->setHighlightSections(false);
    queue_table_->verticalHeader()->setDefaultSectionSize(24);
    queue_table_->setTextElideMode(Qt::ElideMiddle);
    queue_table_->setWordWrap(false);
    queue_frame_layout->addWidget(queue_table_, 1);

    auto *details_frame = new QFrame(queue_row);
    details_frame->setObjectName("PanelFrame");
    details_frame->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    details_frame->setMinimumWidth(230);
    auto *details_layout = new QFormLayout(details_frame);
    details_layout->setContentsMargins(8, 8, 8, 8);
    details_layout->setHorizontalSpacing(8);
    details_layout->setVerticalSpacing(4);

    const auto make_detail_label = [details_frame]() {
        auto *label = new QLabel("--", details_frame);
        label->setWordWrap(true);
        return label;
    };

    detail_status_value_ = make_detail_label();
    detail_elapsed_value_ = make_detail_label();
    detail_remaining_value_ = make_detail_label();
    detail_efps_value_ = make_detail_label();
    detail_speed_value_ = make_detail_label();
    detail_input_size_value_ = make_detail_label();
    detail_output_size_value_ = make_detail_label();
    detail_timeline_value_ = make_detail_label();

    const auto make_detail_name = [details_frame](const QString &text) {
        auto *label = new QLabel(text, details_frame);
        label->setObjectName("DetailLabel");
        return label;
    };

    details_layout->addRow(make_detail_name("Status"), detail_status_value_);
    details_layout->addRow(make_detail_name("Elapsed"), detail_elapsed_value_);
    details_layout->addRow(make_detail_name("Remaining"), detail_remaining_value_);
    details_layout->addRow(make_detail_name("EFPS"), detail_efps_value_);
    details_layout->addRow(make_detail_name("Speed"), detail_speed_value_);
    details_layout->addRow(make_detail_name("Input Size"), detail_input_size_value_);
    details_layout->addRow(make_detail_name("Output Size"), detail_output_size_value_);
    details_layout->addRow(make_detail_name("Timeline"), detail_timeline_value_);

    queue_row_layout->addWidget(queue_frame, 6);
    queue_row_layout->addWidget(details_frame, 2);
    top_section_layout->addWidget(queue_row, 1);

    auto *output_frame = new QFrame(top_section);
    output_frame->setObjectName("PanelFrame");
    auto *output_layout = new QVBoxLayout(output_frame);
    output_layout->setContentsMargins(8, 5, 8, 5);
    output_layout->setSpacing(3);

    auto *output_row = new QHBoxLayout();
    output_row->setContentsMargins(0, 0, 0, 0);
    output_row->setSpacing(8);

    auto *output_label = new QLabel("Output", output_frame);
    output_label->setMinimumWidth(52);
    output_path_edit_ = new QLineEdit(output_frame);
    output_path_edit_->setPlaceholderText("Output path");
    output_auto_button_ = new QPushButton("Auto", output_frame);
    output_auto_button_->setToolTip("Restore the generated default output path");
    output_auto_button_->setCursor(Qt::PointingHandCursor);
    output_browse_button_ = new QPushButton(output_frame);
    output_browse_button_->setProperty("browseButton", true);
    apply_icon_or_text(output_browse_button_, ":/icons/browse.svg", "...", QSize(15, 15), 30, 30, false);
    same_as_input_check_ = new QCheckBox("Same as input", output_frame);
    same_as_input_check_->setCursor(Qt::PointingHandCursor);

    output_row->addWidget(output_label);
    output_row->addWidget(output_path_edit_, 1);
    output_row->addWidget(output_auto_button_);
    output_row->addWidget(output_browse_button_);
    output_row->addWidget(same_as_input_check_);
    output_layout->addLayout(output_row);

    top_section_layout->addWidget(output_frame);

    auto *content_splitter = new QSplitter(Qt::Horizontal, body_splitter);
    content_splitter->setChildrenCollapsible(false);
    editor_tabs_ = new QTabWidget(content_splitter);
    editor_tabs_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto *main_tab_content = new QWidget(editor_tabs_);
    auto *main_tab_layout = new QVBoxLayout(main_tab_content);
    main_tab_layout->setContentsMargins(8, 8, 8, 8);
    main_tab_layout->setSpacing(8);

    auto *subtitle_group = new QGroupBox("Subtitle", main_tab_content);
    auto *subtitle_layout = new QGridLayout(subtitle_group);
    subtitle_enable_check_ = new QCheckBox("Enable subtitle burn-in", subtitle_group);
    auto *subtitle_path_row = new QWidget(subtitle_group);
    auto *subtitle_path_layout = new QHBoxLayout(subtitle_path_row);
    subtitle_path_layout->setContentsMargins(0, 0, 0, 0);
    subtitle_path_layout->setSpacing(8);
    subtitle_path_edit_ = new QLineEdit(subtitle_path_row);
    subtitle_path_edit_->setPlaceholderText("Automatic or manual subtitle file");
    subtitle_auto_button_ = new QPushButton("Auto", subtitle_path_row);
    subtitle_auto_button_->setToolTip("Restore automatic subtitle selection");
    subtitle_auto_button_->setCursor(Qt::PointingHandCursor);
    subtitle_browse_button_ = new QPushButton(subtitle_path_row);
    subtitle_browse_button_->setProperty("browseButton", true);
    subtitle_browse_button_->setToolTip("Choose subtitle file");
    subtitle_browse_button_->setCursor(Qt::PointingHandCursor);
    apply_icon_or_text(subtitle_browse_button_, ":/icons/browse.svg", "...", QSize(15, 15), 30, 30, false);
    subtitle_path_layout->addWidget(subtitle_path_edit_, 1);
    subtitle_path_layout->addWidget(subtitle_auto_button_);
    subtitle_path_layout->addWidget(subtitle_browse_button_);
    auto *subtitle_note = new QLabel(
        "Automatic selection checks only the source folder for an exact stem match or an exact '.fx' subtitle variant.",
        subtitle_group
    );
    subtitle_note->setObjectName("MutedNote");
    subtitle_note->setWordWrap(true);
    subtitle_layout->addWidget(subtitle_enable_check_, 0, 0, 1, 2);
    subtitle_layout->addWidget(new QLabel("Subtitle file", subtitle_group), 1, 0);
    subtitle_layout->addWidget(subtitle_path_row, 1, 1);
    subtitle_layout->addWidget(subtitle_note, 2, 0, 1, 2);
    main_tab_layout->addWidget(subtitle_group);

    auto *intro_group = new QGroupBox("Intro", main_tab_content);
    auto *intro_layout = new QGridLayout(intro_group);
    intro_enable_check_ = new QCheckBox("Enable intro clip", intro_group);
    const auto intro_row = create_path_field(intro_group, "Optional intro media", "Choose intro media");
    intro_path_edit_ = intro_row.line_edit;
    intro_browse_button_ = intro_row.browse_button;
    intro_music_check_ = new QCheckBox("Use separate intro music", intro_group);
    const auto intro_music_row = create_path_field(intro_group, "Optional intro music", "Choose intro music");
    intro_music_path_edit_ = intro_music_row.line_edit;
    intro_music_browse_button_ = intro_music_row.browse_button;
    auto *intro_note = new QLabel("Only meaningful when the intro asset itself has no audio.", intro_group);
    intro_note->setObjectName("MutedNote");
    intro_note->setWordWrap(true);
    intro_layout->addWidget(intro_enable_check_, 0, 0, 1, 2);
    intro_layout->addWidget(new QLabel("Intro media", intro_group), 1, 0);
    intro_layout->addWidget(intro_row.container, 1, 1);
    intro_layout->addWidget(intro_music_check_, 2, 0, 1, 2);
    intro_layout->addWidget(new QLabel("Intro music", intro_group), 3, 0);
    intro_layout->addWidget(intro_music_row.container, 3, 1);
    intro_layout->addWidget(intro_note, 4, 0, 1, 2);
    main_tab_layout->addWidget(intro_group);

    auto *endcard_group = new QGroupBox("EndCard", main_tab_content);
    auto *endcard_layout = new QGridLayout(endcard_group);
    endcard_enable_check_ = new QCheckBox("Enable endcard clip", endcard_group);
    const auto endcard_row = create_path_field(endcard_group, "Optional endcard media", "Choose endcard media");
    endcard_path_edit_ = endcard_row.line_edit;
    endcard_browse_button_ = endcard_row.browse_button;
    endcard_music_check_ = new QCheckBox("Use separate endcard music", endcard_group);
    const auto endcard_music_row = create_path_field(endcard_group, "Optional endcard music", "Choose endcard music");
    endcard_music_path_edit_ = endcard_music_row.line_edit;
    endcard_music_browse_button_ = endcard_music_row.browse_button;
    auto *endcard_note = new QLabel("Only meaningful when the endcard asset itself has no audio.", endcard_group);
    endcard_note->setObjectName("MutedNote");
    endcard_note->setWordWrap(true);
    endcard_layout->addWidget(endcard_enable_check_, 0, 0, 1, 2);
    endcard_layout->addWidget(new QLabel("Endcard media", endcard_group), 1, 0);
    endcard_layout->addWidget(endcard_row.container, 1, 1);
    endcard_layout->addWidget(endcard_music_check_, 2, 0, 1, 2);
    endcard_layout->addWidget(new QLabel("Endcard music", endcard_group), 3, 0);
    endcard_layout->addWidget(endcard_music_row.container, 3, 1);
    endcard_layout->addWidget(endcard_note, 4, 0, 1, 2);
    main_tab_layout->addWidget(endcard_group);

    auto *main_tab_note = new QLabel(
        "Timeline trim applies only to the main source. Thumbnail pre-roll, intro, and endcard sit outside that trim range.",
        main_tab_content
    );
    main_tab_note->setObjectName("MutedNote");
    main_tab_note->setWordWrap(true);
    main_tab_layout->addWidget(main_tab_note);
    main_tab_layout->addStretch(1);
    editor_tabs_->addTab(wrap_in_scroll_area(main_tab_content, editor_tabs_), "Main");

    auto *encode_tab_content = new QWidget(editor_tabs_);
    auto *encode_tab_layout = new QVBoxLayout(encode_tab_content);
    encode_tab_layout->setContentsMargins(8, 8, 8, 8);
    encode_tab_layout->setSpacing(8);
    auto *encode_settings_row = new QHBoxLayout();
    encode_settings_row->setContentsMargins(0, 0, 0, 0);
    encode_settings_row->setSpacing(8);

    auto *video_group = new QGroupBox("Video", encode_tab_content);
    auto *video_layout = new QFormLayout(video_group);
    video_codec_combo_ = new QComboBox(video_group);
    video_codec_combo_->addItem("H.265", static_cast<int>(utsure::core::media::OutputVideoCodec::h265));
    video_codec_combo_->addItem("H.264", static_cast<int>(utsure::core::media::OutputVideoCodec::h264));
    preset_combo_ = new QComboBox(video_group);
    preset_combo_->addItems(QStringList{
        "ultrafast",
        "superfast",
        "veryfast",
        "faster",
        "fast",
        "medium",
        "slow",
        "slower",
        "veryslow"
    });
    crf_spin_box_ = new QSpinBox(video_group);
    crf_spin_box_->setRange(0, 51);
    video_layout->addRow("Codec", video_codec_combo_);
    video_layout->addRow("CRF", crf_spin_box_);
    video_layout->addRow("Preset", preset_combo_);

    auto *audio_group = new QGroupBox("Audio", encode_tab_content);
    auto *audio_layout = new QFormLayout(audio_group);
    audio_format_combo_ = new QComboBox(audio_group);
    audio_format_combo_->addItem("AAC", static_cast<int>(utsure::core::media::AudioOutputMode::encode_aac));
    audio_format_combo_->addItem(
        "Copy source",
        static_cast<int>(utsure::core::media::AudioOutputMode::copy_source)
    );
    audio_quality_combo_ = new QComboBox(audio_group);
    audio_quality_combo_->addItem("128 kbps", 128);
    audio_quality_combo_->addItem("160 kbps", 160);
    audio_quality_combo_->addItem("192 kbps", 192);
    audio_quality_combo_->addItem("256 kbps", 256);
    audio_quality_combo_->addItem("320 kbps", 320);
    audio_track_combo_ = new QComboBox(audio_group);
    audio_layout->addRow("Format", audio_format_combo_);
    audio_layout->addRow("Quality", audio_quality_combo_);
    audio_layout->addRow("Track", audio_track_combo_);
    auto *audio_note = new QLabel(
        "Track selection is UI-only for now. The current backend still encodes the primary detected source track.",
        audio_group
    );
    audio_note->setObjectName("MutedNote");
    audio_note->setWordWrap(true);
    audio_layout->addRow(audio_note);

    auto *output_naming_group = new QGroupBox("Output Naming", encode_tab_content);
    auto *output_naming_layout = new QFormLayout(output_naming_group);
    output_name_text_edit_ = new QLineEdit(output_naming_group);
    output_name_text_edit_->setPlaceholderText("Custom text, no brackets");
    output_name_text_edit_->setClearButtonEnabled(true);
    auto *output_naming_note = new QLabel(
        "Used only for the generated default file name. Leave empty to omit the bracketed prefix.",
        output_naming_group
    );
    output_naming_note->setObjectName("MutedNote");
    output_naming_note->setWordWrap(true);
    output_naming_layout->addRow("Custom text", output_name_text_edit_);
    output_naming_layout->addRow(output_naming_note);

    encode_settings_row->addWidget(video_group, 1);
    encode_settings_row->addWidget(audio_group, 1);
    encode_tab_layout->addLayout(encode_settings_row);
    encode_tab_layout->addWidget(output_naming_group);
    encode_tab_layout->addStretch(1);
    editor_tabs_->addTab(wrap_in_scroll_area(encode_tab_content, editor_tabs_), "Encode");

    auto *special_tab_content = new QWidget(editor_tabs_);
    auto *special_tab_layout = new QVBoxLayout(special_tab_content);
    special_tab_layout->setContentsMargins(8, 8, 8, 8);
    special_tab_layout->setSpacing(8);

    auto *thumbnail_group = new QGroupBox("Thumbnail Pre-roll", special_tab_content);
    auto *thumbnail_layout = new QGridLayout(thumbnail_group);
    const auto thumbnail_row = create_path_field(
        thumbnail_group,
        "Temporary placeholder for ./thumbnail.* or generated black frame",
        "Choose thumbnail image"
    );
    thumbnail_image_path_edit_ = thumbnail_row.line_edit;
    thumbnail_image_browse_button_ = thumbnail_row.browse_button;
    thumbnail_title_edit_ = new QLineEdit(thumbnail_group);
    thumbnail_title_edit_->setPlaceholderText("Temporary placeholder for thumbnail.ass Actor=TNEPTITLE");
    thumbnail_load_ass_button_ = new QPushButton("Load thumbnail.ass (placeholder)", thumbnail_group);
    thumbnail_edit_title_button_ = new QPushButton("Edit title source (placeholder)", thumbnail_group);
    thumbnail_layout->addWidget(new QLabel("Picture", thumbnail_group), 0, 0);
    thumbnail_layout->addWidget(thumbnail_row.container, 0, 1);
    thumbnail_layout->addWidget(new QLabel("Title", thumbnail_group), 1, 0);
    thumbnail_layout->addWidget(thumbnail_title_edit_, 1, 1);
    thumbnail_layout->addWidget(thumbnail_load_ass_button_, 2, 0);
    thumbnail_layout->addWidget(thumbnail_edit_title_button_, 2, 1);
    auto *thumbnail_note = new QLabel(
        "Temporary placeholder: the thumbnail image/title UI is visible now, but thumbnail.ass loading, editing, and burn-in integration will be wired in a later milestone.",
        thumbnail_group
    );
    thumbnail_note->setObjectName("MutedNote");
    thumbnail_note->setWordWrap(true);
    thumbnail_layout->addWidget(thumbnail_note, 3, 0, 1, 2);
    special_tab_layout->addWidget(thumbnail_group);
    special_tab_layout->addStretch(1);
    editor_tabs_->addTab(wrap_in_scroll_area(special_tab_content, editor_tabs_), "Special");

    auto *logs_tab = new QWidget(editor_tabs_);
    auto *logs_tab_layout = new QVBoxLayout(logs_tab);
    logs_tab_layout->setContentsMargins(8, 8, 8, 8);
    session_log_view_ = new QPlainTextEdit(logs_tab);
    session_log_view_->setReadOnly(true);
    session_log_view_->setLineWrapMode(QPlainTextEdit::NoWrap);
    session_log_view_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    logs_tab_layout->addWidget(session_log_view_);
    editor_tabs_->addTab(logs_tab, "Logs");

    auto *right_tabs = new QTabWidget(content_splitter);
    right_tabs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto *preview_corner_widget = new QWidget(right_tabs);
    preview_corner_widget->setObjectName("PreviewTabCorner");
    auto *preview_corner_layout = new QHBoxLayout(preview_corner_widget);
    preview_corner_layout->setContentsMargins(0, 0, 0, 0);
    preview_corner_layout->setSpacing(8);
    preview_time_badge_ = new QLabel("00:00:00.000", preview_corner_widget);
    preview_time_badge_->setObjectName("PreviewTimeBadge");
    current_time_value_ = preview_time_badge_;
    preview_enabled_check_ = new QCheckBox("Preview", preview_corner_widget);
    preview_enabled_check_->setCursor(Qt::PointingHandCursor);
    preview_corner_layout->addStretch(1);
    preview_corner_layout->addWidget(preview_time_badge_);
    preview_corner_layout->addWidget(preview_enabled_check_);
    right_tabs->setCornerWidget(preview_corner_widget, Qt::TopRightCorner);

    auto *preview_tab = new QWidget(right_tabs);
    auto *preview_tab_layout = new QVBoxLayout(preview_tab);
    preview_tab_layout->setContentsMargins(4, 4, 4, 4);
    preview_tab_layout->setSpacing(4);

    auto *preview_surface = new QFrame(preview_tab);
    preview_surface->setObjectName("PreviewSurface");
    preview_surface->setMinimumHeight(280);
    preview_surface->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto *preview_surface_layout = new QVBoxLayout(preview_surface);
    preview_surface_layout->setContentsMargins(0, 0, 0, 0);
    preview_surface_layout->setSpacing(0);
    preview_surface_widget_ = new PreviewSurfaceWidget(preview_surface);
    preview_surface_layout->addWidget(preview_surface_widget_, 1);

    trim_in_value_ = new QLabel("IN=00:00:00.000", preview_surface_widget_);
    trim_in_value_->setObjectName("PreviewTrimBadge");
    trim_out_value_ = new QLabel("OUT=00:00:00.000", preview_surface_widget_);
    trim_out_value_->setObjectName("PreviewTrimBadge");

    preview_controls_panel_ = new QFrame(preview_surface_widget_);
    preview_controls_panel_->setObjectName("PreviewTransportBar");
    preview_controls_panel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *preview_controls_layout = new QVBoxLayout(preview_controls_panel_);
    preview_controls_layout->setContentsMargins(8, 6, 8, 6);
    preview_controls_layout->setSpacing(6);

    trim_timeline_widget_ = new TrimTimelineWidget(preview_controls_panel_);
    trim_timeline_widget_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    preview_controls_layout->addWidget(trim_timeline_widget_);

    auto *timeline_controls_container = new QWidget(preview_controls_panel_);
    auto *timeline_controls = new QHBoxLayout(timeline_controls_container);
    timeline_controls->setContentsMargins(0, 0, 0, 0);
    timeline_controls->setSpacing(4);

    preview_play_pause_button_ = new QPushButton(timeline_controls_container);
    preview_play_pause_button_->setObjectName("TimelineButton");
    preview_play_pause_button_->setCursor(Qt::PointingHandCursor);
    preview_play_pause_button_->setFocusPolicy(Qt::NoFocus);
    preview_play_pause_button_->setToolTip("Play preview");
    apply_icon_or_text(preview_play_pause_button_, ":/icons/play.svg", "Play", QSize(15, 15), 30, 24, false);

    preview_stop_button_ = new QPushButton(timeline_controls_container);
    preview_stop_button_->setObjectName("TimelineButton");
    preview_stop_button_->setCursor(Qt::PointingHandCursor);
    preview_stop_button_->setFocusPolicy(Qt::NoFocus);
    preview_stop_button_->setToolTip("Stop preview and return to trim in");
    apply_icon_or_text(preview_stop_button_, ":/icons/stop.svg", "Stop", QSize(15, 15), 30, 24, false);

    frame_back_button_ = new QPushButton(timeline_controls_container);
    frame_back_button_->setObjectName("TimelineButton");
    frame_back_button_->setCursor(Qt::PointingHandCursor);
    frame_back_button_->setFocusPolicy(Qt::NoFocus);
    frame_back_button_->setToolTip("Previous frame");
    apply_icon_or_text(frame_back_button_, ":/icons/frame-back.svg", "<", QSize(14, 14), 30, 24, false);
    frame_back_button_->hide();
    frame_forward_button_ = new QPushButton(timeline_controls_container);
    frame_forward_button_->setObjectName("TimelineButton");
    frame_forward_button_->setCursor(Qt::PointingHandCursor);
    frame_forward_button_->setFocusPolicy(Qt::NoFocus);
    frame_forward_button_->setToolTip("Next frame");
    apply_icon_or_text(frame_forward_button_, ":/icons/frame-forward.svg", ">", QSize(14, 14), 30, 24, false);
    frame_forward_button_->hide();
    set_in_button_ = new QPushButton(timeline_controls_container);
    set_in_button_->setObjectName("TimelineButton");
    set_in_button_->setCursor(Qt::PointingHandCursor);
    set_in_button_->setFocusPolicy(Qt::NoFocus);
    set_in_button_->setToolTip("Set trim in to the current preview time");
    apply_icon_or_text(set_in_button_, ":/icons/set-in.svg", "Set IN", QSize(15, 15), 34, 24, true);
    set_out_button_ = new QPushButton(timeline_controls_container);
    set_out_button_->setObjectName("TimelineButton");
    set_out_button_->setCursor(Qt::PointingHandCursor);
    set_out_button_->setFocusPolicy(Qt::NoFocus);
    set_out_button_->setToolTip("Set trim out to the current preview time");
    apply_icon_or_text(set_out_button_, ":/icons/set-out.svg", "Set OUT", QSize(15, 15), 34, 24, true);
    jump_in_button_ = new QPushButton(timeline_controls_container);
    jump_in_button_->setObjectName("TimelineButton");
    jump_in_button_->setCursor(Qt::PointingHandCursor);
    jump_in_button_->setFocusPolicy(Qt::NoFocus);
    jump_in_button_->setToolTip("Jump preview to trim in");
    apply_icon_or_text(jump_in_button_, ":/icons/jump-in.svg", "To IN", QSize(15, 15), 36, 24, true);
    jump_out_button_ = new QPushButton(timeline_controls_container);
    jump_out_button_->setObjectName("TimelineButton");
    jump_out_button_->setCursor(Qt::PointingHandCursor);
    jump_out_button_->setFocusPolicy(Qt::NoFocus);
    jump_out_button_->setToolTip("Jump preview to trim out");
    apply_icon_or_text(jump_out_button_, ":/icons/jump-out.svg", "To OUT", QSize(15, 15), 36, 24, true);
    timeline_controls->addWidget(preview_play_pause_button_);
    timeline_controls->addWidget(preview_stop_button_);
    timeline_controls->addWidget(jump_in_button_);
    timeline_controls->addWidget(set_in_button_);
    timeline_controls->addWidget(set_out_button_);
    timeline_controls->addWidget(jump_out_button_);
    timeline_controls->addStretch(1);
    timeline_controls->addWidget(trim_in_value_);
    timeline_controls->addWidget(trim_out_value_);
    preview_controls_layout->addWidget(timeline_controls_container);
    preview_controls_panel_->setFixedHeight(74);
    preview_controls_panel_->hide();

    preview_tab_layout->addWidget(preview_surface, 1);

    preview_surface_widget_->installEventFilter(this);
    preview_controls_panel_->installEventFilter(this);
    right_tabs->addTab(preview_tab, "Preview");

    auto *task_log_tab = new QWidget(right_tabs);
    auto *task_log_layout = new QVBoxLayout(task_log_tab);
    task_log_layout->setContentsMargins(8, 8, 8, 8);
    task_log_summary_label_ = new QLabel("Select a job to inspect its task log.", task_log_tab);
    task_log_summary_label_->setObjectName("TaskLogSummaryLabel");
    task_log_summary_label_->setWordWrap(true);
    task_log_view_ = new QPlainTextEdit(task_log_tab);
    task_log_view_->setReadOnly(true);
    task_log_view_->setLineWrapMode(QPlainTextEdit::NoWrap);
    task_log_view_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    task_log_layout->addWidget(task_log_summary_label_);
    task_log_layout->addWidget(task_log_view_, 1);
    right_tabs->addTab(task_log_tab, "Task Log");
    connect(right_tabs, &QTabWidget::currentChanged, this, [this](const int index) {
        if (preview_time_badge_ != nullptr) {
            preview_time_badge_->setVisible(index == 0);
        }
    });
    preview_time_badge_->setVisible(right_tabs->currentIndex() == 0);

    content_splitter->addWidget(editor_tabs_);
    content_splitter->addWidget(right_tabs);
    content_splitter->setStretchFactor(0, 4);
    content_splitter->setStretchFactor(1, 5);

    body_splitter->addWidget(top_section);
    body_splitter->addWidget(content_splitter);
    body_splitter->setStretchFactor(0, 3);
    body_splitter->setStretchFactor(1, 7);
    content_layout->addWidget(body_splitter, 1);

    root_layout->addWidget(content_widget, 1);
    setCentralWidget(central_widget);
    source_drop_overlay_ = new SourceDropOverlayWidget(central_widget);
    update_source_drop_overlay_geometry();

    QTimer::singleShot(0, this, [this, body_splitter, content_splitter]() {
        body_splitter->setSizes(QList<int>{160, 410});
        content_splitter->setSizes(QList<int>{560, 680});
        apply_native_caption_accent(this);
    });

    ensure_runner_slot_count(1);
    connect(
        preview_renderer_controller_,
        &PreviewFrameRendererController::preview_loading,
        this,
        &MainWindow::handle_preview_loading
    );
    connect(
        preview_renderer_controller_,
        &PreviewFrameRendererController::preview_ready,
        this,
        &MainWindow::handle_preview_ready
    );
    connect(
        preview_renderer_controller_,
        &PreviewFrameRendererController::preview_failed,
        this,
        &MainWindow::handle_preview_failed
    );
    connect(preview_audio_controller_, &PreviewAudioController::preview_audio_failed, this, [this](const QString &detail) {
        append_session_log(QString("[warning] Preview audio: %1").arg(detail));
    });
    connect(preview_surface_widget_, &PreviewSurfaceWidget::surface_clicked, this, &MainWindow::handle_preview_surface_clicked);
    connect(preview_surface_widget_, &PreviewSurfaceWidget::frame_step_requested, this, &MainWindow::step_selected_job_frame);
    connect(preview_play_pause_button_, &QPushButton::clicked, this, &MainWindow::handle_preview_play_pause_requested);
    connect(preview_stop_button_, &QPushButton::clicked, this, &MainWindow::handle_preview_stop_requested);
    connect(busy_spinner_timer_, &QTimer::timeout, this, &MainWindow::advance_busy_spinner);
    connect(preview_playback_timer_, &QTimer::timeout, this, &MainWindow::handle_preview_playback_tick);

    connect(add_button_, &QToolButton::clicked, this, &MainWindow::add_source_jobs);
    connect(remove_button_, &QToolButton::clicked, this, &MainWindow::remove_selected_job);
    connect(settings_button_, &QToolButton::clicked, this, &MainWindow::show_settings_placeholder);
    connect(info_button_, &QToolButton::clicked, this, &MainWindow::show_info_dialog);
    connect(parallel_button_, &QToolButton::clicked, this, &MainWindow::show_parallel_settings_dialog);
    connect(start_button_, &QToolButton::clicked, this, &MainWindow::start_encode_queue);
    connect(stop_button_, &QToolButton::clicked, this, &MainWindow::stop_encode_queue);
    connect(same_as_input_check_, &QCheckBox::toggled, this, &MainWindow::handle_same_as_input_toggled);
    connect(preview_enabled_check_, &QCheckBox::toggled, this, &MainWindow::handle_preview_toggled);

    connect(output_auto_button_, &QPushButton::clicked, this, &MainWindow::restore_selected_job_auto_output_path);
    connect(output_browse_button_, &QPushButton::clicked, this, &MainWindow::choose_output_path);
    connect(subtitle_auto_button_, &QPushButton::clicked, this, &MainWindow::restore_selected_job_auto_subtitle_selection);
    connect(subtitle_browse_button_, &QPushButton::clicked, this, &MainWindow::choose_subtitle_file);
    connect(intro_browse_button_, &QPushButton::clicked, this, &MainWindow::choose_intro_clip);
    connect(intro_music_browse_button_, &QPushButton::clicked, this, &MainWindow::choose_intro_music_file);
    connect(endcard_browse_button_, &QPushButton::clicked, this, &MainWindow::choose_endcard_clip);
    connect(endcard_music_browse_button_, &QPushButton::clicked, this, &MainWindow::choose_endcard_music_file);
    connect(thumbnail_image_browse_button_, &QPushButton::clicked, this, &MainWindow::choose_thumbnail_image);
    connect(thumbnail_load_ass_button_, &QPushButton::clicked, this, &MainWindow::show_thumbnail_placeholder_note);
    connect(thumbnail_edit_title_button_, &QPushButton::clicked, this, &MainWindow::show_thumbnail_placeholder_note);

    connect(queue_table_, &QTableWidget::itemChanged, this, &MainWindow::handle_queue_item_changed);
    connect(queue_table_, &QTableWidget::itemSelectionChanged, this, &MainWindow::handle_queue_selection_changed);

    const auto bind_editor_change = [this]() {
        sync_selected_job_from_editor();
    };
    connect(output_name_text_edit_, &QLineEdit::textChanged, this, [bind_editor_change](const QString &) { bind_editor_change(); });
    connect(output_path_edit_, &QLineEdit::textEdited, this, [this](const QString &) {
        if (!is_valid_job_index(selected_job_index_)) {
            return;
        }

        jobs_[static_cast<std::size_t>(selected_job_index_)].output_path_manual_override = true;
    });
    connect(output_path_edit_, &QLineEdit::textChanged, this, [bind_editor_change](const QString &) { bind_editor_change(); });
    connect(subtitle_enable_check_, &QCheckBox::clicked, this, [this](bool) {
        if (!is_valid_job_index(selected_job_index_)) {
            return;
        }

        jobs_[static_cast<std::size_t>(selected_job_index_)].subtitle_manual_override = true;
    });
    connect(subtitle_path_edit_, &QLineEdit::textEdited, this, [this](const QString &) {
        if (!is_valid_job_index(selected_job_index_)) {
            return;
        }

        jobs_[static_cast<std::size_t>(selected_job_index_)].subtitle_manual_override = true;
    });
    connect(subtitle_enable_check_, &QCheckBox::toggled, this, [bind_editor_change](bool) { bind_editor_change(); });
    connect(subtitle_path_edit_, &QLineEdit::textChanged, this, [bind_editor_change](const QString &) { bind_editor_change(); });
    connect(intro_enable_check_, &QCheckBox::toggled, this, [bind_editor_change](bool) { bind_editor_change(); });
    connect(intro_path_edit_, &QLineEdit::textChanged, this, [bind_editor_change](const QString &) { bind_editor_change(); });
    connect(intro_music_check_, &QCheckBox::toggled, this, [bind_editor_change](bool) { bind_editor_change(); });
    connect(intro_music_path_edit_, &QLineEdit::textChanged, this, [bind_editor_change](const QString &) { bind_editor_change(); });
    connect(endcard_enable_check_, &QCheckBox::toggled, this, [bind_editor_change](bool) { bind_editor_change(); });
    connect(endcard_path_edit_, &QLineEdit::textChanged, this, [bind_editor_change](const QString &) { bind_editor_change(); });
    connect(endcard_music_check_, &QCheckBox::toggled, this, [bind_editor_change](bool) { bind_editor_change(); });
    connect(endcard_music_path_edit_, &QLineEdit::textChanged, this, [bind_editor_change](const QString &) { bind_editor_change(); });
    connect(thumbnail_image_path_edit_, &QLineEdit::textChanged, this, [bind_editor_change](const QString &) { bind_editor_change(); });
    connect(thumbnail_title_edit_, &QLineEdit::textChanged, this, [bind_editor_change](const QString &) { bind_editor_change(); });
    connect(video_codec_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [bind_editor_change](int) { bind_editor_change(); });
    connect(preset_combo_, &QComboBox::currentTextChanged, this, [bind_editor_change](const QString &) { bind_editor_change(); });
    connect(crf_spin_box_, qOverload<int>(&QSpinBox::valueChanged), this, [bind_editor_change](int) { bind_editor_change(); });
    connect(audio_format_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, bind_editor_change](int) {
        bind_editor_change();
        refresh_editor_state();
    });
    connect(audio_quality_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [bind_editor_change](int) { bind_editor_change(); });
    connect(audio_track_combo_, &QComboBox::currentTextChanged, this, [bind_editor_change](const QString &) { bind_editor_change(); });

    connect(frame_back_button_, &QPushButton::clicked, this, [this]() { step_selected_job_frame(-1); });
    connect(frame_forward_button_, &QPushButton::clicked, this, [this]() { step_selected_job_frame(1); });
    connect(set_in_button_, &QPushButton::clicked, this, &MainWindow::set_selected_job_trim_in);
    connect(set_out_button_, &QPushButton::clicked, this, &MainWindow::set_selected_job_trim_out);
    connect(jump_in_button_, &QPushButton::clicked, this, &MainWindow::jump_selected_job_to_in);
    connect(jump_out_button_, &QPushButton::clicked, this, &MainWindow::jump_selected_job_to_out);
    connect(trim_timeline_widget_, &TrimTimelineWidget::seek_requested, this, &MainWindow::handle_timeline_seek);

    append_session_log("[info] Window ready.");
    refresh_all_views();
}

QString MainWindow::window_structure_summary() const {
    return QString(
        "Main window structure:\n"
        "- Toolbar: left controls, centered branding, right-side Parallel/priority/start/stop\n"
        "- Queue row: batch queue table plus selected-job details summary\n"
        "- Output strip: output path, auto-name action, and Same as input toggle\n"
        "- Left tabs: Main, Encode, Special, and global Logs, with automatic subtitle selection under Main and custom output naming under Encode\n"
        "- Right tabs: transport-controlled Preview with trim timeline plus selected-task log"
    );
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event) {
    if (event == nullptr) {
        return;
    }

    const bool accepted = can_accept_source_drop(event->mimeData());
    set_source_drop_overlay_visible(accepted);
    if (accepted) {
        event->acceptProposedAction();
        return;
    }

    event->ignore();
}

void MainWindow::dragMoveEvent(QDragMoveEvent *event) {
    if (event == nullptr) {
        return;
    }

    const bool accepted = can_accept_source_drop(event->mimeData());
    set_source_drop_overlay_visible(accepted);
    if (accepted) {
        event->acceptProposedAction();
        return;
    }

    event->ignore();
}

void MainWindow::dragLeaveEvent(QDragLeaveEvent *event) {
    set_source_drop_overlay_visible(false);
    if (event != nullptr) {
        event->accept();
    }
}

void MainWindow::dropEvent(QDropEvent *event) {
    set_source_drop_overlay_visible(false);
    if (event == nullptr) {
        return;
    }

    if (!can_accept_source_drop(event->mimeData())) {
        event->ignore();
        return;
    }

    event->acceptProposedAction();
    const QStringList source_paths = resolve_source_drop_paths(event->mimeData());
    if (!source_paths.isEmpty()) {
        add_source_jobs_from_paths(source_paths);
    }
}

void MainWindow::resizeEvent(QResizeEvent *event) {
    QMainWindow::resizeEvent(event);
    update_source_drop_overlay_geometry();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if ((watched == preview_surface_widget_ || watched == preview_controls_panel_) && event != nullptr) {
        switch (event->type()) {
        case QEvent::Enter:
        case QEvent::MouseMove:
        case QEvent::Show:
        case QEvent::Resize:
            layout_preview_controls_overlay();
            refresh_preview_footer_visibility();
            break;
        case QEvent::Leave:
        case QEvent::Hide:
            QTimer::singleShot(0, this, [this]() { refresh_preview_footer_visibility(); });
            break;
        default:
            break;
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

std::optional<utsure::core::job::EncodeJob> MainWindow::build_job_from_entry(
    const int job_index,
    QString &error_message
) const {
    if (job_index < 0 || job_index >= static_cast<int>(jobs_.size())) {
        error_message = "Select a job before building an encode request.";
        return std::nullopt;
    }

    const auto &entry = jobs_[static_cast<std::size_t>(job_index)];
    if (entry.source_path.trimmed().isEmpty()) {
        error_message = "The selected queue row does not have a source video.";
        return std::nullopt;
    }

    if (entry.output_path.trimmed().isEmpty()) {
        error_message = "Set an output path before queuing this job.";
        return std::nullopt;
    }

    if (entry.video_preset.trimmed().isEmpty()) {
        error_message = "The selected video preset is empty.";
        return std::nullopt;
    }

    utsure::core::job::EncodeJob job{};
    job.input.main_source_path = qstring_to_path(entry.source_path);
    job.output.output_path = qstring_to_path(entry.output_path);
    job.output.video.codec = entry.video_codec;
    job.output.video.preset = entry.video_preset.trimmed().toUtf8().toStdString();
    job.output.video.crf = entry.video_crf;
    job.output.audio.mode = entry.audio_mode;
    job.output.audio.codec = utsure::core::media::OutputAudioCodec::aac;
    job.output.audio.bitrate_kbps = entry.audio_bitrate_kbps;
    job.execution.threading.cpu_usage_mode = utsure::core::media::CpuUsageMode::auto_select;
    job.execution.process_priority = current_worker_priority();

    if (entry.subtitle_enabled && !entry.subtitle_path.trimmed().isEmpty()) {
        job.subtitles = utsure::core::job::EncodeJobSubtitleSettings{
            .subtitle_path = qstring_to_path(entry.subtitle_path),
            .format_hint = subtitle_format_hint_for_path(entry.subtitle_path).toUtf8().toStdString()
        };
    }

    if (entry.intro_enabled && !entry.intro_path.trimmed().isEmpty()) {
        job.input.intro_source_path = qstring_to_path(entry.intro_path);
    }

    if (entry.endcard_enabled && !entry.endcard_path.trimmed().isEmpty()) {
        job.input.outro_source_path = qstring_to_path(entry.endcard_path);
    }

    // Thumbnail pre-roll, trim, and intro/endcard music remain UI-only placeholders in this milestone
    // because encoder-core does not expose the corresponding contracts yet.
    return job;
}

utsure::core::job::OutputNamingRequest MainWindow::build_output_naming_request(const UiEncodeJob &job) const {
    const auto source_path = qstring_to_path(job.source_path);
    const auto current_output_path = qstring_to_path(job.output_path);

    std::filesystem::path output_directory{};
    if (job.same_as_input && !source_path.parent_path().empty()) {
        output_directory = source_path.parent_path();
    } else if (!current_output_path.parent_path().empty()) {
        output_directory = current_output_path.parent_path();
    } else if (!source_path.parent_path().empty()) {
        output_directory = source_path.parent_path();
    }

    const std::string extension_hint = current_output_path.has_extension()
        ? current_output_path.extension().string()
        : std::string{};
    const bool source_audio_known = job.inspected_source_info.has_value();
    const std::optional<utsure::core::media::AudioStreamInfo> source_audio_stream =
        source_audio_known ? job.inspected_source_info->primary_audio_stream : std::nullopt;

    return utsure::core::job::OutputNamingRequest{
        .source_path = source_path,
        .output_directory = output_directory,
        .custom_text = job.output_name_custom_text.trimmed().toUtf8().toStdString(),
        .extension_hint = extension_hint,
        .video_codec = job.video_codec,
        .audio_settings = {
            .mode = job.audio_mode,
            .codec = utsure::core::media::OutputAudioCodec::aac,
            .bitrate_kbps = job.audio_bitrate_kbps
        },
        .source_audio_known = source_audio_known,
        .source_audio_stream = source_audio_stream
    };
}

utsure::core::job::EncodeJobProcessPriority MainWindow::current_worker_priority() const {
    switch (static_cast<utsure::core::job::EncodeJobProcessPriority>(priority_combo_->currentData().toInt())) {
    case utsure::core::job::EncodeJobProcessPriority::high:
        return utsure::core::job::EncodeJobProcessPriority::high;
    case utsure::core::job::EncodeJobProcessPriority::above_normal:
        return utsure::core::job::EncodeJobProcessPriority::above_normal;
    case utsure::core::job::EncodeJobProcessPriority::normal:
        return utsure::core::job::EncodeJobProcessPriority::normal;
    case utsure::core::job::EncodeJobProcessPriority::below_normal:
        return utsure::core::job::EncodeJobProcessPriority::below_normal;
    case utsure::core::job::EncodeJobProcessPriority::low:
    default:
        return utsure::core::job::EncodeJobProcessPriority::low;
    }
}

QString MainWindow::selected_job_name() const {
    if (selected_job_index_ < 0 || selected_job_index_ >= static_cast<int>(jobs_.size())) {
        return "No job selected";
    }

    return queue_source_display_name(jobs_[static_cast<std::size_t>(selected_job_index_)]);
}

QString MainWindow::format_job_state_text(const UiEncodeJob &job) const {
    switch (job.state) {
    case UiJobState::encoding:
        return "Encoding";
    case UiJobState::finished:
        return "Finished";
    case UiJobState::failed:
        return "Failed";
    case UiJobState::canceled:
        return "Canceled";
    case UiJobState::pending:
    default:
        return job_has_minimum_required_fields(job) ? "Ready" : QString();
    }
}

QString MainWindow::format_job_state_display_text(const UiEncodeJob &job) const {
    return display_text_or_fallback(format_job_state_text(job), "Not ready");
}

bool MainWindow::job_is_terminal(const UiEncodeJob &job) const {
    return job.state == UiJobState::finished ||
        job.state == UiJobState::failed ||
        job.state == UiJobState::canceled;
}

bool MainWindow::any_runner_slot_running() const {
    return std::any_of(
        runner_slots_.begin(),
        runner_slots_.end(),
        [](const RunnerSlot &slot) {
            return slot.controller != nullptr && slot.controller->is_running();
        }
    );
}

int MainWindow::active_runner_count() const {
    return static_cast<int>(std::count_if(
        runner_slots_.begin(),
        runner_slots_.end(),
        [](const RunnerSlot &slot) {
            return slot.controller != nullptr && slot.controller->is_running();
        }
    ));
}

int MainWindow::configured_parallel_job_count() const {
    return std::max(parallel_batch_summary_.selected_job_count, 1);
}

int MainWindow::find_free_runner_slot_index() const {
    for (int index = 0; index < static_cast<int>(runner_slots_.size()); ++index) {
        const auto &slot = runner_slots_[static_cast<std::size_t>(index)];
        if (slot.controller != nullptr && !slot.controller->is_running() && slot.active_job_index < 0) {
            return index;
        }
    }

    return -1;
}

QString MainWindow::format_parallel_tooltip() const {
    return QString("Parallel: %1\nJobs: %2\nThreads/job: %3\nBuffer/job: %4")
        .arg(parallel_batch_summary_.enabled ? "On" : "Off")
        .arg(parallel_batch_summary_.selected_job_count)
        .arg(parallel_batch_summary_.threads_per_job)
        .arg(parallel_batch_summary_.video_frame_queue_depth);
}

QString MainWindow::normalized_output_path_key(const QString &path_text) const {
    const auto path = qstring_to_path(path_text.trimmed());
    if (path.empty()) {
        return {};
    }

    std::error_code error{};
    auto absolute_path = std::filesystem::absolute(path, error);
    if (error) {
        absolute_path = path;
    }

#ifdef _WIN32
    return QString::fromStdWString(absolute_path.lexically_normal().generic_wstring()).toLower();
#else
    return QString::fromStdString(absolute_path.lexically_normal().generic_string());
#endif
}

bool MainWindow::job_has_minimum_required_fields(const UiEncodeJob &job) const {
    return !job.source_path.trimmed().isEmpty() &&
        !job.output_path.trimmed().isEmpty() &&
        !job.video_preset.trimmed().isEmpty();
}

QString MainWindow::current_audio_quality_label() const {
    return audio_quality_combo_->currentText().trimmed();
}

bool MainWindow::is_valid_job_index(const int index) const {
    return index >= 0 && index < static_cast<int>(jobs_.size());
}

bool MainWindow::can_accept_source_drop(const QMimeData *mime_data) const {
    if (queue_run_active_ || mime_data == nullptr || !mime_data->hasUrls()) {
        return false;
    }

    const QList<QUrl> urls = mime_data->urls();
    for (const QUrl &url : urls) {
        if (!url.isLocalFile()) {
            continue;
        }

        const QString local_path = QDir::cleanPath(url.toLocalFile().trimmed());
        if (local_path.isEmpty()) {
            continue;
        }

        if (utsure::core::media::SourceImportPaths::is_supported_drop_candidate(qstring_to_path(local_path))) {
            return true;
        }
    }

    return false;
}

QStringList MainWindow::resolve_source_drop_paths(const QMimeData *mime_data) const {
    if (!can_accept_source_drop(mime_data)) {
        return {};
    }

    std::vector<std::filesystem::path> dropped_paths{};
    const QList<QUrl> urls = mime_data->urls();
    dropped_paths.reserve(static_cast<std::size_t>(urls.size()));
    for (const QUrl &url : urls) {
        if (!url.isLocalFile()) {
            continue;
        }

        const QString local_path = QDir::cleanPath(url.toLocalFile().trimmed());
        if (local_path.isEmpty()) {
            continue;
        }

        dropped_paths.push_back(qstring_to_path(local_path));
    }

    const auto expansion_result = utsure::core::media::SourceImportPaths::expand_drop_candidates(dropped_paths);
    QStringList resolved_paths{};
    resolved_paths.reserve(static_cast<qsizetype>(expansion_result.accepted_source_paths.size()));
    for (const auto &path : expansion_result.accepted_source_paths) {
        resolved_paths.push_back(path_to_qstring(path));
    }

    return resolved_paths;
}

qint64 MainWindow::selected_job_frame_step_us() const {
    if (!is_valid_job_index(selected_job_index_)) {
        return 41708;
    }

    const auto &job = jobs_[static_cast<std::size_t>(selected_job_index_)];
    const double frame_rate = job.inspected_source_info.has_value() &&
            job.inspected_source_info->primary_video_stream.has_value()
        ? rational_to_double(job.inspected_source_info->primary_video_stream->average_frame_rate)
        : 0.0;
    return frame_rate > 0.0
        ? static_cast<qint64>(std::llround(1000000.0 / frame_rate))
        : 41708;
}

void MainWindow::add_source_jobs() {
    const QStringList selected_paths = QFileDialog::getOpenFileNames(
        this,
        "Add Source Jobs",
        QString(),
        video_file_filter()
    );
    if (!selected_paths.isEmpty()) {
        add_source_jobs_from_paths(selected_paths);
    }
}

void MainWindow::add_source_jobs_from_paths(const QStringList &paths) {
    if (queue_run_active_ || paths.isEmpty()) {
        return;
    }

    const int first_new_index = static_cast<int>(jobs_.size());
    for (const QString &path : paths) {
        QFileInfo info(path);
        UiEncodeJob job{};
        job.source_path = QDir::toNativeSeparators(path);
        job.source_name = info.fileName();
        job.type_label = info.suffix().isEmpty() ? "-" : "." + info.suffix().toLower();
        job.input_size_bytes = info.exists() ? info.size() : -1;
        jobs_.push_back(std::move(job));
        const int job_index = static_cast<int>(jobs_.size()) - 1;
        apply_generated_output_path(job_index, true);
        QString subtitle_decision_summary{};
        apply_automatic_subtitle_selection(job_index, true, &subtitle_decision_summary);
        jobs_.back().last_status_message = job_has_minimum_required_fields(jobs_.back())
            ? "Ready to queue."
            : "Select an output path to make the job runnable.";
        append_session_log(QString("[info] Added '%1' to the queue.").arg(info.fileName()));
        if (!subtitle_decision_summary.trimmed().isEmpty()) {
            append_session_log("[info] " + subtitle_decision_summary);
        }
    }

    select_job(first_new_index);
}

void MainWindow::remove_selected_job() {
    if (queue_run_active_ || selected_job_index_ < 0 || selected_job_index_ >= static_cast<int>(jobs_.size())) {
        return;
    }

    append_session_log(QString("[info] Removed '%1' from the queue.").arg(selected_job_name()));
    jobs_.erase(jobs_.begin() + selected_job_index_);

    if (jobs_.empty()) {
        selected_job_index_ = -1;
    } else if (selected_job_index_ >= static_cast<int>(jobs_.size())) {
        selected_job_index_ = static_cast<int>(jobs_.size()) - 1;
    }

    if (selected_job_index_ >= 0) {
        select_job(selected_job_index_);
    } else {
        refresh_all_views();
    }
}

void MainWindow::show_settings_placeholder() {
    QMessageBox::information(
        this,
        "Settings",
        "Settings is a placeholder in this slice.\n\nThe current UI exposes global parallel batch and worker priority controls in the toolbar, plus per-job encode settings in the selected-job editor."
    );
}

void MainWindow::show_info_dialog() {
    QMessageBox::information(
        this,
        "utsure",
        QString("%1 %2\n\nQueue-based desktop shell over encoder-core.\n\nThis slice updates the UI around queue editing, preview/task logs, and placeholder surfaces for unfinished features.")
            .arg(to_qstring(utsure::core::BuildInfo::project_name()))
            .arg(to_qstring(utsure::core::BuildInfo::project_version()))
    );
}

void MainWindow::show_parallel_settings_dialog() {
    if (queue_run_active_) {
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle("Parallel Batch Encoding");
    dialog.setModal(true);

    auto *layout = new QVBoxLayout(&dialog);
    auto *form_layout = new QFormLayout();

    auto *enable_parallel_check = new QCheckBox("Enable parallel batch encoding", &dialog);
    enable_parallel_check->setChecked(parallel_batch_settings_.enabled);

    auto *job_count_combo = new QComboBox(&dialog);
    auto *usable_threads_value = new QLabel(&dialog);
    auto *threads_per_job_value = new QLabel(&dialog);
    auto *buffer_per_job_value = new QLabel(&dialog);

    const std::uint32_t usable_thread_count = parallel_batch_summary_.usable_thread_count;
    auto dialog_summary = parallel_batch_summary_;

    const auto populate_job_count_combo = [&](const utsure::core::job::ParallelBatchSummary &summary, const int preferred_count) {
        const QSignalBlocker blocker(job_count_combo);
        job_count_combo->clear();
        for (const int job_count : summary.valid_job_counts) {
            job_count_combo->addItem(QString::number(job_count), job_count);
        }

        const int selected_index = job_count_combo->findData(preferred_count);
        if (selected_index >= 0) {
            job_count_combo->setCurrentIndex(selected_index);
            return;
        }

        const int fallback_index = job_count_combo->findData(summary.selected_job_count);
        job_count_combo->setCurrentIndex(fallback_index >= 0 ? fallback_index : 0);
    };

    const auto refresh_dialog_summary = [&]() {
        const int requested_job_count = std::max(job_count_combo->currentData().toInt(), 1);
        dialog_summary = utsure::core::job::BatchParallelism::summarize(
            utsure::core::job::ParallelBatchSettings{
                .enabled = enable_parallel_check->isChecked(),
                .requested_job_count = requested_job_count
            },
            usable_thread_count
        );
        populate_job_count_combo(dialog_summary, requested_job_count);
        job_count_combo->setEnabled(enable_parallel_check->isChecked());
        usable_threads_value->setText(QString::number(dialog_summary.usable_thread_count));
        threads_per_job_value->setText(QString::number(dialog_summary.threads_per_job));
        buffer_per_job_value->setText(QString("%1 frames").arg(dialog_summary.video_frame_queue_depth));
    };

    populate_job_count_combo(dialog_summary, parallel_batch_settings_.requested_job_count);
    form_layout->addRow(QString(), enable_parallel_check);
    form_layout->addRow("Jobs", job_count_combo);
    form_layout->addRow("Usable threads", usable_threads_value);
    form_layout->addRow("Threads/job", threads_per_job_value);
    form_layout->addRow("Buffer/job", buffer_per_job_value);
    layout->addLayout(form_layout);

    auto *button_box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(button_box);

    connect(enable_parallel_check, &QCheckBox::toggled, &dialog, [refresh_dialog_summary](bool) { refresh_dialog_summary(); });
    connect(job_count_combo, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [refresh_dialog_summary](int) {
        refresh_dialog_summary();
    });
    connect(button_box, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(button_box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    refresh_dialog_summary();

    if (dialog.exec() != QDialog::Accepted) {
        refresh_parallel_batch_summary();
        return;
    }

    parallel_batch_settings_.enabled = enable_parallel_check->isChecked();
    parallel_batch_settings_.requested_job_count = std::max(job_count_combo->currentData().toInt(), 1);
    refresh_parallel_batch_summary();
    append_session_log(
        QString("[info] Parallel batch encoding %1. Jobs: %2 | Threads/job: %3 | Buffer/job: %4 frames.")
            .arg(parallel_batch_summary_.enabled ? "enabled" : "disabled")
            .arg(parallel_batch_summary_.selected_job_count)
            .arg(parallel_batch_summary_.threads_per_job)
            .arg(parallel_batch_summary_.video_frame_queue_depth)
    );
    refresh_toolbar_state();
}

void MainWindow::choose_output_path() {
    if (selected_job_index_ < 0 || selected_job_index_ >= static_cast<int>(jobs_.size())) {
        return;
    }

    auto &job = jobs_[static_cast<std::size_t>(selected_job_index_)];
    QString suggested_path = output_path_edit_->text().trimmed();
    if (suggested_path.isEmpty()) {
        suggested_path = job.same_as_input
            ? QFileInfo(job.source_path).dir().absolutePath()
            : job.source_path;
    }

    const QString selected_path = QFileDialog::getSaveFileName(
        this,
        "Choose Output Path",
        suggested_path,
        output_file_filter()
    );
    if (!selected_path.isEmpty()) {
        job.output_path_manual_override = true;
        output_path_edit_->setText(QDir::toNativeSeparators(selected_path));
    }
}

void MainWindow::restore_selected_job_auto_output_path() {
    if (!is_valid_job_index(selected_job_index_)) {
        return;
    }

    apply_generated_output_path(selected_job_index_, true);
    refresh_all_views();
}

void MainWindow::choose_subtitle_file() {
    if (!is_valid_job_index(selected_job_index_)) {
        return;
    }

    const QString selected_path = QFileDialog::getOpenFileName(
        this,
        "Choose Subtitle File",
        subtitle_path_edit_->text().trimmed(),
        subtitle_file_filter()
    );
    if (!selected_path.isEmpty()) {
        auto &job = jobs_[static_cast<std::size_t>(selected_job_index_)];
        job.subtitle_manual_override = true;
        subtitle_enable_check_->setChecked(true);
        subtitle_path_edit_->setText(QDir::toNativeSeparators(selected_path));
        append_session_log(
            QString("[info] Subtitle selection for '%1' is now manual: '%2'.")
                .arg(queue_source_display_name(job))
                .arg(QFileInfo(selected_path).fileName())
        );
    }
}

void MainWindow::restore_selected_job_auto_subtitle_selection() {
    if (!is_valid_job_index(selected_job_index_)) {
        return;
    }

    QString decision_summary{};
    apply_automatic_subtitle_selection(selected_job_index_, true, &decision_summary);
    if (!decision_summary.trimmed().isEmpty()) {
        append_session_log("[info] " + decision_summary);
    }
    refresh_all_views();
}

void MainWindow::choose_intro_clip() {
    const QString selected_path = QFileDialog::getOpenFileName(
        this,
        "Choose Intro Clip",
        intro_path_edit_->text().trimmed(),
        video_file_filter()
    );
    if (!selected_path.isEmpty()) {
        intro_enable_check_->setChecked(true);
        intro_path_edit_->setText(QDir::toNativeSeparators(selected_path));
    }
}

void MainWindow::choose_intro_music_file() {
    const QString selected_path = QFileDialog::getOpenFileName(
        this,
        "Choose Intro Music",
        intro_music_path_edit_->text().trimmed(),
        "Audio Files (*.aac *.flac *.m4a *.mp3 *.ogg *.opus *.wav);;All Files (*)"
    );
    if (!selected_path.isEmpty()) {
        intro_enable_check_->setChecked(true);
        intro_music_check_->setChecked(true);
        intro_music_path_edit_->setText(QDir::toNativeSeparators(selected_path));
    }
}

void MainWindow::choose_endcard_clip() {
    const QString selected_path = QFileDialog::getOpenFileName(
        this,
        "Choose Endcard Clip",
        endcard_path_edit_->text().trimmed(),
        video_file_filter()
    );
    if (!selected_path.isEmpty()) {
        endcard_enable_check_->setChecked(true);
        endcard_path_edit_->setText(QDir::toNativeSeparators(selected_path));
    }
}

void MainWindow::choose_endcard_music_file() {
    const QString selected_path = QFileDialog::getOpenFileName(
        this,
        "Choose Endcard Music",
        endcard_music_path_edit_->text().trimmed(),
        "Audio Files (*.aac *.flac *.m4a *.mp3 *.ogg *.opus *.wav);;All Files (*)"
    );
    if (!selected_path.isEmpty()) {
        endcard_enable_check_->setChecked(true);
        endcard_music_check_->setChecked(true);
        endcard_music_path_edit_->setText(QDir::toNativeSeparators(selected_path));
    }
}

void MainWindow::choose_thumbnail_image() {
    const QString selected_path = QFileDialog::getOpenFileName(
        this,
        "Choose Thumbnail Image",
        thumbnail_image_path_edit_->text().trimmed(),
        image_file_filter()
    );
    if (!selected_path.isEmpty()) {
        thumbnail_image_path_edit_->setText(QDir::toNativeSeparators(selected_path));
    }
}

void MainWindow::show_thumbnail_placeholder_note() {
    QMessageBox::information(
        this,
        "Thumbnail Placeholder",
        "The thumbnail subtitle-file editing/loading flow is intentionally placeholder-only in this milestone.\n\nThe UI is present so the workflow is visible, but thumbnail.ass parsing and temporary-file save behavior will be added later."
    );
}

void MainWindow::handle_queue_selection_changed() {
    if (suppress_queue_table_changes_) {
        return;
    }

    const int current_row = queue_table_->currentRow();
    if (!is_valid_job_index(current_row) || current_row == selected_job_index_) {
        return;
    }

    select_job(current_row);
}

void MainWindow::handle_queue_item_changed(QTableWidgetItem *item) {
    if (item == nullptr || suppress_queue_table_changes_ || item->column() != 0) {
        return;
    }

    const int row = item->row();
    if (row < 0 || row >= static_cast<int>(jobs_.size())) {
        return;
    }

    if (queue_run_active_) {
        refresh_queue_table();
        return;
    }

    auto &job = jobs_[static_cast<std::size_t>(row)];
    if (job_is_terminal(job)) {
        if (item->checkState() == Qt::Checked) {
            reset_job_for_rerun(job);
            append_session_log(QString("[info] Reset '%1' back to Ready.").arg(job.source_name));
        }
    } else {
        job.checked = item->checkState() == Qt::Checked;
    }

    refresh_all_views();
}

void MainWindow::handle_same_as_input_toggled(const bool enabled) {
    if (loading_selected_job_ || selected_job_index_ < 0 || selected_job_index_ >= static_cast<int>(jobs_.size())) {
        return;
    }

    auto &job = jobs_[static_cast<std::size_t>(selected_job_index_)];
    job.same_as_input = enabled;
    if (job.output_path_manual_override) {
        if (enabled) {
            apply_same_as_input_folder(job);
        }
    } else {
        apply_generated_output_path(selected_job_index_, false);
    }

    load_selected_job_into_editor();
    refresh_all_views();
}

void MainWindow::handle_preview_toggled(const bool enabled) {
    if (!enabled) {
        pause_preview_playback();
    }

    sync_preview_surface_state();
    refresh_selected_job_preview();
    if (enabled && preview_surface_widget_ != nullptr) {
        preview_surface_widget_->setFocus(Qt::OtherFocusReason);
    }
}

void MainWindow::request_selected_job_preview_frame() {
    if (!preview_enabled_check_->isChecked() || !is_valid_job_index(selected_job_index_) || preview_renderer_controller_ == nullptr) {
        return;
    }

    const auto &job = jobs_[static_cast<std::size_t>(selected_job_index_)];
    request_preview_frame_for_time(std::clamp<qint64>(
        job.current_time_us,
        0,
        std::max<qint64>(job.duration_us, 0)
    ));
}

void MainWindow::request_preview_frame_for_time(const qint64 requested_time_us) {
    if (!preview_enabled_check_->isChecked() || !is_valid_job_index(selected_job_index_) || preview_renderer_controller_ == nullptr) {
        return;
    }

    const auto &job = jobs_[static_cast<std::size_t>(selected_job_index_)];
    const QString normalized_source_path = job.source_path.trimmed();
    const QString normalized_subtitle_path = job.subtitle_path.trimmed();
    const bool subtitles_enabled = job.subtitle_enabled && !normalized_subtitle_path.isEmpty();
    const QString subtitle_format_hint = subtitles_enabled
        ? subtitle_format_hint_for_path(normalized_subtitle_path)
        : QString("auto");
    const qint64 bounded_requested_time_us = std::clamp<qint64>(
        requested_time_us,
        0,
        std::max<qint64>(job.duration_us, 0)
    );
    const bool preview_context_changed =
        preview_requested_job_index_ != selected_job_index_ ||
        preview_requested_source_path_ != normalized_source_path ||
        preview_requested_subtitle_enabled_ != subtitles_enabled ||
        preview_requested_subtitle_path_ != normalized_subtitle_path ||
        preview_requested_subtitle_format_hint_ != subtitle_format_hint;

    qCInfo(previewPlaybackLog).noquote()
        << QString(
               "request_preview_frame token=%1 requested=%2 (%3) bounded=%4 (%5) current=%6 (%7) next=%8 (%9) in_flight=%10 playing=%11 context_changed=%12"
           )
               .arg(preview_request_token_)
               .arg(requested_time_us)
               .arg(format_time_us(requested_time_us))
               .arg(bounded_requested_time_us)
               .arg(format_time_us(bounded_requested_time_us))
               .arg(job.current_time_us)
               .arg(format_time_us(job.current_time_us))
               .arg(preview_next_playback_time_us_)
               .arg(format_time_us(std::max<qint64>(preview_next_playback_time_us_, 0)))
               .arg(bool_text(preview_request_in_flight_))
               .arg(bool_text(preview_playing_))
               .arg(bool_text(preview_context_changed));

    if (preview_requested_job_index_ == selected_job_index_ &&
        preview_requested_time_us_ == bounded_requested_time_us &&
        preview_requested_source_path_ == normalized_source_path &&
        preview_requested_subtitle_enabled_ == subtitles_enabled &&
        preview_requested_subtitle_path_ == normalized_subtitle_path &&
        preview_requested_subtitle_format_hint_ == subtitle_format_hint) {
        qCInfo(previewPlaybackLog).noquote()
            << QString("request_preview_frame deduped token=%1 requested=%2 (%3) in_flight=%4 playing=%5")
                   .arg(preview_request_token_)
                   .arg(bounded_requested_time_us)
                   .arg(format_time_us(bounded_requested_time_us))
                   .arg(bool_text(preview_request_in_flight_))
                   .arg(bool_text(preview_playing_));
        return;
    }

    if (preview_context_changed) {
        ++preview_request_token_;
    }

    if (preview_context_changed && preview_surface_widget_ != nullptr) {
        preview_surface_widget_->clear_frame();
    }

    preview_requested_job_index_ = selected_job_index_;
    preview_requested_time_us_ = bounded_requested_time_us;
    preview_requested_source_path_ = normalized_source_path;
    preview_requested_subtitle_enabled_ = subtitles_enabled;
    preview_requested_subtitle_path_ = normalized_subtitle_path;
    preview_requested_subtitle_format_hint_ = subtitle_format_hint;

    qCInfo(previewPlaybackLog).noquote()
        << QString("request_preview_frame dispatch token=%1 requested=%2 (%3)")
               .arg(preview_request_token_)
               .arg(bounded_requested_time_us)
               .arg(format_time_us(bounded_requested_time_us));
    preview_renderer_controller_->request_preview(PreviewFrameRenderRequest{
        .request_token = preview_request_token_,
        .source_path = normalized_source_path,
        .requested_time_us = bounded_requested_time_us,
        .playback_active = preview_playing_,
        .subtitle_enabled = subtitles_enabled,
        .subtitle_path = normalized_subtitle_path,
        .subtitle_format_hint = subtitle_format_hint
    });
}

void MainWindow::clear_preview_surface() {
    if (preview_audio_controller_ != nullptr) {
        preview_audio_controller_->clear();
    }

    const bool preview_state_already_cleared =
        preview_requested_job_index_ == -1 &&
        preview_requested_time_us_ == -1 &&
        preview_requested_source_path_.isEmpty() &&
        !preview_requested_subtitle_enabled_ &&
        preview_requested_subtitle_path_.isEmpty();
    if (preview_state_already_cleared) {
        return;
    }

    ++preview_request_token_;
    preview_request_in_flight_ = false;
    preview_requested_job_index_ = -1;
    preview_requested_time_us_ = -1;
    preview_next_playback_time_us_ = -1;
    preview_requested_source_path_.clear();
    preview_requested_subtitle_enabled_ = false;
    preview_requested_subtitle_path_.clear();
    preview_requested_subtitle_format_hint_ = "auto";

    if (preview_renderer_controller_ != nullptr) {
        preview_renderer_controller_->clear_cache();
    }
}

void MainWindow::start_preview_playback() {
    if (!preview_enabled_check_->isChecked() || !is_valid_job_index(selected_job_index_) || preview_playback_timer_ == nullptr) {
        return;
    }

    auto &job = jobs_[static_cast<std::size_t>(selected_job_index_)];
    const qint64 bounded_duration_us = std::max<qint64>(job.duration_us, 0);
    const qint64 trim_in_us = std::clamp<qint64>(job.trim_in_us, 0, bounded_duration_us);
    const qint64 trim_out_us = std::clamp<qint64>(std::max(job.trim_out_us, trim_in_us), trim_in_us, bounded_duration_us);

    if (job.current_time_us < trim_in_us || job.current_time_us >= trim_out_us) {
        job.current_time_us = trim_in_us;
        refresh_trim_controls();
    }

    const bool resume_from_displayed_frame =
        preview_surface_widget_ != nullptr &&
        preview_surface_widget_->has_frame() &&
        job.current_time_us >= trim_in_us &&
        job.current_time_us < trim_out_us;
    preview_next_playback_time_us_ = resume_from_displayed_frame
        ? std::clamp<qint64>(job.current_time_us + selected_job_frame_step_us(), trim_in_us, trim_out_us)
        : std::clamp<qint64>(job.current_time_us, trim_in_us, trim_out_us);
    preview_playing_ = true;

    if (preview_audio_controller_ != nullptr) {
        if (job.inspected_source_info.has_value() && job.inspected_source_info->primary_audio_stream.has_value()) {
            const bool preview_audio_started = preview_audio_controller_->start_preview(PreviewAudioPlaybackRequest{
                .source_path = job.source_path.trimmed(),
                .requested_time_us = job.current_time_us,
                .source_audio_stream_info = *job.inspected_source_info->primary_audio_stream
            });
            if (!preview_audio_started) {
                qCInfo(previewPlaybackLog) << "start_preview_playback audio path unavailable for the selected source";
            }
        } else {
            preview_audio_controller_->stop_preview();
        }
    }

    preview_playback_timer_->setInterval(
        std::clamp<int>(static_cast<int>(selected_job_frame_step_us() / 1000), 16, 67)
    );
    preview_playback_timer_->start();
    sync_preview_surface_state();
    qCInfo(previewPlaybackLog).noquote()
        << QString("start_preview_playback token=%1 current=%2 (%3) next=%4 (%5) timer_interval_ms=%6")
               .arg(preview_request_token_)
               .arg(job.current_time_us)
               .arg(format_time_us(job.current_time_us))
               .arg(preview_next_playback_time_us_)
               .arg(format_time_us(preview_next_playback_time_us_))
               .arg(preview_playback_timer_->interval());
    handle_preview_playback_tick();
}

void MainWindow::pause_preview_playback() {
    qCInfo(previewPlaybackLog).noquote()
        << QString("pause_preview_playback token=%1 requested=%2 next=%3 in_flight=%4")
               .arg(preview_request_token_)
               .arg(preview_requested_time_us_)
               .arg(preview_next_playback_time_us_)
               .arg(bool_text(preview_request_in_flight_));
    ++preview_request_token_;
    preview_request_in_flight_ = false;
    preview_requested_time_us_ = -1;
    preview_next_playback_time_us_ = -1;
    preview_playing_ = false;
    if (preview_playback_timer_ != nullptr) {
        preview_playback_timer_->stop();
    }
    if (preview_audio_controller_ != nullptr) {
        preview_audio_controller_->stop_preview();
    }
    sync_preview_surface_state();
}

void MainWindow::stop_preview_playback() {
    pause_preview_playback();
    if (!is_valid_job_index(selected_job_index_)) {
        return;
    }

    auto &job = jobs_[static_cast<std::size_t>(selected_job_index_)];
    job.current_time_us = std::clamp<qint64>(job.trim_in_us, 0, std::max<qint64>(job.duration_us, 0));
    refresh_trim_controls();
    refresh_selected_job_preview();
}

void MainWindow::sync_preview_surface_state() {
    if (preview_surface_widget_ == nullptr || preview_enabled_check_ == nullptr) {
        return;
    }

    const bool controls_enabled = preview_enabled_check_->isChecked() &&
        is_valid_job_index(selected_job_index_) &&
        jobs_[static_cast<std::size_t>(selected_job_index_)].source_inspection_error.trimmed().isEmpty();
    preview_surface_widget_->set_controls_enabled(controls_enabled);
    preview_surface_widget_->set_playing(preview_playing_);

    if (preview_controls_panel_ != nullptr) {
        preview_controls_panel_->setEnabled(controls_enabled);
    }

    if (preview_play_pause_button_ != nullptr) {
        const QString tooltip = preview_playing_ ? "Pause preview" : "Play preview";
        preview_play_pause_button_->setToolTip(tooltip);
        apply_icon_or_text(
            preview_play_pause_button_,
            preview_playing_ ? ":/icons/pause.svg" : ":/icons/play.svg",
            preview_playing_ ? "Pause" : "Play",
            QSize(15, 15),
            30,
            24,
            false
        );
        preview_play_pause_button_->setEnabled(controls_enabled);
    }

    if (preview_stop_button_ != nullptr) {
        preview_stop_button_->setEnabled(controls_enabled);
    }

    layout_preview_controls_overlay();
    refresh_preview_footer_visibility();
}

void MainWindow::layout_preview_controls_overlay() {
    if (preview_controls_panel_ == nullptr || preview_surface_widget_ == nullptr) {
        return;
    }

    constexpr int kOverlayMargin = 10;
    const QRect host_rect = preview_surface_widget_->rect();
    const int panel_height = preview_controls_panel_->height();
    const int panel_width = std::max(220, host_rect.width() - (kOverlayMargin * 2));
    preview_controls_panel_->setGeometry(
        kOverlayMargin,
        std::max(kOverlayMargin, host_rect.height() - panel_height - kOverlayMargin),
        panel_width,
        panel_height
    );
    preview_controls_panel_->raise();
}

void MainWindow::refresh_preview_footer_visibility() {
    if (preview_controls_panel_ == nullptr || preview_surface_widget_ == nullptr || preview_enabled_check_ == nullptr) {
        return;
    }

    const bool controls_enabled = preview_enabled_check_->isChecked() &&
        is_valid_job_index(selected_job_index_) &&
        jobs_[static_cast<std::size_t>(selected_job_index_)].source_inspection_error.trimmed().isEmpty();

    bool hover_active = false;
    if (controls_enabled) {
        const QPoint cursor_pos = QCursor::pos();
        hover_active = widget_contains_global_pos(preview_surface_widget_, cursor_pos) ||
            widget_contains_global_pos(preview_controls_panel_, cursor_pos);
    }

    preview_controls_panel_->setEnabled(controls_enabled);
    preview_controls_panel_->setVisible(controls_enabled && hover_active);
    if (controls_enabled && hover_active) {
        layout_preview_controls_overlay();
    }
}

void MainWindow::handle_preview_loading(const quint64 request_token, const qint64 requested_time_us) {
    if (request_token != preview_request_token_ || !preview_enabled_check_->isChecked() || !is_valid_job_index(selected_job_index_)) {
        qCInfo(previewPlaybackLog).noquote()
            << QString("handle_preview_loading ignored token=%1 requested=%2 active_token=%3")
                   .arg(request_token)
                   .arg(requested_time_us)
                   .arg(preview_request_token_);
        return;
    }

    preview_request_in_flight_ = true;
    qCInfo(previewPlaybackLog).noquote()
        << QString("handle_preview_loading token=%1 requested=%2 (%3) request_in_flight=true")
               .arg(request_token)
               .arg(requested_time_us)
               .arg(format_time_us(requested_time_us));
    if (!preview_surface_widget_->has_frame()) {
        preview_surface_widget_->set_placeholder("LOADING PREVIEW");
        preview_time_badge_->setText(format_time_us(requested_time_us));
        return;
    }

    if (!preview_playing_) {
        preview_time_badge_->setText(format_time_us(requested_time_us));
    }
}

void MainWindow::handle_preview_ready(
    const quint64 request_token,
    const qint64 requested_time_us,
    const qint64 frame_time_us,
    const qint64 frame_duration_us,
    const QImage &image
) {
    preview_request_in_flight_ = false;
    qCInfo(previewPlaybackLog).noquote()
        << QString(
               "handle_preview_ready token=%1 requested=%2 (%3) frame_time=%4 (%5) frame_duration=%6 request_in_flight=false"
           )
               .arg(request_token)
               .arg(requested_time_us)
               .arg(format_time_us(requested_time_us))
               .arg(frame_time_us)
               .arg(format_time_us(frame_time_us))
               .arg(frame_duration_us);

    if (request_token != preview_request_token_ || !preview_enabled_check_->isChecked() || !is_valid_job_index(selected_job_index_)) {
        qCInfo(previewPlaybackLog).noquote()
            << QString("handle_preview_ready ignored token=%1 active_token=%2 preview_enabled=%3 valid_job=%4")
                   .arg(request_token)
                   .arg(preview_request_token_)
                   .arg(bool_text(preview_enabled_check_->isChecked()))
                   .arg(bool_text(is_valid_job_index(selected_job_index_)));
        return;
    }

    if (requested_time_us != preview_requested_time_us_) {
        qCInfo(previewPlaybackLog).noquote()
            << QString("handle_preview_ready ignored stale_request requested=%1 active_requested=%2")
                   .arg(requested_time_us)
                   .arg(preview_requested_time_us_);
        return;
    }

    auto &job = jobs_[static_cast<std::size_t>(selected_job_index_)];
    const qint64 bounded_duration_us = std::max<qint64>(job.duration_us, 0);
    const qint64 trim_in_us = std::clamp<qint64>(job.trim_in_us, 0, bounded_duration_us);
    const qint64 trim_out_us = std::clamp<qint64>(std::max(job.trim_out_us, trim_in_us), trim_in_us, bounded_duration_us);
    const qint64 normalized_frame_time_us = std::clamp<qint64>(frame_time_us, trim_in_us, trim_out_us);
    const qint64 normalized_requested_time_us = std::clamp<qint64>(requested_time_us, trim_in_us, trim_out_us);
    const qint64 effective_frame_step_us = std::max<qint64>(
        frame_duration_us,
        selected_job_frame_step_us()
    );

    if (preview_playing_) {
        job.current_time_us = normalized_frame_time_us;
        preview_next_playback_time_us_ = std::min(trim_out_us, normalized_frame_time_us + effective_frame_step_us);
        if (preview_playback_timer_ != nullptr) {
            preview_playback_timer_->setInterval(
                std::clamp<int>(static_cast<int>(effective_frame_step_us / 1000), 16, 67)
            );
        }
    } else {
        job.current_time_us = normalized_requested_time_us;
        preview_next_playback_time_us_ = -1;
    }

    preview_surface_widget_->set_frame_image(image);
    preview_time_badge_->setText(format_time_us(job.current_time_us));
    refresh_trim_controls();

    qCInfo(previewPlaybackLog).noquote()
        << QString("handle_preview_ready applied current=%1 (%2) next=%3 (%4) timer_interval_ms=%5")
               .arg(job.current_time_us)
               .arg(format_time_us(job.current_time_us))
               .arg(preview_next_playback_time_us_)
               .arg(format_time_us(std::max<qint64>(preview_next_playback_time_us_, 0)))
               .arg(preview_playback_timer_ != nullptr ? preview_playback_timer_->interval() : -1);

    if (preview_playing_ && job.current_time_us >= trim_out_us) {
        pause_preview_playback();
    }
}

void MainWindow::handle_preview_failed(
    const quint64 request_token,
    const qint64 requested_time_us,
    const QString &title,
    const QString &detail
) {
    preview_request_in_flight_ = false;
    qCInfo(previewPlaybackLog).noquote()
        << QString("handle_preview_failed token=%1 requested=%2 (%3) title='%4' detail='%5' request_in_flight=false")
               .arg(request_token)
               .arg(requested_time_us)
               .arg(format_time_us(requested_time_us))
               .arg(title, detail);

    if (request_token != preview_request_token_ || !preview_enabled_check_->isChecked() || !is_valid_job_index(selected_job_index_)) {
        qCInfo(previewPlaybackLog).noquote()
            << QString("handle_preview_failed ignored token=%1 active_token=%2 preview_enabled=%3 valid_job=%4")
                   .arg(request_token)
                   .arg(preview_request_token_)
                   .arg(bool_text(preview_enabled_check_->isChecked()))
                   .arg(bool_text(is_valid_job_index(selected_job_index_)));
        return;
    }

    if (requested_time_us != preview_requested_time_us_) {
        qCInfo(previewPlaybackLog).noquote()
            << QString("handle_preview_failed ignored stale_request requested=%1 active_requested=%2")
                   .arg(requested_time_us)
                   .arg(preview_requested_time_us_);
        return;
    }

    pause_preview_playback();
    preview_surface_widget_->set_placeholder(title, detail);
    preview_time_badge_->setText(format_time_us(requested_time_us));
}

void MainWindow::handle_preview_surface_clicked() {
    if (!preview_enabled_check_->isChecked() || !is_valid_job_index(selected_job_index_)) {
        return;
    }

    if (preview_playing_) {
        pause_preview_playback();
        return;
    }

    start_preview_playback();
}

void MainWindow::handle_preview_play_pause_requested() {
    if (preview_surface_widget_ != nullptr) {
        preview_surface_widget_->setFocus(Qt::OtherFocusReason);
    }
    handle_preview_surface_clicked();
}

void MainWindow::handle_preview_stop_requested() {
    if (preview_surface_widget_ != nullptr) {
        preview_surface_widget_->setFocus(Qt::OtherFocusReason);
    }
    stop_preview_playback();
}

void MainWindow::handle_preview_playback_tick() {
    if (!preview_playing_ || preview_playback_timer_ == nullptr || !preview_enabled_check_->isChecked() || !is_valid_job_index(selected_job_index_)) {
        qCInfo(previewPlaybackLog).noquote()
            << QString("handle_preview_playback_tick stopping playing=%1 timer=%2 preview_enabled=%3 valid_job=%4")
                   .arg(bool_text(preview_playing_))
                   .arg(bool_text(preview_playback_timer_ != nullptr))
                   .arg(bool_text(preview_enabled_check_->isChecked()))
                   .arg(bool_text(is_valid_job_index(selected_job_index_)));
        pause_preview_playback();
        return;
    }

    if (preview_request_in_flight_) {
        qCInfo(previewPlaybackLog).noquote()
            << QString("handle_preview_playback_tick skipped request_in_flight=true token=%1 requested=%2")
                   .arg(preview_request_token_)
                   .arg(preview_requested_time_us_);
        return;
    }

    auto &job = jobs_[static_cast<std::size_t>(selected_job_index_)];
    const qint64 bounded_duration_us = std::max<qint64>(job.duration_us, 0);
    const qint64 trim_in_us = std::clamp<qint64>(job.trim_in_us, 0, bounded_duration_us);
    const qint64 trim_out_us = std::clamp<qint64>(std::max(job.trim_out_us, trim_in_us), trim_in_us, bounded_duration_us);
    qint64 requested_time_us = preview_next_playback_time_us_;

    if (preview_audio_controller_ != nullptr && preview_audio_controller_->is_audio_playing()) {
        const qint64 audio_playback_time_us = preview_audio_controller_->current_playback_time_us();
        if (audio_playback_time_us >= 0) {
            requested_time_us = audio_playback_time_us;
        }
    }

    if (requested_time_us < 0) {
        requested_time_us =
            preview_surface_widget_ != nullptr && preview_surface_widget_->has_frame()
                ? job.current_time_us + selected_job_frame_step_us()
                : job.current_time_us;
    }

    requested_time_us = std::clamp<qint64>(requested_time_us, trim_in_us, trim_out_us);
    qCInfo(previewPlaybackLog).noquote()
        << QString("handle_preview_playback_tick advancing current=%1 (%2) next=%3 (%4) requested=%5 (%6)")
               .arg(job.current_time_us)
               .arg(format_time_us(job.current_time_us))
               .arg(preview_next_playback_time_us_)
               .arg(format_time_us(std::max<qint64>(preview_next_playback_time_us_, 0)))
               .arg(requested_time_us)
               .arg(format_time_us(requested_time_us));
    if (requested_time_us >= trim_out_us) {
        job.current_time_us = trim_out_us;
        pause_preview_playback();
        refresh_trim_controls();
        return;
    }

    request_preview_frame_for_time(requested_time_us);
}

void MainWindow::step_selected_job_frame(const int direction) {
    if (selected_job_index_ < 0 || selected_job_index_ >= static_cast<int>(jobs_.size())) {
        return;
    }

    if (preview_surface_widget_ != nullptr) {
        preview_surface_widget_->setFocus(Qt::OtherFocusReason);
    }
    pause_preview_playback();
    auto &job = jobs_[static_cast<std::size_t>(selected_job_index_)];
    const qint64 frame_step_us = selected_job_frame_step_us();

    job.current_time_us = std::clamp(
        job.current_time_us + (frame_step_us * direction),
        0LL,
        std::max<qint64>(job.duration_us, 0)
    );
    refresh_trim_controls();
    refresh_selected_job_preview();
}

void MainWindow::set_selected_job_trim_in() {
    if (selected_job_index_ < 0 || selected_job_index_ >= static_cast<int>(jobs_.size())) {
        return;
    }

    if (preview_surface_widget_ != nullptr) {
        preview_surface_widget_->setFocus(Qt::OtherFocusReason);
    }
    pause_preview_playback();
    auto &job = jobs_[static_cast<std::size_t>(selected_job_index_)];
    job.trim_in_us = std::min(job.current_time_us, job.trim_out_us);
    refresh_trim_controls();
    refresh_selected_job_details();
    refresh_selected_job_preview();
}

void MainWindow::set_selected_job_trim_out() {
    if (selected_job_index_ < 0 || selected_job_index_ >= static_cast<int>(jobs_.size())) {
        return;
    }

    if (preview_surface_widget_ != nullptr) {
        preview_surface_widget_->setFocus(Qt::OtherFocusReason);
    }
    pause_preview_playback();
    auto &job = jobs_[static_cast<std::size_t>(selected_job_index_)];
    job.trim_out_us = std::max(job.current_time_us, job.trim_in_us);
    refresh_trim_controls();
    refresh_selected_job_details();
    refresh_selected_job_preview();
}

void MainWindow::jump_selected_job_to_in() {
    if (selected_job_index_ < 0 || selected_job_index_ >= static_cast<int>(jobs_.size())) {
        return;
    }

    if (preview_surface_widget_ != nullptr) {
        preview_surface_widget_->setFocus(Qt::OtherFocusReason);
    }
    pause_preview_playback();
    jobs_[static_cast<std::size_t>(selected_job_index_)].current_time_us =
        jobs_[static_cast<std::size_t>(selected_job_index_)].trim_in_us;
    refresh_trim_controls();
    refresh_selected_job_preview();
}

void MainWindow::jump_selected_job_to_out() {
    if (selected_job_index_ < 0 || selected_job_index_ >= static_cast<int>(jobs_.size())) {
        return;
    }

    if (preview_surface_widget_ != nullptr) {
        preview_surface_widget_->setFocus(Qt::OtherFocusReason);
    }
    pause_preview_playback();
    jobs_[static_cast<std::size_t>(selected_job_index_)].current_time_us =
        jobs_[static_cast<std::size_t>(selected_job_index_)].trim_out_us;
    refresh_trim_controls();
    refresh_selected_job_preview();
}

void MainWindow::handle_timeline_seek(const qint64 time_us) {
    if (selected_job_index_ < 0 || selected_job_index_ >= static_cast<int>(jobs_.size())) {
        return;
    }

    if (preview_surface_widget_ != nullptr) {
        preview_surface_widget_->setFocus(Qt::OtherFocusReason);
    }
    pause_preview_playback();
    auto &job = jobs_[static_cast<std::size_t>(selected_job_index_)];
    job.current_time_us = std::clamp<qint64>(time_us, 0, std::max<qint64>(job.duration_us, 0));
    refresh_trim_controls();
    refresh_selected_job_preview();
}

void MainWindow::sync_selected_job_from_editor() {
    if (loading_selected_job_ || selected_job_index_ < 0 || selected_job_index_ >= static_cast<int>(jobs_.size())) {
        return;
    }

    auto &job = jobs_[static_cast<std::size_t>(selected_job_index_)];
    job.output_name_custom_text = output_name_text_edit_->text().trimmed();
    job.output_path = output_path_edit_->text().trimmed();
    job.subtitle_enabled = subtitle_enable_check_->isChecked();
    job.subtitle_path = subtitle_path_edit_->text().trimmed();
    job.intro_enabled = intro_enable_check_->isChecked();
    job.intro_path = intro_path_edit_->text().trimmed();
    job.intro_music_enabled = intro_music_check_->isChecked();
    job.intro_music_path = intro_music_path_edit_->text().trimmed();
    job.endcard_enabled = endcard_enable_check_->isChecked();
    job.endcard_path = endcard_path_edit_->text().trimmed();
    job.endcard_music_enabled = endcard_music_check_->isChecked();
    job.endcard_music_path = endcard_music_path_edit_->text().trimmed();
    job.thumbnail_image_path = thumbnail_image_path_edit_->text().trimmed();
    job.thumbnail_title = thumbnail_title_edit_->text().trimmed();
    job.video_codec = static_cast<utsure::core::media::OutputVideoCodec>(video_codec_combo_->currentData().toInt());
    job.video_preset = preset_combo_->currentText().trimmed();
    job.video_crf = crf_spin_box_->value();
    job.audio_mode = static_cast<utsure::core::media::AudioOutputMode>(audio_format_combo_->currentData().toInt());
    job.audio_bitrate_kbps = audio_quality_combo_->currentData().toInt();
    job.audio_track_display = audio_track_combo_->currentText().trimmed();

    if (!job.output_path_manual_override) {
        apply_generated_output_path(selected_job_index_, false);
    }
    if (!job.subtitle_manual_override) {
        apply_automatic_subtitle_selection(selected_job_index_, false);
    }

    if (job.state == UiJobState::pending) {
        job.last_status_message = job_has_minimum_required_fields(job)
            ? "Ready to queue."
            : "Select an output path to make the job runnable.";
    }

    refresh_all_views();
}

void MainWindow::load_selected_job_into_editor() {
    loading_selected_job_ = true;

    if (!is_valid_job_index(selected_job_index_)) {
        output_name_text_edit_->clear();
        output_path_edit_->clear();
        same_as_input_check_->setChecked(true);
        subtitle_enable_check_->setChecked(false);
        subtitle_path_edit_->clear();
        intro_enable_check_->setChecked(false);
        intro_path_edit_->clear();
        intro_music_check_->setChecked(false);
        intro_music_path_edit_->clear();
        endcard_enable_check_->setChecked(false);
        endcard_path_edit_->clear();
        endcard_music_check_->setChecked(false);
        endcard_music_path_edit_->clear();
        video_codec_combo_->setCurrentIndex(0);
        preset_combo_->setCurrentText("medium");
        crf_spin_box_->setValue(28);
        audio_format_combo_->setCurrentIndex(0);
        audio_quality_combo_->setCurrentIndex(audio_quality_combo_->findData(192));
        audio_track_combo_->clear();
        thumbnail_image_path_edit_->clear();
        thumbnail_title_edit_->clear();
    } else {
        const auto &job = jobs_[static_cast<std::size_t>(selected_job_index_)];
        output_name_text_edit_->setText(job.output_name_custom_text);
        output_path_edit_->setText(job.output_path);
        same_as_input_check_->setChecked(job.same_as_input);
        subtitle_enable_check_->setChecked(job.subtitle_enabled);
        subtitle_path_edit_->setText(job.subtitle_path);
        intro_enable_check_->setChecked(job.intro_enabled);
        intro_path_edit_->setText(job.intro_path);
        intro_music_check_->setChecked(job.intro_music_enabled);
        intro_music_path_edit_->setText(job.intro_music_path);
        endcard_enable_check_->setChecked(job.endcard_enabled);
        endcard_path_edit_->setText(job.endcard_path);
        endcard_music_check_->setChecked(job.endcard_music_enabled);
        endcard_music_path_edit_->setText(job.endcard_music_path);
        video_codec_combo_->setCurrentIndex(video_codec_combo_->findData(static_cast<int>(job.video_codec)));
        preset_combo_->setCurrentText(job.video_preset);
        crf_spin_box_->setValue(job.video_crf);
        audio_format_combo_->setCurrentIndex(audio_format_combo_->findData(static_cast<int>(job.audio_mode)));
        audio_quality_combo_->setCurrentIndex(audio_quality_combo_->findData(job.audio_bitrate_kbps));
        thumbnail_image_path_edit_->setText(job.thumbnail_image_path);
        thumbnail_title_edit_->setText(job.thumbnail_title);
        refresh_audio_track_combo();
    }

    loading_selected_job_ = false;
    refresh_editor_state();
}

void MainWindow::select_job(const int index) {
    if (index != selected_job_index_) {
        pause_preview_playback();
    }

    if (!is_valid_job_index(index)) {
        selected_job_index_ = -1;
        if (queue_table_ != nullptr) {
            const QSignalBlocker blocker(queue_table_);
            queue_table_->clearSelection();
        }
        refresh_all_views();
        return;
    }

    selected_job_index_ = index;
    ensure_job_inspection(index);
    if (!queue_run_active_) {
        apply_generated_output_path(index, false);
        apply_automatic_subtitle_selection(index, false);
    }

    if (queue_table_ != nullptr && index < queue_table_->rowCount() && queue_table_->currentRow() != index) {
        const QSignalBlocker blocker(queue_table_);
        queue_table_->selectRow(index);
    }

    load_selected_job_into_editor();
    refresh_all_views();
}

void MainWindow::ensure_runner_slot_count(const int slot_count) {
    const int required_slot_count = std::max(slot_count, 1);
    while (static_cast<int>(runner_slots_.size()) < required_slot_count) {
        const int slot_index = static_cast<int>(runner_slots_.size());
        auto *controller = new EncodeJobRunnerController(this);

        connect(controller, &EncodeJobRunnerController::running_changed, this, [this, slot_index](const bool running) {
            handle_runner_running_changed(slot_index, running);
        });
        connect(
            controller,
            &EncodeJobRunnerController::progress_changed,
            this,
            [this, slot_index](const utsure::core::job::EncodeJobProgress &progress) {
                handle_runner_progress(slot_index, progress);
            }
        );
        connect(
            controller,
            &EncodeJobRunnerController::job_finished,
            this,
            [this, slot_index](
                const bool succeeded,
                const bool canceled,
                const QString &status_text,
                const QString &details_text
            ) {
                handle_runner_finished(slot_index, succeeded, canceled, status_text, details_text);
            }
        );
        connect(controller, &EncodeJobRunnerController::log_message, this, [this, slot_index](const QString &line) {
            if (slot_index < 0 || slot_index >= static_cast<int>(runner_slots_.size())) {
                append_session_log(line);
                return;
            }

            const int active_job_index = runner_slots_[static_cast<std::size_t>(slot_index)].active_job_index;
            if (active_job_index >= 0 && active_job_index < static_cast<int>(jobs_.size())) {
                append_job_log(active_job_index, line);
                return;
            }

            append_session_log(line);
        });

        runner_slots_.push_back(RunnerSlot{
            .controller = controller
        });
    }
}

void MainWindow::ensure_job_inspection(const int job_index) {
    if (job_index < 0 || job_index >= static_cast<int>(jobs_.size())) {
        return;
    }

    auto &job = jobs_[static_cast<std::size_t>(job_index)];
    const QString normalized_key = QDir::toNativeSeparators(job.source_path.trimmed());
    if (normalized_key.isEmpty() || job.inspected_source_key == normalized_key) {
        return;
    }

    job.inspected_source_key = normalized_key;
    job.inspected_source_info.reset();
    job.source_inspection_error.clear();
    job.input_size_bytes = QFileInfo(job.source_path).exists() ? QFileInfo(job.source_path).size() : -1;

    const auto inspection_result = utsure::core::media::MediaInspector::inspect(qstring_to_path(job.source_path));
    if (!inspection_result.succeeded()) {
        job.source_inspection_error = to_qstring(inspection_result.error->message);
        job.audio_track_display = "No source audio detected";
        apply_automatic_subtitle_selection(job_index, false);
        return;
    }

    job.inspected_source_info = *inspection_result.media_source_info;
    job.audio_track_display = format_audio_track_display(*job.inspected_source_info);

    if (job.inspected_source_info->container_duration_microseconds.has_value() &&
        *job.inspected_source_info->container_duration_microseconds > 0) {
        job.duration_us = *job.inspected_source_info->container_duration_microseconds;
        job.trim_out_us = job.duration_us;
    }

    job.current_time_us = std::clamp<qint64>(job.current_time_us, 0, std::max<qint64>(job.duration_us, 0));
    job.trim_in_us = std::clamp<qint64>(job.trim_in_us, 0, std::max<qint64>(job.duration_us, 0));
    job.trim_out_us = std::clamp<qint64>(job.trim_out_us, job.trim_in_us, std::max<qint64>(job.duration_us, 0));
    apply_automatic_subtitle_selection(job_index, false);
}

void MainWindow::reset_job_for_rerun(UiEncodeJob &job) {
    job.state = UiJobState::pending;
    job.checked = true;
    job.efps_display.clear();
    job.speed_display.clear();
    job.elapsed_ms = 0;
    job.remaining_ms = -1;
    job.output_size_bytes = -1;
    job.last_status_message = job_has_minimum_required_fields(job)
        ? "Ready to queue."
        : "Select an output path to make the job runnable.";
    job.last_details_summary.clear();
    job.task_log.clear();
}

void MainWindow::apply_same_as_input_folder(UiEncodeJob &job) {
    if (!job.same_as_input || job.source_path.trimmed().isEmpty() || job.output_path.trimmed().isEmpty()) {
        return;
    }

    const auto source_path = qstring_to_path(job.source_path);
    auto output_path = qstring_to_path(job.output_path);
    if (source_path.empty() || source_path.parent_path().empty() || output_path.empty()) {
        return;
    }

    std::filesystem::path file_name = output_path.filename();
    if (file_name.empty()) {
        file_name = output_path;
    }
    if (file_name.empty()) {
        return;
    }

    job.output_path = path_to_qstring((source_path.parent_path() / file_name).lexically_normal());
}

void MainWindow::reserve_batch_output_paths_for_jobs(const std::vector<int> &job_indices) {
    struct BatchOutputReservation final {
        int job_index{-1};
        utsure::core::job::OutputNamingRequest request{};
    };

    std::vector<BatchOutputReservation> reservations{};
    reservations.reserve(job_indices.size());

    for (const int job_index : job_indices) {
        if (!is_valid_job_index(job_index)) {
            continue;
        }

        const auto &job = jobs_[static_cast<std::size_t>(job_index)];
        if (job.output_path_manual_override) {
            continue;
        }

        reservations.push_back(BatchOutputReservation{
            .job_index = job_index,
            .request = build_output_naming_request(job)
        });
    }

    std::vector<utsure::core::job::OutputNamingRequest> requests{};
    requests.reserve(reservations.size());
    for (const auto &reservation : reservations) {
        requests.push_back(reservation.request);
    }

    const auto reserved_results = utsure::core::job::OutputNaming::reserve_batch(requests);
    for (std::size_t index = 0; index < reservations.size() && index < reserved_results.size(); ++index) {
        auto &job = jobs_[static_cast<std::size_t>(reservations[index].job_index)];
        job.output_path = path_to_qstring(reserved_results[index].output_path);

        if (reservations[index].job_index == selected_job_index_ && output_path_edit_ != nullptr) {
            const QSignalBlocker blocker(output_path_edit_);
            output_path_edit_->setText(job.output_path);
        }
    }
}

QString MainWindow::generate_output_path_for_job(const UiEncodeJob &job) const {
    const auto result = utsure::core::job::OutputNaming::suggest(build_output_naming_request(job));

    return path_to_qstring(result.output_path);
}

void MainWindow::apply_generated_output_path(const int job_index, const bool force_auto_mode) {
    if (!is_valid_job_index(job_index)) {
        return;
    }

    auto &job = jobs_[static_cast<std::size_t>(job_index)];
    if (force_auto_mode) {
        job.output_path_manual_override = false;
    }
    if (job.output_path_manual_override) {
        return;
    }

    const QString generated_output_path = generate_output_path_for_job(job);
    if (generated_output_path.trimmed().isEmpty()) {
        return;
    }

    job.output_path = generated_output_path;
    if (job_index == selected_job_index_ && output_path_edit_ != nullptr &&
        output_path_edit_->text().trimmed() != generated_output_path) {
        const QSignalBlocker blocker(output_path_edit_);
        output_path_edit_->setText(generated_output_path);
    }
}

void MainWindow::apply_automatic_subtitle_selection(
    const int job_index,
    const bool force_auto_mode,
    QString *decision_summary
) {
    if (decision_summary != nullptr) {
        decision_summary->clear();
    }
    if (!is_valid_job_index(job_index)) {
        return;
    }

    auto &job = jobs_[static_cast<std::size_t>(job_index)];
    if (force_auto_mode) {
        job.subtitle_manual_override = false;
    }
    if (job.subtitle_manual_override) {
        return;
    }

    const auto selection_result =
        utsure::core::subtitles::SubtitleAutoSelector::select(qstring_to_path(job.source_path));
    if (decision_summary != nullptr) {
        *decision_summary = to_qstring(selection_result.decision_summary);
    }

    if (selection_result.has_selection()) {
        job.subtitle_enabled = true;
        job.subtitle_path = path_to_qstring(selection_result.selected_candidate->subtitle_path);
    } else {
        job.subtitle_enabled = false;
        job.subtitle_path.clear();
    }

    if (job_index == selected_job_index_ && subtitle_enable_check_ != nullptr && subtitle_path_edit_ != nullptr) {
        const QSignalBlocker enabled_blocker(subtitle_enable_check_);
        const QSignalBlocker path_blocker(subtitle_path_edit_);
        subtitle_enable_check_->setChecked(job.subtitle_enabled);
        subtitle_path_edit_->setText(job.subtitle_path);
    }
}

void MainWindow::refresh_all_views() {
    refresh_queue_table();
    refresh_editor_state();
    refresh_selected_job_details();
    refresh_selected_job_preview();
    refresh_trim_controls();
    refresh_task_log_view();
    refresh_session_log_view();
    refresh_toolbar_state();
}

void MainWindow::refresh_queue_table() {
    suppress_queue_table_changes_ = true;
    const QSignalBlocker blocker(queue_table_);
    queue_table_->clearContents();
    queue_table_->setRowCount(static_cast<int>(jobs_.size()));

    for (int row = 0; row < static_cast<int>(jobs_.size()); ++row) {
        const auto &job = jobs_[static_cast<std::size_t>(row)];

        auto *file_item = new QTableWidgetItem(queue_source_display_name(job));
        Qt::ItemFlags file_flags = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
        if (!queue_run_active_) {
            file_flags |= Qt::ItemIsUserCheckable;
        }
        file_item->setFlags(file_flags);
        file_item->setCheckState(job.checked ? Qt::Checked : Qt::Unchecked);
        file_item->setToolTip(job.source_path);
        file_item->setTextAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        queue_table_->setItem(row, 0, file_item);

        const auto make_item = [](const QString &text, const Qt::Alignment alignment = Qt::AlignVCenter | Qt::AlignLeft) {
            auto *item = new QTableWidgetItem(text);
            item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
            item->setTextAlignment(alignment);
            return item;
        };

        queue_table_->setItem(row, 1, make_item(queue_type_display_name(job), Qt::AlignCenter));
        auto *status_item = make_item(format_job_state_display_text(job), Qt::AlignCenter);
        status_item->setForeground(foreground_for_state(job.state));
        status_item->setToolTip(job.last_status_message);
        queue_table_->setItem(row, 2, status_item);
        queue_table_->setItem(row, 3, make_item(display_text_or_fallback(job.efps_display, "--"), Qt::AlignCenter));
        queue_table_->setItem(row, 4, make_item(display_text_or_fallback(job.speed_display, "--"), Qt::AlignCenter));
        queue_table_->setItem(row, 5, make_item(queue_output_display_name(job)));
    }

    suppress_queue_table_changes_ = false;

    if (is_valid_job_index(selected_job_index_)) {
        queue_table_->selectRow(selected_job_index_);
    }
}

void MainWindow::refresh_editor_state() {
    const bool has_job = selected_job_index_ >= 0 && selected_job_index_ < static_cast<int>(jobs_.size());
    const bool editable = has_job && !queue_run_active_;
    const bool audio_quality_enabled = editable &&
        audio_format_combo_->currentData().toInt() ==
            static_cast<int>(utsure::core::media::AudioOutputMode::encode_aac);

    output_name_text_edit_->setEnabled(editable);
    output_path_edit_->setEnabled(editable);
    output_auto_button_->setEnabled(editable);
    output_browse_button_->setEnabled(editable);
    same_as_input_check_->setEnabled(editable);

    editor_tabs_->setTabEnabled(0, has_job && !queue_run_active_);
    editor_tabs_->setTabEnabled(1, has_job && !queue_run_active_);
    editor_tabs_->setTabEnabled(2, has_job && !queue_run_active_);
    editor_tabs_->setTabEnabled(3, true);

    subtitle_enable_check_->setEnabled(editable);
    subtitle_path_edit_->setEnabled(editable);
    subtitle_auto_button_->setEnabled(editable);
    subtitle_browse_button_->setEnabled(editable);
    intro_enable_check_->setEnabled(editable);
    intro_path_edit_->setEnabled(editable);
    intro_browse_button_->setEnabled(editable);
    intro_music_check_->setEnabled(editable);
    intro_music_path_edit_->setEnabled(editable && intro_music_check_->isChecked());
    intro_music_browse_button_->setEnabled(editable && intro_music_check_->isChecked());
    endcard_enable_check_->setEnabled(editable);
    endcard_path_edit_->setEnabled(editable);
    endcard_browse_button_->setEnabled(editable);
    endcard_music_check_->setEnabled(editable);
    endcard_music_path_edit_->setEnabled(editable && endcard_music_check_->isChecked());
    endcard_music_browse_button_->setEnabled(editable && endcard_music_check_->isChecked());
    video_codec_combo_->setEnabled(editable);
    preset_combo_->setEnabled(editable);
    crf_spin_box_->setEnabled(editable);
    audio_format_combo_->setEnabled(editable);
    audio_quality_combo_->setEnabled(audio_quality_enabled);
    audio_track_combo_->setEnabled(editable);
    thumbnail_image_path_edit_->setEnabled(editable);
    thumbnail_image_browse_button_->setEnabled(editable);
    thumbnail_title_edit_->setEnabled(editable);
    thumbnail_load_ass_button_->setEnabled(editable);
    thumbnail_edit_title_button_->setEnabled(editable);

    if (!has_job && preview_enabled_check_->isChecked()) {
        const QSignalBlocker blocker(preview_enabled_check_);
        preview_enabled_check_->setChecked(false);
    }
    preview_enabled_check_->setEnabled(has_job);
    if (!has_job) {
        pause_preview_playback();
    }
    sync_preview_surface_state();
    if (preview_play_pause_button_ != nullptr) {
        preview_play_pause_button_->setEnabled(has_job);
    }
    if (preview_stop_button_ != nullptr) {
        preview_stop_button_->setEnabled(has_job);
    }
    trim_timeline_widget_->setEnabled(has_job);
    frame_back_button_->setEnabled(has_job);
    frame_forward_button_->setEnabled(has_job);
    set_in_button_->setEnabled(has_job);
    set_out_button_->setEnabled(has_job);
    jump_in_button_->setEnabled(has_job);
    jump_out_button_->setEnabled(has_job);
}

void MainWindow::refresh_selected_job_details() {
    if (selected_job_index_ < 0 || selected_job_index_ >= static_cast<int>(jobs_.size())) {
        detail_status_value_->setText("--");
        detail_elapsed_value_->setText("--");
        detail_remaining_value_->setText("--");
        detail_efps_value_->setText("--");
        detail_speed_value_->setText("--");
        detail_input_size_value_->setText("--");
        detail_output_size_value_->setText("--");
        detail_timeline_value_->setText("Select a queue row.");
        return;
    }

    auto &job = jobs_[static_cast<std::size_t>(selected_job_index_)];
    update_job_file_sizes(job);

    detail_status_value_->setText(format_job_state_display_text(job));
    detail_elapsed_value_->setText(format_duration_ms(job.elapsed_ms));
    detail_remaining_value_->setText(format_duration_ms(job.remaining_ms));
    detail_efps_value_->setText(display_text_or_fallback(job.efps_display, "--"));
    detail_speed_value_->setText(display_text_or_fallback(job.speed_display, "--"));
    detail_input_size_value_->setText(format_file_size(job.input_size_bytes));
    detail_output_size_value_->setText(format_file_size(job.output_size_bytes));
    detail_timeline_value_->setText(
        QString("Thumb -> Intro -> Main [%1 to %2] -> EndCard")
            .arg(format_time_us(job.trim_in_us))
            .arg(format_time_us(job.trim_out_us))
    );
}

void MainWindow::refresh_selected_job_preview() {
    if (preview_surface_widget_ == nullptr) {
        return;
    }

    if (!is_valid_job_index(selected_job_index_)) {
        pause_preview_playback();
        clear_preview_surface();
        sync_preview_surface_state();
        preview_surface_widget_->set_placeholder("PREVIEW OFFLINE", "Select a queue row.");
        preview_time_badge_->setText("00:00:00.000");
        return;
    }

    const auto &job = jobs_[static_cast<std::size_t>(selected_job_index_)];

    if (!preview_enabled_check_->isChecked()) {
        pause_preview_playback();
        clear_preview_surface();
        sync_preview_surface_state();
        preview_surface_widget_->set_placeholder("PREVIEW OFFLINE");
        preview_time_badge_->setText("00:00:00.000");
        return;
    }

    preview_time_badge_->setText(format_time_us(job.current_time_us));

    if (!job.source_inspection_error.isEmpty()) {
        pause_preview_playback();
        clear_preview_surface();
        sync_preview_surface_state();
        preview_surface_widget_->set_placeholder("PREVIEW UNAVAILABLE", job.source_inspection_error);
        return;
    }

    sync_preview_surface_state();
    request_selected_job_preview_frame();
}

void MainWindow::refresh_trim_controls() {
    if (selected_job_index_ < 0 || selected_job_index_ >= static_cast<int>(jobs_.size())) {
        current_time_value_->setText("00:00:00.000");
        trim_in_value_->setText("IN=00:00:00.000");
        trim_out_value_->setText("OUT=00:00:00.000");
        trim_timeline_widget_->set_duration_us(10000000);
        trim_timeline_widget_->set_trim_range_us(0, 10000000);
        trim_timeline_widget_->set_current_time_us(0);
        return;
    }

    const auto &job = jobs_[static_cast<std::size_t>(selected_job_index_)];
    current_time_value_->setText(format_time_us(job.current_time_us));
    trim_in_value_->setText(QString("IN=%1").arg(format_time_us(job.trim_in_us)));
    trim_out_value_->setText(QString("OUT=%1").arg(format_time_us(job.trim_out_us)));
    trim_timeline_widget_->set_duration_us(std::max<qint64>(job.duration_us, 1));
    trim_timeline_widget_->set_trim_range_us(job.trim_in_us, job.trim_out_us);
    trim_timeline_widget_->set_current_time_us(job.current_time_us);
}

void MainWindow::refresh_task_log_view() {
    if (selected_job_index_ < 0 || selected_job_index_ >= static_cast<int>(jobs_.size())) {
        task_log_summary_label_->setText("Select a job to inspect its task log.");
        task_log_view_->setPlainText("No selected task.");
        return;
    }

    const auto &job = jobs_[static_cast<std::size_t>(selected_job_index_)];
    task_log_summary_label_->setText(
        QString("Status: %1 | Output: %2")
            .arg(format_job_state_display_text(job))
            .arg(queue_output_display_name(job))
    );
    task_log_view_->setPlainText(
        job.task_log.isEmpty()
            ? QString("No task log for '%1' yet.").arg(queue_source_display_name(job))
            : job.task_log.join('\n')
    );
}

void MainWindow::refresh_session_log_view() {
    session_log_view_->setPlainText(
        session_log_lines_.isEmpty()
            ? QString("Session log is empty.")
            : session_log_lines_.join('\n')
    );
}

void MainWindow::refresh_parallel_batch_summary() {
    parallel_batch_summary_ = utsure::core::job::BatchParallelism::summarize(parallel_batch_settings_);
    if (parallel_button_ == nullptr) {
        return;
    }

    parallel_button_->setChecked(parallel_batch_summary_.enabled);
    parallel_button_->setToolTip(format_parallel_tooltip());
}

void MainWindow::refresh_toolbar_state() {
    const bool has_job = selected_job_index_ >= 0 && selected_job_index_ < static_cast<int>(jobs_.size());
    add_button_->setEnabled(!queue_run_active_);
    remove_button_->setEnabled(!queue_run_active_ && has_job);
    if (parallel_button_ != nullptr) {
        parallel_button_->setEnabled(!queue_run_active_);
        parallel_button_->setChecked(parallel_batch_summary_.enabled);
        parallel_button_->setToolTip(format_parallel_tooltip());
    }
    priority_combo_->setEnabled(!queue_run_active_);
    start_button_->setEnabled(!jobs_.empty());
    stop_button_->setEnabled(queue_run_active_);
    update_start_button_visuals();
}

void MainWindow::refresh_audio_track_combo() {
    if (selected_job_index_ < 0 || selected_job_index_ >= static_cast<int>(jobs_.size())) {
        return;
    }

    const auto &job = jobs_[static_cast<std::size_t>(selected_job_index_)];
    const QSignalBlocker blocker(audio_track_combo_);
    audio_track_combo_->clear();
    audio_track_combo_->addItem(job.audio_track_display);
}

void MainWindow::update_start_button_visuals() {
    if (queue_run_active_) {
        if (!busy_spinner_timer_->isActive()) {
            busy_spinner_timer_->start();
        }
        start_button_->setToolTip(stop_requested_ ? "Stopping..." : "Encoding...");
        start_button_->setText(QString{});
        start_button_->setProperty("iconFallback", false);
        start_button_->setIcon(make_busy_icon(busy_spinner_phase_));
        start_button_->setMinimumWidth(30);
        start_button_->setMaximumWidth(30);
        start_button_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        start_button_->setToolButtonStyle(Qt::ToolButtonIconOnly);
        refresh_widget_style(start_button_);
        return;
    }

    busy_spinner_timer_->stop();
    busy_spinner_phase_ = 0;
    start_button_->setToolTip("Start checked jobs");
    apply_icon_or_text(start_button_, ":/icons/play.svg", "Start", QSize(16, 16), 30, 30, true);
}

void MainWindow::advance_busy_spinner() {
    if (!queue_run_active_) {
        return;
    }

    busy_spinner_phase_ = (busy_spinner_phase_ + 1) % 12;
    start_button_->setIcon(make_busy_icon(busy_spinner_phase_));
}

void MainWindow::set_source_drop_overlay_visible(const bool visible) {
    if (source_drop_overlay_ == nullptr) {
        return;
    }

    if (!visible) {
        source_drop_overlay_->hide();
        return;
    }

    update_source_drop_overlay_geometry();
    source_drop_overlay_->show();
    source_drop_overlay_->raise();
}

void MainWindow::update_source_drop_overlay_geometry() {
    if (source_drop_overlay_ == nullptr || centralWidget() == nullptr) {
        return;
    }

    source_drop_overlay_->setGeometry(centralWidget()->rect());
}

void MainWindow::start_encode_queue() {
    if (queue_run_active_ || any_runner_slot_running()) {
        return;
    }

    planned_queue_jobs_.clear();
    queue_cursor_ = 0;
    stop_requested_ = false;

    std::vector<int> checked_job_indices{};
    checked_job_indices.reserve(jobs_.size());
    for (int index = 0; index < static_cast<int>(jobs_.size()); ++index) {
        if (!jobs_[static_cast<std::size_t>(index)].checked) {
            continue;
        }

        ensure_job_inspection(index);
        checked_job_indices.push_back(index);
    }

    reserve_batch_output_paths_for_jobs(checked_job_indices);
    refresh_parallel_batch_summary();

    QHash<QString, int> reserved_output_paths{};
    for (int index = 0; index < static_cast<int>(jobs_.size()); ++index) {
        auto &job = jobs_[static_cast<std::size_t>(index)];
        if (!job.checked) {
            continue;
        }

        QString build_error{};
        auto built_job = build_job_from_entry(index, build_error);
        if (!built_job.has_value()) {
            job.last_status_message = build_error;
            append_job_log(index, "[error] " + build_error);
            continue;
        }

        utsure::core::job::BatchParallelism::apply_execution_settings(*built_job, parallel_batch_summary_);

        append_job_log(index, "[info] Validating job before queue start.");
        const auto preflight = utsure::core::job::EncodeJobPreflight::inspect(*built_job);

        for (const auto &issue : preflight.issues) {
            append_job_log(index, format_preflight_issue(issue));
        }

        if (!preflight.can_start_encode()) {
            const auto first_error = std::find_if(
                preflight.issues.begin(),
                preflight.issues.end(),
                [](const utsure::core::job::EncodeJobPreflightIssue &issue) {
                    return issue.severity == utsure::core::job::EncodeJobPreflightIssueSeverity::error;
                }
            );
            job.last_status_message = first_error != preflight.issues.end()
                ? to_qstring(first_error->message)
                : "Job is not ready to start.";
            continue;
        }

        const QString normalized_output_key =
            normalized_output_path_key(path_to_qstring(built_job->output.output_path));
        if (!normalized_output_key.isEmpty() && reserved_output_paths.contains(normalized_output_key)) {
            job.last_status_message = "Output path conflicts with another queued job.";
            append_job_log(index, "[error] Output path conflicts with another queued job in this batch.");
            continue;
        }

        if (preflight.requires_output_overwrite_confirmation()) {
            const auto response = QMessageBox::question(
                this,
                "Overwrite Existing Output?",
                QString("'%1' already exists.\n\nOverwrite it?")
                    .arg(QDir::toNativeSeparators(path_to_qstring(built_job->output.output_path)))
            );
            if (response != QMessageBox::Yes) {
                append_job_log(index, "[warning] Overwrite declined. The job was skipped.");
                continue;
            }
        }

        job.last_status_message = "Queued for batch encode.";
        if (!normalized_output_key.isEmpty()) {
            reserved_output_paths.insert(normalized_output_key, index);
        }
        planned_queue_jobs_.push_back(PlannedBatchJob{
            .job_index = index,
            .job = *built_job
        });
    }

    if (planned_queue_jobs_.empty()) {
        append_session_log("[warning] No checked jobs were runnable.");
        refresh_all_views();
        return;
    }

    queue_run_active_ = true;
    ensure_runner_slot_count(configured_parallel_job_count());
    append_session_log(
        QString("[info] Starting queue with %1 job(s). Parallel %2 | Jobs: %3 | Threads/job: %4 | Buffer/job: %5 frames.")
            .arg(planned_queue_jobs_.size())
            .arg(parallel_batch_summary_.enabled ? "On" : "Off")
            .arg(parallel_batch_summary_.selected_job_count)
            .arg(parallel_batch_summary_.threads_per_job)
            .arg(parallel_batch_summary_.video_frame_queue_depth)
    );
    refresh_all_views();
    start_available_queued_jobs();
}

void MainWindow::start_available_queued_jobs() {
    if (!queue_run_active_) {
        return;
    }

    ensure_runner_slot_count(configured_parallel_job_count());
    while (!stop_requested_) {
        const int slot_index = find_free_runner_slot_index();
        if (slot_index < 0 || queue_cursor_ >= static_cast<int>(planned_queue_jobs_.size())) {
            break;
        }

        auto &slot = runner_slots_[static_cast<std::size_t>(slot_index)];
        const auto &planned_job = planned_queue_jobs_[static_cast<std::size_t>(queue_cursor_++)];
        if (!is_valid_job_index(planned_job.job_index) || slot.controller == nullptr) {
            continue;
        }

        auto &job = jobs_[static_cast<std::size_t>(planned_job.job_index)];
        job.state = UiJobState::encoding;
        job.last_status_message = "Encoding...";
        job.elapsed_ms = 0;
        job.remaining_ms = -1;
        job.efps_display.clear();
        job.speed_display.clear();

        slot.active_job_index = planned_job.job_index;
        slot.elapsed_timer.restart();
        slot.elapsed_valid = true;

        if (configured_parallel_job_count() == 1) {
            select_job(planned_job.job_index);
        }

        append_job_log(planned_job.job_index, "[info] Starting encode job.");
        slot.controller->start_job(planned_job.job);
    }

    if (!stop_requested_ && queue_cursor_ >= static_cast<int>(planned_queue_jobs_.size()) && active_runner_count() == 0) {
        append_session_log("[info] Queue completed.");
        finish_queue_run();
        return;
    }

    refresh_all_views();
}

void MainWindow::stop_encode_queue() {
    if (!queue_run_active_) {
        return;
    }

    stop_requested_ = true;
    append_session_log("[warning] Stop requested. Canceling active jobs and leaving queued jobs pending.");
    for (auto &slot : runner_slots_) {
        if (slot.controller == nullptr || !slot.controller->is_running()) {
            continue;
        }

        if (slot.active_job_index >= 0 && slot.active_job_index < static_cast<int>(jobs_.size())) {
            append_job_log(slot.active_job_index, "[warning] Cancel requested by user.");
        }

        slot.controller->cancel_job();
    }

    refresh_toolbar_state();

    if (active_runner_count() == 0) {
        append_session_log("[warning] Queue stopped.");
        finish_queue_run();
    }
}

void MainWindow::finish_queue_run() {
    queue_run_active_ = false;
    stop_requested_ = false;
    planned_queue_jobs_.clear();
    queue_cursor_ = 0;
    for (auto &slot : runner_slots_) {
        slot.active_job_index = -1;
        slot.elapsed_valid = false;
    }
    refresh_all_views();
}

void MainWindow::append_session_log(const QString &line) {
    session_log_lines_.push_back(QString("%1 %2").arg(QTime::currentTime().toString("HH:mm:ss"), line));
    refresh_session_log_view();
}

void MainWindow::append_job_log(const int job_index, const QString &line, const bool mirror_to_session) {
    if (job_index < 0 || job_index >= static_cast<int>(jobs_.size())) {
        return;
    }

    jobs_[static_cast<std::size_t>(job_index)].task_log.push_back(line);
    if (mirror_to_session) {
        append_session_log(status_prefix_for_session(jobs_[static_cast<std::size_t>(job_index)].source_name, line));
    }

    if (job_index == selected_job_index_) {
        refresh_task_log_view();
    }
}

void MainWindow::update_job_progress(
    const int job_index,
    const QElapsedTimer *elapsed_timer,
    const bool elapsed_valid,
    const utsure::core::job::EncodeJobProgress &progress
) {
    if (job_index < 0 || job_index >= static_cast<int>(jobs_.size())) {
        return;
    }

    auto &job = jobs_[static_cast<std::size_t>(job_index)];
    if (elapsed_valid && elapsed_timer != nullptr) {
        job.elapsed_ms = elapsed_timer->elapsed();
    }

    if (progress.encoded_video_duration_us.has_value()) {
        job.current_time_us = std::clamp<qint64>(
            *progress.encoded_video_duration_us,
            0,
            std::max<qint64>(job.duration_us, 0)
        );
    }

    if (progress.encoded_fps.has_value() && *progress.encoded_fps > 0.0) {
        job.efps_display = QString::number(*progress.encoded_fps, 'f', 1);
        if (job.inspected_source_info.has_value() && job.inspected_source_info->primary_video_stream.has_value()) {
            const double source_fps = rational_to_double(job.inspected_source_info->primary_video_stream->average_frame_rate);
            if (source_fps > 0.0) {
                job.speed_display = QString("%1x").arg(QString::number(*progress.encoded_fps / source_fps, 'f', 1));
            }
        }
    }

    const double fraction = std::clamp(
        progress.stage_fraction.value_or(progress.overall_fraction.value_or(0.0)),
        0.0,
        1.0
    );
    if (fraction > 0.0 && fraction < 1.0 && job.elapsed_ms > 0) {
        job.remaining_ms = static_cast<qint64>(
            std::llround((static_cast<double>(job.elapsed_ms) * (1.0 - fraction)) / fraction)
        );
    }

    job.last_status_message = progress.message.empty() ? "Encoding..." : to_qstring(progress.message);
    refresh_all_views();
}

void MainWindow::update_job_file_sizes(UiEncodeJob &job) {
    const QFileInfo source_info(job.source_path);
    job.input_size_bytes = source_info.exists() ? source_info.size() : -1;

    const QFileInfo output_info(job.output_path);
    job.output_size_bytes = output_info.exists() ? output_info.size() : -1;
}

void MainWindow::handle_runner_running_changed(const int /*slot_index*/, const bool /*running*/) {
    refresh_all_views();
}

void MainWindow::handle_runner_progress(
    const int slot_index,
    const utsure::core::job::EncodeJobProgress &progress
) {
    if (slot_index < 0 || slot_index >= static_cast<int>(runner_slots_.size())) {
        return;
    }

    const auto &slot = runner_slots_[static_cast<std::size_t>(slot_index)];
    update_job_progress(slot.active_job_index, &slot.elapsed_timer, slot.elapsed_valid, progress);
}

void MainWindow::handle_runner_finished(
    const int slot_index,
    const bool succeeded,
    const bool canceled,
    const QString &status_text,
    const QString &details_text
) {
    if (slot_index < 0 || slot_index >= static_cast<int>(runner_slots_.size())) {
        if (stop_requested_ && active_runner_count() == 0) {
            append_session_log("[warning] Queue stopped.");
            finish_queue_run();
        }
        return;
    }

    auto &slot = runner_slots_[static_cast<std::size_t>(slot_index)];
    const int job_index = slot.active_job_index;
    if (job_index < 0 || job_index >= static_cast<int>(jobs_.size())) {
        slot.active_job_index = -1;
        slot.elapsed_valid = false;
        if (stop_requested_ && active_runner_count() == 0) {
            append_session_log("[warning] Queue stopped.");
            finish_queue_run();
        }
        return;
    }

    auto &job = jobs_[static_cast<std::size_t>(job_index)];
    if (slot.elapsed_valid) {
        job.elapsed_ms = slot.elapsed_timer.elapsed();
    }
    job.last_status_message = status_text;
    job.last_details_summary = details_text;
    update_job_file_sizes(job);

    if (succeeded) {
        job.state = UiJobState::finished;
        job.checked = false;
        job.remaining_ms = 0;
        append_job_log(job_index, "[info] Encode finished successfully.");
    } else if (canceled) {
        job.state = UiJobState::canceled;
        job.checked = false;
        job.efps_display.clear();
        job.speed_display.clear();
        job.remaining_ms = -1;
        append_job_log(job_index, "[warning] Encode canceled.");
    } else {
        job.state = UiJobState::failed;
        job.checked = false;
        job.remaining_ms = -1;
        append_job_log(job_index, "[error] Encode failed.");
    }

    append_job_log(job_index, details_text, false);
    slot.active_job_index = -1;
    slot.elapsed_valid = false;

    if (stop_requested_) {
        if (active_runner_count() == 0) {
            append_session_log("[warning] Queue stopped.");
            finish_queue_run();
        } else {
            refresh_all_views();
        }
        return;
    }

    start_available_queued_jobs();
}

std::filesystem::path MainWindow::qstring_to_path(const QString &text) {
#ifdef _WIN32
    return std::filesystem::path(text.toStdWString());
#else
    return std::filesystem::path(QFile::encodeName(text).constData());
#endif
}

QString MainWindow::path_to_qstring(const std::filesystem::path &path) {
#ifdef _WIN32
    return QDir::toNativeSeparators(QString::fromStdWString(path.native()));
#else
    const auto encoded = path.string();
    return QDir::toNativeSeparators(QFile::decodeName(encoded.c_str()));
#endif
}
