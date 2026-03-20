#include "main_window.hpp"

#include "encode_job_runner_controller.hpp"
#include "utsure/core/build_info.hpp"
#include "utsure/core/job/encode_job_preflight.hpp"

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
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
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

utsure::core::media::OutputVideoCodec codec_from_index(const int index) {
    return index == static_cast<int>(utsure::core::media::OutputVideoCodec::h265)
        ? utsure::core::media::OutputVideoCodec::h265
        : utsure::core::media::OutputVideoCodec::h264;
}

utsure::core::media::AudioOutputMode audio_mode_from_value(const int value) {
    switch (static_cast<utsure::core::media::AudioOutputMode>(value)) {
    case utsure::core::media::AudioOutputMode::auto_select:
        return utsure::core::media::AudioOutputMode::auto_select;
    case utsure::core::media::AudioOutputMode::copy_source:
        return utsure::core::media::AudioOutputMode::copy_source;
    case utsure::core::media::AudioOutputMode::encode_aac:
        return utsure::core::media::AudioOutputMode::encode_aac;
    case utsure::core::media::AudioOutputMode::disable:
        return utsure::core::media::AudioOutputMode::disable;
    default:
        return utsure::core::media::AudioOutputMode::auto_select;
    }
}

utsure::core::media::OutputAudioCodec output_audio_codec_from_value(const int value) {
    switch (static_cast<utsure::core::media::OutputAudioCodec>(value)) {
    case utsure::core::media::OutputAudioCodec::aac:
        return utsure::core::media::OutputAudioCodec::aac;
    default:
        return utsure::core::media::OutputAudioCodec::aac;
    }
}

utsure::core::job::EncodeJobProcessPriority priority_from_value(const int value) {
    switch (static_cast<utsure::core::job::EncodeJobProcessPriority>(value)) {
    case utsure::core::job::EncodeJobProcessPriority::high:
        return utsure::core::job::EncodeJobProcessPriority::high;
    case utsure::core::job::EncodeJobProcessPriority::above_normal:
        return utsure::core::job::EncodeJobProcessPriority::above_normal;
    case utsure::core::job::EncodeJobProcessPriority::normal:
        return utsure::core::job::EncodeJobProcessPriority::normal;
    case utsure::core::job::EncodeJobProcessPriority::below_normal:
        return utsure::core::job::EncodeJobProcessPriority::below_normal;
    case utsure::core::job::EncodeJobProcessPriority::low:
        return utsure::core::job::EncodeJobProcessPriority::low;
    default:
        return utsure::core::job::EncodeJobProcessPriority::below_normal;
    }
}

QString recommended_preset(const utsure::core::media::OutputVideoCodec /*codec*/) {
    return "medium";
}

int recommended_crf(const utsure::core::media::OutputVideoCodec codec) {
    return codec == utsure::core::media::OutputVideoCodec::h265 ? 28 : 23;
}

QString codec_help_text(const utsure::core::media::OutputVideoCodec codec) {
    return codec == utsure::core::media::OutputVideoCodec::h265
        ? "Basic preset defaults for H.265: medium / CRF 28."
        : "Basic preset defaults for H.264: medium / CRF 23.";
}

QString video_file_filter() {
    return "Video Files (*.avi *.mkv *.mov *.mp4 *.webm);;All Files (*)";
}

