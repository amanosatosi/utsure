#pragma once

#include "utsure/core/job/encode_job.hpp"

#include <QMainWindow>
#include <QString>

#include <filesystem>
#include <optional>

class EncodeJobRunnerController;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QWidget *parent = nullptr);

    [[nodiscard]] QString window_structure_summary() const;

private:
    [[nodiscard]] std::optional<utsure::core::job::EncodeJob> build_job(QString &error_message) const;
    [[nodiscard]] utsure::core::media::OutputVideoCodec current_output_codec() const;

    void choose_source_video();
    void choose_subtitle_file();
    void choose_intro_clip();
    void choose_outro_clip();
    void choose_output_path();
    void start_encode();
    void apply_codec_defaults();
    void handle_running_changed(bool running);
    void handle_progress_changed(int current_step, int total_steps, const QString &status_text);
    void handle_job_finished(bool succeeded, const QString &status_text, const QString &details_text);
    void append_log_line(const QString &line);
    void mark_preview_stale();
    void set_preview_text(const QString &text, bool is_error);
    void set_status_text(const QString &text, bool is_error);
    void maybe_seed_output_path();

    static std::filesystem::path qstring_to_path(const QString &text);
    static QString path_to_qstring(const std::filesystem::path &path);

    QGroupBox *inputs_group_{nullptr};
    QGroupBox *output_group_{nullptr};
    QLabel *status_label_{nullptr};
    QLabel *preview_label_{nullptr};
    QProgressBar *progress_bar_{nullptr};
    QLineEdit *source_path_edit_{nullptr};
    QLineEdit *subtitle_path_edit_{nullptr};
    QLineEdit *intro_path_edit_{nullptr};
    QLineEdit *outro_path_edit_{nullptr};
    QComboBox *codec_combo_{nullptr};
    QComboBox *preset_combo_{nullptr};
    QSpinBox *crf_spin_box_{nullptr};
    QLineEdit *output_path_edit_{nullptr};
    QPushButton *start_button_{nullptr};
    QPlainTextEdit *log_view_{nullptr};
    EncodeJobRunnerController *runner_controller_{nullptr};
};
