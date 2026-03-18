#include "main_window.hpp"

#include "encode_job_runner_controller.hpp"
#include "utsure/core/build_info.hpp"

#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

#include <string_view>

namespace {

struct PathRowWidgets final {
    QWidget *container{nullptr};
    QLineEdit *line_edit{nullptr};
    QPushButton *browse_button{nullptr};
    QPushButton *clear_button{nullptr};
};

QString to_qstring(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QString video_file_filter() {
    return "Video Files (*.avi *.mkv *.mov *.mp4 *.webm);;All Files (*)";
}

PathRowWidgets create_path_row(
    QWidget *parent,
    const QString &placeholder_text,
    const bool optional
) {
    auto *container = new QWidget(parent);
    auto *layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);

    auto *line_edit = new QLineEdit(container);
    auto *browse_button = new QPushButton("Browse...", container);

    line_edit->setPlaceholderText(placeholder_text);

    layout->addWidget(line_edit, 1);
    layout->addWidget(browse_button);

    QPushButton *clear_button = nullptr;
    if (optional) {
        clear_button = new QPushButton("Clear", container);
        layout->addWidget(clear_button);
    }

    return PathRowWidgets{
        .container = container,
        .line_edit = line_edit,
        .browse_button = browse_button,
        .clear_button = clear_button
    };
}

}  // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle(QString("%1 %2")
                       .arg(to_qstring(utsure::core::BuildInfo::project_name()))
                       .arg(to_qstring(utsure::core::BuildInfo::project_version())));
    resize(960, 720);

    auto *central_widget = new QWidget(this);
    auto *main_layout = new QVBoxLayout(central_widget);

    auto *summary_label = new QLabel(
        "Build one encode job against encoder-core. The window only collects paths and settings, then delegates the run to the core pipeline.",
        central_widget
    );
    summary_label->setWordWrap(true);

    inputs_group_ = new QGroupBox("Inputs", central_widget);
    auto *inputs_layout = new QFormLayout(inputs_group_);

    const auto source_row = create_path_row(inputs_group_, "Required main source video", false);
    source_path_edit_ = source_row.line_edit;
    source_path_edit_->setObjectName("sourcePathEdit");
    connect(source_row.browse_button, &QPushButton::clicked, this, &MainWindow::choose_source_video);
    inputs_layout->addRow("Source video", source_row.container);

    const auto subtitle_row = create_path_row(inputs_group_, "Optional subtitle file", true);
    subtitle_path_edit_ = subtitle_row.line_edit;
    subtitle_path_edit_->setObjectName("subtitlePathEdit");
    connect(subtitle_row.browse_button, &QPushButton::clicked, this, &MainWindow::choose_subtitle_file);
    connect(subtitle_row.clear_button, &QPushButton::clicked, subtitle_path_edit_, &QLineEdit::clear);
    inputs_layout->addRow("Subtitle file", subtitle_row.container);

    const auto intro_row = create_path_row(inputs_group_, "Optional intro clip", true);
    intro_path_edit_ = intro_row.line_edit;
    intro_path_edit_->setObjectName("introPathEdit");
    connect(intro_row.browse_button, &QPushButton::clicked, this, &MainWindow::choose_intro_clip);
    connect(intro_row.clear_button, &QPushButton::clicked, intro_path_edit_, &QLineEdit::clear);
    inputs_layout->addRow("Intro clip", intro_row.container);

    const auto outro_row = create_path_row(inputs_group_, "Optional outro clip", true);
    outro_path_edit_ = outro_row.line_edit;
    outro_path_edit_->setObjectName("outroPathEdit");
    connect(outro_row.browse_button, &QPushButton::clicked, this, &MainWindow::choose_outro_clip);
    connect(outro_row.clear_button, &QPushButton::clicked, outro_path_edit_, &QLineEdit::clear);
    inputs_layout->addRow("Outro clip", outro_row.container);

    output_group_ = new QGroupBox("Output", central_widget);
    auto *output_layout = new QFormLayout(output_group_);

    codec_combo_ = new QComboBox(output_group_);
    codec_combo_->setObjectName("codecCombo");
    codec_combo_->addItem("H.264", static_cast<int>(utsure::core::media::OutputVideoCodec::h264));
    codec_combo_->addItem("H.265", static_cast<int>(utsure::core::media::OutputVideoCodec::h265));
    output_layout->addRow("Codec", codec_combo_);

    preset_combo_ = new QComboBox(output_group_);
    preset_combo_->setObjectName("presetCombo");
    preset_combo_->setEditable(true);
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
    preset_combo_->setCurrentText("medium");
    output_layout->addRow("Preset", preset_combo_);

    crf_spin_box_ = new QSpinBox(output_group_);
    crf_spin_box_->setObjectName("crfSpinBox");
    crf_spin_box_->setRange(0, 51);
    crf_spin_box_->setValue(23);
    output_layout->addRow("CRF", crf_spin_box_);

    const auto output_row = create_path_row(output_group_, "Required output path", false);
    output_path_edit_ = output_row.line_edit;
    output_path_edit_->setObjectName("outputPathEdit");
    connect(output_row.browse_button, &QPushButton::clicked, this, &MainWindow::choose_output_path);
    output_layout->addRow("Output path", output_row.container);

    auto *run_group = new QGroupBox("Run", central_widget);
    auto *run_layout = new QGridLayout(run_group);

    status_label_ = new QLabel(run_group);
    status_label_->setObjectName("statusLabel");
    status_label_->setWordWrap(true);

    progress_bar_ = new QProgressBar(run_group);
    progress_bar_->setObjectName("progressBar");
    progress_bar_->setRange(0, 1);
    progress_bar_->setValue(0);

    start_button_ = new QPushButton("Start Encode", run_group);
    start_button_->setObjectName("startEncodeButton");
    connect(start_button_, &QPushButton::clicked, this, &MainWindow::start_encode);

    run_layout->addWidget(new QLabel("Status", run_group), 0, 0);
    run_layout->addWidget(status_label_, 0, 1);
    run_layout->addWidget(new QLabel("Progress", run_group), 1, 0);
    run_layout->addWidget(progress_bar_, 1, 1);
    run_layout->addWidget(start_button_, 0, 2, 2, 1);

    auto *logs_group = new QGroupBox("Logs", central_widget);
    auto *logs_layout = new QVBoxLayout(logs_group);

    log_view_ = new QPlainTextEdit(logs_group);
    log_view_->setObjectName("logView");
    log_view_->setReadOnly(true);
    log_view_->setLineWrapMode(QPlainTextEdit::NoWrap);
    log_view_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    logs_layout->addWidget(log_view_);

    main_layout->addWidget(summary_label);
    main_layout->addWidget(inputs_group_);
    main_layout->addWidget(output_group_);
    main_layout->addWidget(run_group);
    main_layout->addWidget(logs_group, 1);

    setCentralWidget(central_widget);

    runner_controller_ = new EncodeJobRunnerController(this);
    connect(runner_controller_, &EncodeJobRunnerController::running_changed, this, &MainWindow::handle_running_changed);
    connect(runner_controller_, &EncodeJobRunnerController::progress_changed, this, &MainWindow::handle_progress_changed);
    connect(runner_controller_, &EncodeJobRunnerController::log_message, this, &MainWindow::append_log_line);
    connect(runner_controller_, &EncodeJobRunnerController::job_finished, this, &MainWindow::handle_job_finished);

    set_status_text("Ready to build an encode job.", false);
    append_log_line("[info] Window ready.");
}