QString format_preflight_issue(
    const utsure::core::job::EncodeJobPreflightIssue &issue
) {
    QString line = QString("[%1] %2")
        .arg(to_qstring(utsure::core::job::to_string(issue.severity)))
        .arg(to_qstring(issue.message));

    if (!issue.actionable_hint.empty()) {
        line += QString("\n        Hint: %1").arg(to_qstring(issue.actionable_hint));
    }

    return line;
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

double clamp_progress_fraction(const double value) {
    return std::clamp(value, 0.0, 1.0);
}

bool has_fine_encode_progress(const utsure::core::job::EncodeJobProgress &progress) {
    return progress.stage == utsure::core::job::EncodeJobStage::encoding_output &&
        (progress.stage_fraction.has_value() ||
         progress.encoded_video_frames.has_value() ||
         progress.encoded_video_duration_us.has_value() ||
         progress.encoded_fps.has_value());
}

QString format_duration_text(const std::int64_t microseconds) {
    const auto total_seconds = std::max<std::int64_t>(0, microseconds / 1000000);
    const auto hours = total_seconds / 3600;
    const auto minutes = (total_seconds % 3600) / 60;
    const auto seconds = total_seconds % 60;
    if (hours > 0) {
        return QString("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QChar('0'))
            .arg(seconds, 2, 10, QChar('0'));
    }

    return QString("%1:%2")
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'));
}

QString format_encode_progress_bar_text(const utsure::core::job::EncodeJobProgress &progress) {
    const double fraction = clamp_progress_fraction(
        progress.stage_fraction.value_or(progress.overall_fraction.value_or(0.0))
    );
    const int percent = static_cast<int>(std::lround(fraction * 100.0));

    QStringList parts{QString("Encoding %1%").arg(percent)};
    if (progress.encoded_video_frames.has_value() &&
        progress.total_video_frames.has_value() &&
        *progress.total_video_frames > 0U) {
        parts.push_back(
            QString("%1/%2 frames")
                .arg(QString::number(*progress.encoded_video_frames))
                .arg(QString::number(*progress.total_video_frames))
        );
    }

    if (progress.encoded_video_duration_us.has_value() &&
        progress.total_video_duration_us.has_value() &&
        *progress.total_video_duration_us > 0) {
        parts.push_back(
            QString("%1 / %2")
                .arg(format_duration_text(*progress.encoded_video_duration_us))
                .arg(format_duration_text(*progress.total_video_duration_us))
        );
    }

    if (progress.encoded_fps.has_value() && *progress.encoded_fps > 0.0) {
        parts.push_back(QString("%1 EFPS").arg(QString::number(*progress.encoded_fps, 'f', 1)));
    }

    return parts.join(" | ");
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
    preset_combo_->setEditable(false);
    preset_combo_->addItems(QStringList{
        "fast",
        "medium",
        "slow"
    });
    preset_combo_->setToolTip("Basic presets: fast, medium, or slow.");
    output_layout->addRow("Preset", preset_combo_);

    crf_spin_box_ = new QSpinBox(output_group_);
    crf_spin_box_->setObjectName("crfSpinBox");
    crf_spin_box_->setRange(0, 51);
    output_layout->addRow("CRF", crf_spin_box_);

    const auto output_row = create_path_row(output_group_, "Required output path", false);
    output_path_edit_ = output_row.line_edit;
    output_path_edit_->setObjectName("outputPathEdit");
    connect(output_row.browse_button, &QPushButton::clicked, this, &MainWindow::choose_output_path);
    output_layout->addRow("Output path", output_row.container);

    audio_group_ = new QGroupBox("Audio", central_widget);
    auto *audio_layout = new QFormLayout(audio_group_);

    audio_mode_combo_ = new QComboBox(audio_group_);
    audio_mode_combo_->setObjectName("audioModeCombo");
    audio_mode_combo_->addItem("Auto", static_cast<int>(utsure::core::media::AudioOutputMode::auto_select));
    audio_mode_combo_->addItem("Copy source audio", static_cast<int>(utsure::core::media::AudioOutputMode::copy_source));
    audio_mode_combo_->addItem("Encode to AAC", static_cast<int>(utsure::core::media::AudioOutputMode::encode_aac));
    audio_mode_combo_->addItem("Disable audio", static_cast<int>(utsure::core::media::AudioOutputMode::disable));
    audio_mode_combo_->setToolTip("Auto copies audio only when the container and source codec are clearly safe; otherwise it encodes AAC.");
    audio_layout->addRow("Mode", audio_mode_combo_);

    audio_codec_combo_ = new QComboBox(audio_group_);
    audio_codec_combo_->setObjectName("audioCodecCombo");
    audio_codec_combo_->addItem("AAC", static_cast<int>(utsure::core::media::OutputAudioCodec::aac));
    audio_codec_combo_->setToolTip("Used when the job resolves to audio encoding.");
    audio_layout->addRow("Output codec", audio_codec_combo_);

    audio_bitrate_spin_box_ = new QSpinBox(audio_group_);
    audio_bitrate_spin_box_->setObjectName("audioBitrateSpinBox");
    audio_bitrate_spin_box_->setRange(32, 512);
    audio_bitrate_spin_box_->setSingleStep(16);
    audio_bitrate_spin_box_->setValue(192);
    audio_bitrate_spin_box_->setSuffix(" kbps");
    audio_layout->addRow("Bitrate", audio_bitrate_spin_box_);

    audio_sample_rate_combo_ = new QComboBox(audio_group_);
    audio_sample_rate_combo_->setObjectName("audioSampleRateCombo");
    audio_sample_rate_combo_->addItem("Auto", 0);
    audio_sample_rate_combo_->addItem("44100 Hz", 44100);
    audio_sample_rate_combo_->addItem("48000 Hz", 48000);
    audio_layout->addRow("Sample rate", audio_sample_rate_combo_);

    audio_channels_combo_ = new QComboBox(audio_group_);
    audio_channels_combo_->setObjectName("audioChannelsCombo");
    audio_channels_combo_->addItem("Auto", 0);
    audio_channels_combo_->addItem("Mono", 1);
    audio_channels_combo_->addItem("Stereo", 2);
    audio_channels_combo_->addItem("5.1", 6);
    audio_layout->addRow("Channels", audio_channels_combo_);

    auto *run_group = new QGroupBox("Run", central_widget);
    auto *run_layout = new QGridLayout(run_group);

    status_label_ = new QLabel(run_group);
    status_label_->setObjectName("statusLabel");
    status_label_->setWordWrap(true);

    preview_label_ = new QLabel(run_group);
    preview_label_->setObjectName("previewLabel");
    preview_label_->setWordWrap(true);

    progress_bar_ = new QProgressBar(run_group);
    progress_bar_->setObjectName("progressBar");
    progress_bar_->setRange(0, 1);
    progress_bar_->setValue(0);
    progress_bar_->setFormat("Ready");

    priority_combo_ = new QComboBox(run_group);
    priority_combo_->setObjectName("priorityCombo");
    priority_combo_->addItem("High", static_cast<int>(utsure::core::job::EncodeJobProcessPriority::high));
    priority_combo_->addItem(
        "Above Normal",
        static_cast<int>(utsure::core::job::EncodeJobProcessPriority::above_normal)
    );
    priority_combo_->addItem("Normal", static_cast<int>(utsure::core::job::EncodeJobProcessPriority::normal));
    priority_combo_->addItem(
        "Below Normal",
        static_cast<int>(utsure::core::job::EncodeJobProcessPriority::below_normal)
    );
    priority_combo_->addItem("Low", static_cast<int>(utsure::core::job::EncodeJobProcessPriority::low));
    priority_combo_->setCurrentIndex(priority_combo_->findData(
        static_cast<int>(utsure::core::job::EncodeJobProcessPriority::below_normal)
    ));
    priority_combo_->setToolTip(
        "Applies a safe process priority during encode and restores the previous priority afterward."
    );

    start_button_ = new QPushButton("Start Encode", run_group);
    start_button_->setObjectName("startEncodeButton");
    connect(start_button_, &QPushButton::clicked, this, &MainWindow::start_encode);

    run_layout->addWidget(new QLabel("Status", run_group), 0, 0);
    run_layout->addWidget(status_label_, 0, 1);
    run_layout->addWidget(new QLabel("Priority", run_group), 0, 2);
    run_layout->addWidget(priority_combo_, 0, 3);
    run_layout->addWidget(new QLabel("Preview", run_group), 1, 0);
    run_layout->addWidget(preview_label_, 1, 1);
    run_layout->addWidget(new QLabel("Progress", run_group), 2, 0);
    run_layout->addWidget(progress_bar_, 2, 1);
    run_layout->addWidget(start_button_, 1, 2, 2, 2);

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
    main_layout->addWidget(audio_group_);
    main_layout->addWidget(run_group);
    main_layout->addWidget(logs_group, 1);

    setCentralWidget(central_widget);

    runner_controller_ = new EncodeJobRunnerController(this);
    connect(runner_controller_, &EncodeJobRunnerController::running_changed, this, &MainWindow::handle_running_changed);
    connect(runner_controller_, &EncodeJobRunnerController::progress_changed, this, &MainWindow::handle_progress_changed);
    connect(runner_controller_, &EncodeJobRunnerController::log_message, this, &MainWindow::append_log_line);
    connect(runner_controller_, &EncodeJobRunnerController::job_finished, this, &MainWindow::handle_job_finished);
    connect(codec_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int /*index*/) {
        apply_codec_defaults();
        mark_preview_stale();
    });
    connect(source_path_edit_, &QLineEdit::textChanged, this, [this](const QString &) {
        mark_preview_stale();
    });
    connect(subtitle_path_edit_, &QLineEdit::textChanged, this, [this](const QString &) {
        mark_preview_stale();
    });
    connect(intro_path_edit_, &QLineEdit::textChanged, this, [this](const QString &) {
        mark_preview_stale();
    });
    connect(outro_path_edit_, &QLineEdit::textChanged, this, [this](const QString &) {
        mark_preview_stale();
    });
    connect(output_path_edit_, &QLineEdit::textChanged, this, [this](const QString &) {
        mark_preview_stale();
    });
    connect(audio_mode_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int /*index*/) {
        update_audio_control_states();
        mark_preview_stale();
    });
    connect(audio_codec_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int /*index*/) {
        mark_preview_stale();
    });
    connect(audio_bitrate_spin_box_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int /*value*/) {
        mark_preview_stale();
    });
    connect(audio_sample_rate_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int /*index*/) {
        mark_preview_stale();
    });
    connect(audio_channels_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int /*index*/) {
        mark_preview_stale();
    });
    connect(priority_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int /*index*/) {
        mark_preview_stale();
    });
    connect(preset_combo_, &QComboBox::currentTextChanged, this, [this](const QString &) {
        mark_preview_stale();
    });
    connect(crf_spin_box_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int /*value*/) {
        mark_preview_stale();
    });

    apply_codec_defaults();
    update_audio_control_states();
    set_status_text("Ready to build an encode job.", false);
    set_preview_text("Preview updates after input validation.", false);
    append_log_line("[info] Window ready.");
}

