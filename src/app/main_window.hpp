#pragma once

#include "utsure/core/job/encode_job.hpp"

#include <QElapsedTimer>
#include <QImage>
#include <QMainWindow>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

class EncodeJobRunnerController;
class QEvent;
class QLabel;
class QCheckBox;
class QComboBox;
class QLineEdit;
class PreviewAudioController;
class PreviewFrameRendererController;
class PreviewSurfaceWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTableWidget;
class QTableWidgetItem;
class QToolButton;
class QTimer;
class TrimTimelineWidget;

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QWidget *parent = nullptr);

    [[nodiscard]] QString window_structure_summary() const;
    enum class UiJobState : std::uint8_t {
        pending = 0,
        encoding,
        finished,
        failed,
        canceled
    };

    struct UiEncodeJob final {
        QString source_path{};
        QString source_name{};
        QString type_label{};
        bool checked{true};
        UiJobState state{UiJobState::pending};
        QString output_path{};
        bool same_as_input{true};
        bool subtitle_enabled{false};
        QString subtitle_path{};
        bool intro_enabled{false};
        QString intro_path{};
        bool intro_music_enabled{false};
        QString intro_music_path{};
        bool endcard_enabled{false};
        QString endcard_path{};
        bool endcard_music_enabled{false};
        QString endcard_music_path{};
        QString thumbnail_image_path{};
        QString thumbnail_title{};
        utsure::core::media::OutputVideoCodec video_codec{utsure::core::media::OutputVideoCodec::h265};
        QString video_preset{"medium"};
        int video_crf{28};
        utsure::core::media::AudioOutputMode audio_mode{utsure::core::media::AudioOutputMode::encode_aac};
        int audio_bitrate_kbps{192};
        QString audio_track_display{"Primary track"};
        qint64 current_time_us{0};
        qint64 trim_in_us{0};
        qint64 trim_out_us{10000000};
        qint64 duration_us{10000000};
        QString inspected_source_key{};
        std::optional<utsure::core::media::MediaSourceInfo> inspected_source_info{};
        QString source_inspection_error{};
        QString last_status_message{"Select an output path to make the job runnable."};
        QString last_details_summary{};
        QStringList task_log{};
        QString efps_display{};
        QString speed_display{};
        qint64 elapsed_ms{0};
        qint64 remaining_ms{-1};
        qint64 input_size_bytes{-1};
        qint64 output_size_bytes{-1};
    };

