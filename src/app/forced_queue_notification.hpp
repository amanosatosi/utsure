#pragma once

#include "queue_terminal_notification.hpp"

#include <QPointer>
#include <QWidget>

#include <functional>
#include <optional>

class QCloseEvent;
class QLabel;
class QPaintEvent;
class QPropertyAnimation;
class QPushButton;
class QShowEvent;
class QSoundEffect;
class QTimer;
class QToolButton;

namespace utsure::app {

class ForcedQueueNotification final : public QWidget {
public:
    explicit ForcedQueueNotification(QWidget *owner_window);
    ~ForcedQueueNotification() override;

    void set_open_logs_handler(std::function<void()> handler);
    void set_log_handler(std::function<void(const QString &)> handler);
    void present(const JobTerminalNotificationData &data);
    void dismiss();

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    bool nativeEvent(const QByteArray &event_type, void *message, qintptr *result) override;

private:
    void apply_theme();
    void update_content();
    void update_mascot(const QString &resource_path);
    void position_inside_work_area();
    void apply_windows_no_activate(bool show_window);
    void play_sound_once(bool success);
    void log_sound_failure_once(bool success, const QSoundEffect *sound_effect, const QString &reason);
    void start_lifetime(quint64 run_id);
    void begin_fade(quint64 run_id);
    void stop_lifetime();
    void stop_sounds();
    void handle_primary_action();
    void handle_secondary_action();
    void log_message(const QString &message) const;
    [[nodiscard]] QString output_directory() const;

    std::optional<JobTerminalNotificationData> current_data_{};
    QPointer<QWidget> owner_window_{};
    quint64 last_presented_run_id_{0};
    quint64 audio_failure_logged_run_id_{0};
    std::function<void()> open_logs_handler_{};
    std::function<void(const QString &)> log_handler_{};
    QLabel *title_label_{nullptr};
    QLabel *result_label_{nullptr};
    QLabel *information_label_{nullptr};
    QLabel *mascot_label_{nullptr};
    QWidget *status_icon_{nullptr};
    QWidget *forced_badge_{nullptr};
    QToolButton *close_button_{nullptr};
    QPushButton *primary_button_{nullptr};
    QPushButton *secondary_button_{nullptr};
    QPushButton *dismiss_button_{nullptr};
    QSoundEffect *success_sound_{nullptr};
    QSoundEffect *failure_sound_{nullptr};
    QTimer *visible_timer_{nullptr};
    QPropertyAnimation *fade_animation_{nullptr};
    quint64 lifetime_run_id_{0};
};

}  // namespace utsure::app
