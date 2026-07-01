#include "main_window.hpp"

#include "app_settings.hpp"
#include "crash_dump_writer.hpp"
#include "ui_font.hpp"
#include "utsure/core/build_info.hpp"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDebug>
#include <QFile>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QResource>
#include <QSvgRenderer>
#include <QTimer>

#include <cmath>

namespace {

QString to_qstring(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QIcon load_svg_icon(const QString &resource_path, const QSize &icon_size) {
    const QIcon direct_icon(resource_path);
    if (!direct_icon.isNull()) {
        const QPixmap direct_pixmap = direct_icon.pixmap(icon_size);
        if (!direct_pixmap.isNull()) {
            return direct_icon;
        }
    }

    QFile resource_file(resource_path);
    if (!resource_file.open(QIODevice::ReadOnly)) {
        return {};
    }

    QByteArray svg_bytes = resource_file.readAll();
    if (svg_bytes.startsWith("\xEF\xBB\xBF")) {
        svg_bytes.remove(0, 3);
    }

    if (svg_bytes.isEmpty() || svg_bytes.contains('\0')) {
        return {};
    }

    QSvgRenderer renderer(svg_bytes);
    if (!renderer.isValid()) {
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

}  // namespace

int main(int argc, char *argv[]) {
    utsure::app::crash::configure_crash_log_flushing();
    utsure::app::crash::install_crash_handlers();

    QApplication app(argc, argv);
    QApplication::setApplicationName(to_qstring(utsure::core::BuildInfo::project_name()));
    QApplication::setApplicationVersion(to_qstring(utsure::core::BuildInfo::project_version()));
    Q_INIT_RESOURCE(app_resources);
    const QIcon application_icon = load_svg_icon(":/icons/icon.svg", QSize(256, 256));
    if (!application_icon.isNull()) {
        app.setWindowIcon(application_icon);
    }

    const auto settings_load_result = AppSettings::load_default_location();
    const UiFontResolution ui_font_resolution = UiFontManager::apply(app, settings_load_result.settings.ui_font);
    if (!ui_font_resolution.diagnostic.trimmed().isEmpty()) {
        qInfo().noquote() << ui_font_resolution.diagnostic;
    }
    if (!settings_load_result.warning.trimmed().isEmpty()) {
        qWarning().noquote() << settings_load_result.warning;
    }
    qInfo().noquote() << QString::fromStdString(
        utsure::app::crash::format_crash_dump_setup_log(utsure::app::crash::crash_dump_setup_status())
    );
    const auto recent_crash_artifacts = utsure::app::crash::find_recent_crash_artifacts(
        utsure::app::crash::default_crash_dump_directory(),
        3
    );
    for (const auto &artifact : recent_crash_artifacts) {
        qWarning().noquote()
            << "Recent utsure crash dump found:"
            << QString::fromStdWString(artifact.dump_path.wstring())
            << "sidecar:"
            << QString::fromStdWString(artifact.sidecar_path.wstring())
            << "Please include these when reporting a crash.";
    }

    QCommandLineParser parser;
    parser.setApplicationDescription("utsure desktop encode job runner");
    parser.addHelpOption();

    const QCommandLineOption smokeTestOption(
        "smoke-test",
        "Launch the main window and exit automatically after a short delay."
    );
    const QCommandLineOption dumpWindowStructureOption(
        "dump-window-structure",
        "Print the main window structure to stdout and exit."
    );
    const QCommandLineOption writeDiagnosticDumpOption(
        "write-diagnostic-dump",
        "Write a diagnostic crash dump and sidecar, then exit."
    );
    parser.addOption(smokeTestOption);
    parser.addOption(dumpWindowStructureOption);
    parser.addOption(writeDiagnosticDumpOption);
    parser.process(app);

    if (parser.isSet(writeDiagnosticDumpOption)) {
        const auto dump_result = utsure::app::crash::write_diagnostic_dump_now();
        if (dump_result.dump_written && dump_result.sidecar_written) {
            qInfo().noquote()
                << "Diagnostic dump written:"
                << QString::fromStdWString(dump_result.paths.dump_path.wstring())
                << "sidecar:"
                << QString::fromStdWString(dump_result.paths.sidecar_path.wstring());
            return 0;
        }
        qCritical().noquote()
            << "Diagnostic dump failed:"
            << QString::fromStdString(dump_result.error_message)
            << "handler marker:"
            << QString::fromStdWString(dump_result.paths.handler_entered_path.wstring())
            << "failure marker:"
            << QString::fromStdWString(dump_result.paths.dump_failed_path.wstring())
            << "sidecar:"
            << QString::fromStdWString(dump_result.paths.sidecar_path.wstring());
        return 2;
    }

    MainWindow mainWindow;
    if (!application_icon.isNull()) {
        mainWindow.setWindowIcon(application_icon);
    }
    mainWindow.show();

    if (parser.isSet(dumpWindowStructureOption)) {
        qInfo().noquote() << mainWindow.window_structure_summary();
        if (!parser.isSet(smokeTestOption)) {
            QTimer::singleShot(0, [&app]() {
                app.quit();
            });
        }
    }

    if (parser.isSet(smokeTestOption)) {
        qInfo().noquote() << "Starting utsure GUI smoke test.";
        QTimer::singleShot(300, [&app]() {
            qInfo().noquote() << "utsure GUI smoke test completed.";
            app.quit();
        });
    }

    return app.exec();
}
