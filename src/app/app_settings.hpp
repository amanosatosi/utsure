#pragma once

#include "utsure/core/job/output_naming.hpp"
#include "utsure/core/job/resize.hpp"
#include "utsure/core/media/audio_output.hpp"
#include "utsure/core/media/media_encoder.hpp"

#include <QString>

#include <map>
#include <string>
#include <vector>

class AppSettings final {
public:
    static constexpr int kCurrentVersion = 1;

    struct LastUsedEncodeChoices final {
        utsure::core::media::OutputVideoCodec codec{utsure::core::media::OutputVideoCodec::h265};
        QString preset{"fast"};
        int crf{22};
        utsure::core::media::AudioOutputMode audio_mode{utsure::core::media::AudioOutputMode::encode_aac};
        int audio_bitrate_kbps{192};
    };

    struct UiFontSettings final {
        QString family{"Pyidaungsu"};
        int point_size{10};
        bool use_bundled_myanmar_fallback{true};
    };

    struct EncodingProfile final {
        QString name{};
        LastUsedEncodeChoices encode{};
        utsure::core::job::EncodeResizeSettings resize{};
    };

    [[nodiscard]] static QString default_config_file_path();
    [[nodiscard]] static AppSettings defaults();
    struct LoadResult;
    [[nodiscard]] static LoadResult load(const QString &config_path);
    [[nodiscard]] static LoadResult load_default_location();

    [[nodiscard]] bool save(const QString &config_path, QString *error_message = nullptr) const;
    [[nodiscard]] bool save_default_location(QString *error_message = nullptr) const;

    [[nodiscard]] int sequence_counter_value(const std::string &key) const;
    void set_sequence_counter_value(const std::string &key, int value);
    void remember_encode_choices(const LastUsedEncodeChoices &choices);
    [[nodiscard]] static std::vector<EncodingProfile> default_encoding_profiles();

    int version{kCurrentVersion};
    LastUsedEncodeChoices last_used{};
    utsure::core::job::OutputNamingTemplate output_naming{};
    UiFontSettings ui_font{};
    bool toshi_mode_enabled{false};
    std::vector<EncodingProfile> encoding_profiles{};
    QString last_used_profile{};
    std::map<std::string, int> sequence_counters{};
};

struct AppSettings::LoadResult final {
    AppSettings settings{};
    QString warning{};
    QString config_path{};
    QString backup_path{};
};
