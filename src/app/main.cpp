#include "main_window.hpp"

#include "utsure/core/build_info.hpp"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDebug>
#include <QTimer>

namespace {

QString to_qstring(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

}  // namespace

int main(int argc, char *argv[]) {
    Q_INIT_RESOURCE(app_resources);

    QApplication app(argc, argv);
    QApplication::setApplicationName(to_qstring(utsure::core::BuildInfo::project_name()));
    QApplication::setApplicationVersion(to_qstring(utsure::core::BuildInfo::project_version()));

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
    parser.addOption(smokeTestOption);
    parser.addOption(dumpWindowStructureOption);
    parser.process(app);

    MainWindow mainWindow;
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
