#include "main_window.hpp"

#include "utsure/core/build_info.hpp"

#include <QFont>
#include <QLabel>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

namespace {

QString to_qstring(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QString build_summary_text() {
    return QString(
        "Core target: utsure_encoder_core\n"
        "GUI target: utsure_encoder_app\n\n"
        "Current state:\n"
        "%1"
    ).arg(to_qstring(utsure::core::BuildInfo::project_state()));
}

}  // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle(QString("%1 %2")
                       .arg(to_qstring(utsure::core::BuildInfo::project_name()))
                       .arg(to_qstring(utsure::core::BuildInfo::project_version())));
    resize(720, 420);

    auto *centralWidget = new QWidget(this);
    auto *layout = new QVBoxLayout(centralWidget);

    auto *titleLabel = new QLabel("utsure", centralWidget);
    auto *summaryLabel = new QLabel(build_summary_text(), centralWidget);

    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(titleFont.pointSize() + 8);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);

    summaryLabel->setWordWrap(true);
    summaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    layout->addWidget(titleLabel);
    layout->addWidget(summaryLabel);
    layout->addStretch();

    setCentralWidget(centralWidget);
}