QString MainWindow::window_structure_summary() const {
    return QString(
        "Main window structure:\n"
        "- Inputs: source video, subtitle file, optional intro clip, optional outro clip\n"
        "- Output: codec, preset, CRF, output path\n"
        "- Run: status label, progress bar, start encode button\n"
        "- Logs: read-only text pane for core stage logs, reports, and errors"
    );
}

std::optional<utsure::core::job::EncodeJob> MainWindow::build_job(QString &error_message) const {
    const QString source_text = source_path_edit_->text().trimmed();
    if (source_text.isEmpty()) {
        error_message = "Choose a source video before starting the encode.";
        return std::nullopt;
    }

    const QString output_text = output_path_edit_->text().trimmed();
    if (output_text.isEmpty()) {
        error_message = "Choose an output path before starting the encode.";
        return std::nullopt;
    }

    const QString preset_text = preset_combo_->currentText().trimmed();
    if (preset_text.isEmpty()) {
        error_message = "The encoder preset must not be empty.";
        return std::nullopt;
    }

    const auto codec_value = codec_combo_->currentData().toInt();
    const auto codec = codec_value == static_cast<int>(utsure::core::media::OutputVideoCodec::h265)
        ? utsure::core::media::OutputVideoCodec::h265
        : utsure::core::media::OutputVideoCodec::h264;

    utsure::core::job::EncodeJob job{};
    job.input.main_source_path = qstring_to_path(source_text);
    job.output.output_path = qstring_to_path(output_text);
    job.output.video.codec = codec;
    job.output.video.preset = preset_text.toUtf8().toStdString();
    job.output.video.crf = crf_spin_box_->value();

    const QString subtitle_text = subtitle_path_edit_->text().trimmed();
    if (!subtitle_text.isEmpty()) {
        QString format_hint = QFileInfo(subtitle_text).suffix().trimmed().toLower();
        if (format_hint.isEmpty()) {
            format_hint = "ass";
        }

        job.subtitles = utsure::core::job::EncodeJobSubtitleSettings{
            .subtitle_path = qstring_to_path(subtitle_text),
            .format_hint = format_hint.toUtf8().toStdString()
        };
    }

    const QString intro_text = intro_path_edit_->text().trimmed();
    if (!intro_text.isEmpty()) {
        job.input.intro_source_path = qstring_to_path(intro_text);
    }

    const QString outro_text = outro_path_edit_->text().trimmed();
    if (!outro_text.isEmpty()) {
        job.input.outro_source_path = qstring_to_path(outro_text);
    }

    return job;
}