private:
    bool eventFilter(QObject *watched, QEvent *event) override;
    [[nodiscard]] std::optional<utsure::core::job::EncodeJob> build_job_from_entry(
        int job_index,
        QString &error_message
    ) const;
    [[nodiscard]] utsure::core::job::EncodeJobProcessPriority current_worker_priority() const;
    [[nodiscard]] QString selected_job_name() const;
    [[nodiscard]] QString format_job_state_text(const UiEncodeJob &job) const;
    [[nodiscard]] QString format_job_state_display_text(const UiEncodeJob &job) const;
    [[nodiscard]] bool job_is_terminal(const UiEncodeJob &job) const;
    [[nodiscard]] bool job_has_minimum_required_fields(const UiEncodeJob &job) const;
    [[nodiscard]] QString current_audio_quality_label() const;
    [[nodiscard]] bool is_valid_job_index(int index) const;
    [[nodiscard]] qint64 selected_job_frame_step_us() const;

    void add_source_jobs();
    void add_source_jobs_from_paths(const QStringList &paths);
    void remove_selected_job();
    void show_settings_placeholder();
    void show_info_dialog();

    void choose_output_path();
    void choose_subtitle_file();
    void choose_intro_clip();
    void choose_intro_music_file();
    void choose_endcard_clip();
    void choose_endcard_music_file();
    void choose_thumbnail_image();
    void show_thumbnail_placeholder_note();

    void handle_queue_selection_changed();
    void handle_queue_item_changed(QTableWidgetItem *item);
    void handle_same_as_input_toggled(bool enabled);
    void handle_preview_toggled(bool enabled);

    void step_selected_job_frame(int direction);
    void set_selected_job_trim_in();
    void set_selected_job_trim_out();
    void jump_selected_job_to_in();
    void jump_selected_job_to_out();
    void handle_timeline_seek(qint64 time_us);

    void sync_selected_job_from_editor();
    void load_selected_job_into_editor();
    void select_job(int index);
    void ensure_job_inspection(int job_index);
    void reset_job_for_rerun(UiEncodeJob &job);
    void apply_same_as_input_folder(UiEncodeJob &job);
    void request_selected_job_preview_frame();
    void clear_preview_surface();
    void start_preview_playback();
    void pause_preview_playback();
    void stop_preview_playback();
    void sync_preview_surface_state();
    void layout_preview_controls_overlay();
    void refresh_preview_footer_visibility();
    void request_preview_frame_for_time(qint64 requested_time_us);
    void handle_preview_loading(quint64 request_token, qint64 requested_time_us);
    void handle_preview_ready(
        quint64 request_token,
        qint64 requested_time_us,
        qint64 frame_time_us,
        qint64 frame_duration_us,
        const QImage &image
    );
    void handle_preview_failed(quint64 request_token, qint64 requested_time_us, const QString &title, const QString &detail);
    void handle_preview_surface_clicked();
    void handle_preview_play_pause_requested();
    void handle_preview_stop_requested();
    void handle_preview_playback_tick();

    void refresh_all_views();
    void refresh_queue_table();
    void refresh_editor_state();
    void refresh_selected_job_details();
    void refresh_selected_job_preview();
    void refresh_trim_controls();
    void refresh_task_log_view();
    void refresh_session_log_view();
    void refresh_toolbar_state();
    void refresh_audio_track_combo();
    void update_start_button_visuals();
    void advance_busy_spinner();

    void start_encode_queue();
    void start_next_queued_job();
    void stop_encode_queue();
    void finish_queue_run();
    void append_session_log(const QString &line);
    void append_job_log(int job_index, const QString &line, bool mirror_to_session = true);
    void update_active_job_progress(const utsure::core::job::EncodeJobProgress &progress);
    void update_job_file_sizes(UiEncodeJob &job);

    void handle_running_changed(bool running);
    void handle_progress_changed(const utsure::core::job::EncodeJobProgress &progress);
    void handle_job_finished(bool succeeded, bool canceled, const QString &status_text, const QString &details_text);

    static std::filesystem::path qstring_to_path(const QString &text);
    static QString path_to_qstring(const std::filesystem::path &path);

    std::vector<UiEncodeJob> jobs_{};
    std::vector<int> queued_job_indices_{};
    int selected_job_index_{-1};
    int active_job_index_{-1};
    int queue_cursor_{0};
    bool queue_run_active_{false};
    bool stop_requested_{false};
    bool loading_selected_job_{false};
    bool suppress_queue_table_changes_{false};
    int busy_spinner_phase_{0};
    QElapsedTimer active_job_elapsed_timer_{};
    bool active_job_elapsed_valid_{false};
    bool preview_playing_{false};
    QStringList session_log_lines_{};
    quint64 preview_request_token_{0};
    int preview_requested_job_index_{-1};
    qint64 preview_requested_time_us_{-1};
    qint64 preview_next_playback_time_us_{-1};
    QString preview_requested_source_path_{};
    QString preview_requested_subtitle_path_{};
    QString preview_requested_subtitle_format_hint_{"auto"};
    bool preview_requested_subtitle_enabled_{false};
    bool preview_request_in_flight_{false};

    QTableWidget *queue_table_{nullptr};
    QLabel *detail_status_value_{nullptr};
    QLabel *detail_elapsed_value_{nullptr};
    QLabel *detail_remaining_value_{nullptr};
    QLabel *detail_efps_value_{nullptr};
    QLabel *detail_speed_value_{nullptr};
    QLabel *detail_input_size_value_{nullptr};
    QLabel *detail_output_size_value_{nullptr};
    QLabel *detail_timeline_value_{nullptr};
    QLineEdit *output_path_edit_{nullptr};
    QPushButton *output_browse_button_{nullptr};
    QCheckBox *same_as_input_check_{nullptr};
    QTabWidget *editor_tabs_{nullptr};
    QCheckBox *subtitle_enable_check_{nullptr};
    QLineEdit *subtitle_path_edit_{nullptr};
    QPushButton *subtitle_browse_button_{nullptr};
    QCheckBox *intro_enable_check_{nullptr};
    QLineEdit *intro_path_edit_{nullptr};
    QPushButton *intro_browse_button_{nullptr};
    QCheckBox *intro_music_check_{nullptr};
    QLineEdit *intro_music_path_edit_{nullptr};
    QPushButton *intro_music_browse_button_{nullptr};
    QCheckBox *endcard_enable_check_{nullptr};
    QLineEdit *endcard_path_edit_{nullptr};
    QPushButton *endcard_browse_button_{nullptr};
    QCheckBox *endcard_music_check_{nullptr};
    QLineEdit *endcard_music_path_edit_{nullptr};
    QPushButton *endcard_music_browse_button_{nullptr};
    QComboBox *video_codec_combo_{nullptr};
    QComboBox *preset_combo_{nullptr};
    QSpinBox *crf_spin_box_{nullptr};
    QComboBox *audio_format_combo_{nullptr};
    QComboBox *audio_quality_combo_{nullptr};
    QComboBox *audio_track_combo_{nullptr};
    QLineEdit *thumbnail_image_path_edit_{nullptr};
    QPushButton *thumbnail_image_browse_button_{nullptr};
    QLineEdit *thumbnail_title_edit_{nullptr};
    QPushButton *thumbnail_load_ass_button_{nullptr};
    QPushButton *thumbnail_edit_title_button_{nullptr};
    QPlainTextEdit *session_log_view_{nullptr};
    QPlainTextEdit *task_log_view_{nullptr};
    QLabel *task_log_summary_label_{nullptr};
    QCheckBox *preview_enabled_check_{nullptr};
    PreviewSurfaceWidget *preview_surface_widget_{nullptr};
    QWidget *preview_controls_panel_{nullptr};
    QLabel *preview_time_badge_{nullptr};
    TrimTimelineWidget *trim_timeline_widget_{nullptr};
    QLabel *current_time_value_{nullptr};
    QLabel *trim_in_value_{nullptr};
    QLabel *trim_out_value_{nullptr};
    QPushButton *preview_play_pause_button_{nullptr};
    QPushButton *preview_stop_button_{nullptr};
    QPushButton *frame_back_button_{nullptr};
    QPushButton *frame_forward_button_{nullptr};
    QPushButton *set_in_button_{nullptr};
    QPushButton *set_out_button_{nullptr};
    QPushButton *jump_in_button_{nullptr};
    QPushButton *jump_out_button_{nullptr};
    QToolButton *add_button_{nullptr};
    QToolButton *remove_button_{nullptr};
    QToolButton *settings_button_{nullptr};
    QToolButton *info_button_{nullptr};
    QComboBox *priority_combo_{nullptr};
    QToolButton *start_button_{nullptr};
    QToolButton *stop_button_{nullptr};
    QTimer *busy_spinner_timer_{nullptr};
    QTimer *preview_playback_timer_{nullptr};
    PreviewAudioController *preview_audio_controller_{nullptr};
    EncodeJobRunnerController *runner_controller_{nullptr};
    PreviewFrameRendererController *preview_renderer_controller_{nullptr};
};