QString MainWindow::window_structure_summary() const {
    return QString(
        "Main window structure:\n"
        "- Inputs: source video, subtitle file, optional intro clip, optional outro clip\n"
        "- Output: codec, preset, CRF, output path\n"
        "- Audio: mode, codec, bitrate, sample rate, channels\n"
        "- Run: status label, priority selector, preview label, progress bar, start encode button\n"
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

    const auto codec = current_output_codec();
    const auto audio_mode = current_audio_mode();

    utsure::core::job::EncodeJob job{};
    job.input.main_source_path = qstring_to_path(source_text);
    job.output.output_path = qstring_to_path(output_text);
    job.output.video.codec = codec;
    job.output.video.preset = preset_text.toUtf8().toStdString();
    job.output.video.crf = crf_spin_box_->value();
    job.output.audio.mode = audio_mode;
    job.output.audio.codec = current_output_audio_codec();
    job.output.audio.bitrate_kbps = audio_bitrate_spin_box_->value();
    job.output.audio.sample_rate_hz =
        audio_mode == utsure::core::media::AudioOutputMode::auto_select ||
            audio_mode == utsure::core::media::AudioOutputMode::encode_aac
            ? current_audio_sample_rate_override()
            : std::nullopt;
    job.output.audio.channel_count =
        audio_mode == utsure::core::media::AudioOutputMode::auto_select ||
            audio_mode == utsure::core::media::AudioOutputMode::encode_aac
            ? current_audio_channel_override()
            : std::nullopt;
    job.execution.process_priority = current_encode_priority();

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

utsure::core::media::OutputVideoCodec MainWindow::current_output_codec() const {
    return codec_from_index(codec_combo_->currentData().toInt());
}

utsure::core::media::AudioOutputMode MainWindow::current_audio_mode() const {
    return audio_mode_from_value(audio_mode_combo_->currentData().toInt());
}

utsure::core::media::OutputAudioCodec MainWindow::current_output_audio_codec() const {
    return output_audio_codec_from_value(audio_codec_combo_->currentData().toInt());
}

utsure::core::job::EncodeJobProcessPriority MainWindow::current_encode_priority() const {
    return priority_from_value(priority_combo_->currentData().toInt());
}

std::optional<int> MainWindow::current_audio_sample_rate_override() const {
    const int sample_rate = audio_sample_rate_combo_->currentData().toInt();
    return sample_rate > 0 ? std::optional<int>(sample_rate) : std::nullopt;
}

std::optional<int> MainWindow::current_audio_channel_override() const {
    const int channel_count = audio_channels_combo_->currentData().toInt();
    return channel_count > 0 ? std::optional<int>(channel_count) : std::nullopt;
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

    log_view_->clear();
    QString error_message{};
    const auto job = build_job(error_message);
    if (!job.has_value()) {
        set_status_text(error_message, true);
        append_log_line("[error] " + error_message);
        return;
    }

    append_log_line("[info] Validating inputs before encode.");
    const auto preflight = utsure::core::job::EncodeJobPreflight::inspect(*job);

    if (preflight.preview_summary.has_value()) {
        const QString preview_text =
            to_qstring(utsure::core::job::format_encode_job_preview(*preflight.preview_summary));
        set_preview_text(preview_text, false);
        append_log_line("[info] Preview: " + preview_text);
    } else {
        set_preview_text("No preview available until the inputs pass validation.", true);
    }

    for (const auto &issue : preflight.issues) {
        append_log_line(format_preflight_issue(issue));
    }

    if (!preflight.can_start_encode()) {
        const auto first_issue = std::find_if(
            preflight.issues.begin(),
            preflight.issues.end(),
            [](const utsure::core::job::EncodeJobPreflightIssue &issue) {
                return issue.severity == utsure::core::job::EncodeJobPreflightIssueSeverity::error;
            }
        );
        const QString status_text = first_issue != preflight.issues.end()
            ? to_qstring(first_issue->message)
            : "Fix the input issues before starting the encode.";
        set_status_text(status_text, true);
        return;
    }

    if (preflight.requires_output_overwrite_confirmation()) {
        const auto response = QMessageBox::question(
            this,
            "Overwrite Existing Output?",
            "The selected output file already exists.\n\nDo you want to overwrite it?"
        );
        if (response != QMessageBox::Yes) {
            set_status_text("Encode cancelled before start.", false);
            append_log_line("[info] Overwrite was declined. The encode did not start.");
            return;
        }

        append_log_line("[warning] Existing output confirmed for overwrite.");
    }

    append_log_line("[info] Requested encode job.");
    append_log_line("[info] Source: " + source_path_edit_->text().trimmed());
    append_log_line("[info] Output: " + output_path_edit_->text().trimmed());
    append_log_line(
        QString("[info] Priority: %1")
            .arg(to_qstring(utsure::core::job::to_display_string(current_encode_priority())))
    );
    set_status_text("Validation passed. Starting encode job.", false);
    runner_controller_->start_job(*job);
}

void MainWindow::apply_codec_defaults() {
    const auto codec = current_output_codec();
    const QSignalBlocker preset_blocker(preset_combo_);
    const QSignalBlocker crf_blocker(crf_spin_box_);

    preset_combo_->setCurrentText(recommended_preset(codec));
    preset_combo_->setToolTip(codec_help_text(codec));
    crf_spin_box_->setValue(recommended_crf(codec));
    crf_spin_box_->setToolTip(codec_help_text(codec));
}

void MainWindow::update_audio_control_states() {
    const auto mode = current_audio_mode();
    const bool encode_controls_enabled =
        mode == utsure::core::media::AudioOutputMode::auto_select ||
        mode == utsure::core::media::AudioOutputMode::encode_aac;

    audio_codec_combo_->setEnabled(encode_controls_enabled);
    audio_bitrate_spin_box_->setEnabled(encode_controls_enabled);
    audio_sample_rate_combo_->setEnabled(encode_controls_enabled);
    audio_channels_combo_->setEnabled(encode_controls_enabled);
}

void MainWindow::handle_running_changed(const bool running) {
    inputs_group_->setEnabled(!running);
    output_group_->setEnabled(!running);
    audio_group_->setEnabled(!running);
    start_button_->setEnabled(!running);
    priority_combo_->setEnabled(!running);
    start_button_->setText(running ? "Encoding..." : "Start Encode");

    if (running) {
        progress_bar_->setRange(0, 0);
        progress_bar_->setValue(0);
        progress_bar_->setFormat("Working...");
    } else if (progress_bar_->maximum() == 0) {
        progress_bar_->setRange(0, 1);
        progress_bar_->setValue(0);
        progress_bar_->setFormat("Ready");
    }
}

void MainWindow::handle_progress_changed(const utsure::core::job::EncodeJobProgress &progress) {
    if (has_fine_encode_progress(progress)) {
        const double stage_fraction = clamp_progress_fraction(
            progress.stage_fraction.value_or(progress.overall_fraction.value_or(0.0))
        );
        progress_bar_->setRange(0, 1000);
        progress_bar_->setValue(static_cast<int>(std::lround(stage_fraction * 1000.0)));
        progress_bar_->setFormat(format_encode_progress_bar_text(progress));
        set_status_text(
            progress.message.empty() ? "Encoding output..." : to_qstring(progress.message),
            false
        );
        return;
    }

    if (progress.total_steps > 0) {
        progress_bar_->setRange(0, progress.total_steps);
        progress_bar_->setValue(progress.current_step);
        progress_bar_->setFormat(
            QString("Step %1 of %2").arg(progress.current_step).arg(progress.total_steps)
        );
    } else {
        progress_bar_->setRange(0, 0);
        progress_bar_->setFormat("Working...");
    }

    set_status_text(to_qstring(progress.message), false);
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
        progress_bar_->setFormat("Completed");

        append_log_line("[info] Encode report:");
    } else if (progress_bar_->maximum() <= 0) {
        progress_bar_->setRange(0, 1);
        progress_bar_->setValue(0);
        progress_bar_->setFormat("Failed");
        append_log_line("[error] Encode report:");
    } else {
        progress_bar_->setFormat("Failed");
        append_log_line("[error] Encode report:");
    }

    set_status_text(status_text, !succeeded);
    append_log_line(details_text);
}

void MainWindow::append_log_line(const QString &line) {
    log_view_->appendPlainText(line);
}

void MainWindow::mark_preview_stale() {
    if (runner_controller_ != nullptr && runner_controller_->is_running()) {
        return;
    }

    set_preview_text("Preview updates after input validation.", false);
}

void MainWindow::set_preview_text(const QString &text, const bool is_error) {
    preview_label_->setText(text);
    preview_label_->setStyleSheet(is_error ? "color: #8b0000;" : "color: #444444;");
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