void MainWindow::choose_source_video() {
    const QString selected_path = QFileDialog::getOpenFileName(
        this,
        "Choose Source Video",
        source_path_edit_->text().trimmed(),
        video_file_filter()
    );
    if (selected_path.isEmpty()) {
        return;
    }

    source_path_edit_->setText(QDir::toNativeSeparators(selected_path));
    maybe_seed_output_path();
}

void MainWindow::choose_subtitle_file() {
    const QString selected_path = QFileDialog::getOpenFileName(
        this,
        "Choose Subtitle File",
        subtitle_path_edit_->text().trimmed(),
        "Subtitle Files (*.ass *.ssa);;All Files (*)"
    );
    if (!selected_path.isEmpty()) {
        subtitle_path_edit_->setText(QDir::toNativeSeparators(selected_path));
    }
}

void MainWindow::choose_intro_clip() {
    const QString selected_path = QFileDialog::getOpenFileName(
        this,
        "Choose Intro Clip",
        intro_path_edit_->text().trimmed(),
        video_file_filter()
    );
    if (!selected_path.isEmpty()) {
        intro_path_edit_->setText(QDir::toNativeSeparators(selected_path));
    }
}

void MainWindow::choose_outro_clip() {
    const QString selected_path = QFileDialog::getOpenFileName(
        this,
        "Choose Outro Clip",
        outro_path_edit_->text().trimmed(),
        video_file_filter()
    );
    if (!selected_path.isEmpty()) {
        outro_path_edit_->setText(QDir::toNativeSeparators(selected_path));
    }
}

void MainWindow::choose_output_path() {
    QString suggested_path = output_path_edit_->text().trimmed();
    if (suggested_path.isEmpty()) {
        suggested_path = source_path_edit_->text().trimmed();
    }

    const QString selected_path = QFileDialog::getSaveFileName(
        this,
        "Choose Output Path",
        suggested_path,
        "MP4 Video (*.mp4);;All Files (*)"
    );
    if (!selected_path.isEmpty()) {
        output_path_edit_->setText(QDir::toNativeSeparators(selected_path));
    }
}

void MainWindow::start_encode() {
    if (runner_controller_->is_running()) {
        return;
    }

    QString error_message{};
    const auto job = build_job(error_message);
    if (!job.has_value()) {
        set_status_text(error_message, true);
        append_log_line("[error] " + error_message);
        return;
    }

    log_view_->clear();
    append_log_line("[info] Requested encode job.");
    append_log_line("[info] Source: " + source_path_edit_->text().trimmed());
    append_log_line("[info] Output: " + output_path_edit_->text().trimmed());
    set_status_text("Starting encode job.", false);
    runner_controller_->start_job(*job);
}

void MainWindow::handle_running_changed(const bool running) {
    inputs_group_->setEnabled(!running);
    output_group_->setEnabled(!running);
    start_button_->setEnabled(!running);
    start_button_->setText(running ? "Encoding..." : "Start Encode");

    if (running) {
        progress_bar_->setRange(0, 0);
        progress_bar_->setValue(0);
    } else if (progress_bar_->maximum() == 0) {
        progress_bar_->setRange(0, 1);
        progress_bar_->setValue(0);
    }
}

void MainWindow::handle_progress_changed(
    const int current_step,
    const int total_steps,
    const QString &status_text
) {
    if (total_steps > 0) {
        progress_bar_->setRange(0, total_steps);
        progress_bar_->setValue(current_step);
    } else {
        progress_bar_->setRange(0, 0);
    }

    set_status_text(status_text, false);
}

void MainWindow::handle_job_finished(
    const bool succeeded,
    const QString &status_text,
    const QString &details_text
) {
    if (succeeded) {
        if (progress_bar_->maximum() <= 0) {
            progress_bar_->setRange(0, 1);
            progress_bar_->setValue(1);
        } else {
            progress_bar_->setValue(progress_bar_->maximum());
        }

        append_log_line("[info] Encode report:");
    } else if (progress_bar_->maximum() <= 0) {
        progress_bar_->setRange(0, 1);
        progress_bar_->setValue(0);
        append_log_line("[error] Encode report:");
    } else {
        append_log_line("[error] Encode report:");
    }

    set_status_text(status_text, !succeeded);
    append_log_line(details_text);
}

void MainWindow::append_log_line(const QString &line) {
    log_view_->appendPlainText(line);
}

void MainWindow::set_status_text(const QString &text, const bool is_error) {
    status_label_->setText(text);
    status_label_->setStyleSheet(is_error ? "color: #8b0000; font-weight: 600;" : "font-weight: 600;");
}

void MainWindow::maybe_seed_output_path() {
    if (!output_path_edit_->text().trimmed().isEmpty()) {
        return;
    }

    const auto source_path = qstring_to_path(source_path_edit_->text().trimmed());
    if (source_path.empty()) {
        return;
    }

    auto suggested_path = source_path.parent_path() / source_path.stem();
    suggested_path += "-encoded.mp4";

    output_path_edit_->setText(path_to_qstring(suggested_path.lexically_normal()));
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
