#include "app_settings.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace {

QString video_codec_to_json(const utsure::core::media::OutputVideoCodec codec) {
    switch (codec) {
    case utsure::core::media::OutputVideoCodec::h265:
        return "h265";
    case utsure::core::media::OutputVideoCodec::h264:
    default:
        return "h264";
    }
}

utsure::core::media::OutputVideoCodec video_codec_from_json(
    const QJsonValue &value,
    const utsure::core::media::OutputVideoCodec fallback
) {
    const QString text = value.toString().trimmed().toLower();
    if (text == "h265" || text == "hevc" || text == "x265") {
        return utsure::core::media::OutputVideoCodec::h265;
    }
    if (text == "h264" || text == "avc" || text == "x264") {
        return utsure::core::media::OutputVideoCodec::h264;
    }

    return fallback;
}

QString audio_mode_to_json(const utsure::core::media::AudioOutputMode mode) {
    switch (mode) {
    case utsure::core::media::AudioOutputMode::copy_source:
        return "copy";
    case utsure::core::media::AudioOutputMode::disable:
        return "disabled";
    case utsure::core::media::AudioOutputMode::auto_select:
        return "auto";
    case utsure::core::media::AudioOutputMode::encode_aac:
    default:
        return "aac";
    }
}

utsure::core::media::AudioOutputMode audio_mode_from_json(
    const QJsonValue &value,
    const utsure::core::media::AudioOutputMode fallback
) {
    const QString text = value.toString().trimmed().toLower();
    if (text == "copy" || text == "copy_source" || text == "source") {
        return utsure::core::media::AudioOutputMode::copy_source;
    }
    if (text == "aac" || text == "encode_aac") {
        return utsure::core::media::AudioOutputMode::encode_aac;
    }
    if (text == "disabled" || text == "disable" || text == "none") {
        return utsure::core::media::AudioOutputMode::disable;
    }
    if (text == "auto" || text == "auto_select") {
        return utsure::core::media::AudioOutputMode::auto_select;
    }

    return fallback;
}

QString safe_preset_from_json(const QJsonValue &value, const QString &fallback) {
    static const QStringList kValidPresets{
        "ultrafast",
        "superfast",
        "veryfast",
        "faster",
        "fast",
        "medium",
        "slow",
        "slower",
        "veryslow"
    };

    const QString preset = value.toString().trimmed().toLower();
    return kValidPresets.contains(preset) ? preset : fallback;
}

int bounded_int_from_json(const QJsonValue &value, const int fallback, const int minimum, const int maximum) {
    if (!value.isDouble()) {
        return fallback;
    }

    const int parsed_value = value.toInt(fallback);
    if (parsed_value < minimum || parsed_value > maximum) {
        return fallback;
    }

    return parsed_value;
}

QJsonObject encode_choices_to_json(const AppSettings::LastUsedEncodeChoices &choices) {
    QJsonObject object;
    object.insert("codec", video_codec_to_json(choices.codec));
    object.insert("preset", choices.preset);
    object.insert("crf", choices.crf);
    object.insert("audioMode", audio_mode_to_json(choices.audio_mode));
    object.insert("audioBitrateKbps", choices.audio_bitrate_kbps);
    return object;
}

AppSettings::LastUsedEncodeChoices encode_choices_from_json(
    const QJsonObject &object,
    const AppSettings::LastUsedEncodeChoices &fallback
) {
    AppSettings::LastUsedEncodeChoices choices = fallback;
    choices.codec = video_codec_from_json(object.value("codec"), fallback.codec);
    choices.preset = safe_preset_from_json(object.value("preset"), fallback.preset);
    choices.crf = bounded_int_from_json(object.value("crf"), fallback.crf, 0, 51);
    choices.audio_mode = audio_mode_from_json(object.value("audioMode"), fallback.audio_mode);
    choices.audio_bitrate_kbps = bounded_int_from_json(object.value("audioBitrateKbps"), fallback.audio_bitrate_kbps, 64, 512);
    return choices;
}

QString resize_mode_to_json(const utsure::core::job::EncodeResizeMode mode) {
    return QString::fromUtf8(utsure::core::job::to_string(mode));
}

utsure::core::job::EncodeResizeSettings resize_settings_from_json(
    const QJsonObject &object,
    const utsure::core::job::EncodeResizeSettings &fallback
) {
    auto settings = fallback;
    if (object.contains("mode") && object.value("mode").isString()) {
        const auto parsed_mode = utsure::core::job::resize_mode_from_string(
            object.value("mode").toString().toStdString()
        );
        settings.mode = parsed_mode.value_or(fallback.mode);
    }
    settings.target_height = bounded_int_from_json(object.value("height"), fallback.target_height, 0, 8192);
    if (object.contains("allowUpscale")) {
        settings.allow_upscale = object.value("allowUpscale").toBool(fallback.allow_upscale);
    }
    if (settings.mode == utsure::core::job::EncodeResizeMode::target_height && settings.target_height <= 0) {
        return fallback;
    }
    return settings;
}

QJsonObject resize_settings_to_json(const utsure::core::job::EncodeResizeSettings &settings) {
    QJsonObject object;
    object.insert("mode", resize_mode_to_json(settings.mode));
    if (settings.mode == utsure::core::job::EncodeResizeMode::target_height) {
        object.insert("height", settings.target_height);
        object.insert("aspectRatio", "source");
        object.insert("allowUpscale", settings.allow_upscale);
    }
    return object;
}

QJsonObject encoding_profile_to_json(const AppSettings::EncodingProfile &profile) {
    QJsonObject object;
    object.insert("name", profile.name);
    object.insert("video", QJsonObject{
        {"codec", video_codec_to_json(profile.encode.codec)},
        {"preset", profile.encode.preset},
        {"crf", profile.encode.crf}
    });
    object.insert("audio", QJsonObject{
        {"mode", audio_mode_to_json(profile.encode.audio_mode)},
        {"bitrateKbps", profile.encode.audio_bitrate_kbps}
    });
    object.insert("resize", resize_settings_to_json(profile.resize));
    return object;
}

std::optional<AppSettings::EncodingProfile> encoding_profile_from_json(
    const QJsonObject &object,
    const AppSettings::EncodingProfile &fallback
) {
    const QString name = object.value("name").toString().trimmed();
    if (name.isEmpty()) {
        return std::nullopt;
    }

    AppSettings::EncodingProfile profile = fallback;
    profile.name = name;

    const QJsonObject video_object = object.value("video").toObject();
    profile.encode.codec = video_codec_from_json(video_object.value("codec"), fallback.encode.codec);
    profile.encode.preset = safe_preset_from_json(video_object.value("preset"), fallback.encode.preset);
    profile.encode.crf = bounded_int_from_json(video_object.value("crf"), fallback.encode.crf, 0, 51);

    const QJsonObject audio_object = object.value("audio").toObject();
    profile.encode.audio_mode = audio_mode_from_json(audio_object.value("mode"), fallback.encode.audio_mode);
    profile.encode.audio_bitrate_kbps =
        bounded_int_from_json(audio_object.value("bitrateKbps"), fallback.encode.audio_bitrate_kbps, 64, 512);

    profile.resize = resize_settings_from_json(object.value("resize").toObject(), fallback.resize);
    return profile;
}

QJsonArray encoding_profiles_to_json(const std::vector<AppSettings::EncodingProfile> &profiles) {
    QJsonArray array;
    for (const auto &profile : profiles) {
        if (profile.name.trimmed().isEmpty()) {
            continue;
        }
        array.append(encoding_profile_to_json(profile));
    }
    return array;
}

std::vector<AppSettings::EncodingProfile> encoding_profiles_from_json(const QJsonArray &array) {
    std::vector<AppSettings::EncodingProfile> profiles;
    const auto defaults = AppSettings::default_encoding_profiles();
    const auto fallback = defaults.empty() ? AppSettings::EncodingProfile{} : defaults.front();
    for (const QJsonValue &value : array) {
        if (!value.isObject()) {
            continue;
        }
        auto profile = encoding_profile_from_json(value.toObject(), fallback);
        if (!profile.has_value()) {
            continue;
        }
        const auto duplicate_name = std::find_if(
            profiles.begin(),
            profiles.end(),
            [&](const AppSettings::EncodingProfile &existing) {
                return existing.name.compare(profile->name, Qt::CaseInsensitive) == 0;
            }
        );
        if (duplicate_name == profiles.end()) {
            profiles.push_back(std::move(*profile));
        }
    }
    if (profiles.empty()) {
        return defaults;
    }
    for (const auto &default_profile : defaults) {
        if (default_profile.name != "Default" && default_profile.name != "Low Size") {
            continue;
        }
        const auto duplicate_name = std::find_if(
            profiles.begin(),
            profiles.end(),
            [&](const AppSettings::EncodingProfile &existing) {
                return existing.name.compare(default_profile.name, Qt::CaseInsensitive) == 0;
            }
        );
        if (duplicate_name == profiles.end()) {
            profiles.push_back(default_profile);
        }
    }
    return profiles;
}

QJsonObject ui_font_to_json(const AppSettings::UiFontSettings &settings) {
    QJsonObject object;
    object.insert("family", settings.family);
    object.insert("pointSize", settings.point_size);
    object.insert("useBundledMyanmarFallback", settings.use_bundled_myanmar_fallback);
    return object;
}

AppSettings::UiFontSettings ui_font_from_json(
    const QJsonObject &object,
    const AppSettings::UiFontSettings &fallback
) {
    AppSettings::UiFontSettings settings = fallback;
    if (object.contains("family") && object.value("family").isString()) {
        const QString family = object.value("family").toString().trimmed();
        settings.family = family.isEmpty() ? fallback.family : family;
    }
    settings.point_size = bounded_int_from_json(object.value("pointSize"), fallback.point_size, 6, 24);
    if (object.contains("useBundledMyanmarFallback")) {
        settings.use_bundled_myanmar_fallback =
            object.value("useBundledMyanmarFallback").toBool(fallback.use_bundled_myanmar_fallback);
    }
    return settings;
}

QJsonObject output_naming_to_json(const utsure::core::job::OutputNamingTemplate &settings) {
    QJsonObject object;
    object.insert("enabled", settings.enabled);
    object.insert("separator", QString::fromStdString(settings.separator));
    object.insert("crc32SuffixEnabled", settings.crc32_suffix_enabled);

    QJsonArray tokens;
    for (const auto &token : settings.tokens) {
        QJsonObject token_object;
        token_object.insert("type", QString::fromUtf8(utsure::core::job::OutputNaming::to_string(token.type)));
        token_object.insert("enabled", token.enabled);
        if (token.type == utsure::core::job::OutputNamingTokenType::sequence_number) {
            token_object.insert("padding", std::clamp(token.sequence_padding, 1, 8));
        }
        tokens.append(token_object);
    }
    object.insert("tokens", tokens);
    return object;
}

utsure::core::job::OutputNamingTemplate output_naming_from_json(
    const QJsonObject &object,
    const utsure::core::job::OutputNamingTemplate &fallback
) {
    auto settings = fallback;
    if (object.contains("enabled")) {
        settings.enabled = object.value("enabled").toBool(fallback.enabled);
    }
    if (object.contains("separator") && object.value("separator").isString()) {
        settings.separator = object.value("separator").toString().toStdString();
    }
    if (object.contains("crc32SuffixEnabled")) {
        settings.crc32_suffix_enabled = object.value("crc32SuffixEnabled").toBool(fallback.crc32_suffix_enabled);
    } else if (object.value("suffixes").isObject()) {
        settings.crc32_suffix_enabled =
            object.value("suffixes").toObject().value("crc32").toBool(fallback.crc32_suffix_enabled);
    }

    if (!object.value("tokens").isArray()) {
        return settings;
    }

    std::vector<utsure::core::job::OutputNamingToken> tokens;
    for (const QJsonValue &value : object.value("tokens").toArray()) {
        if (!value.isObject()) {
            continue;
        }

        const QJsonObject token_object = value.toObject();
        const auto token_type = utsure::core::job::OutputNaming::token_type_from_string(
            token_object.value("type").toString().toStdString()
        );
        if (!token_type.has_value()) {
            continue;
        }

        tokens.push_back(utsure::core::job::OutputNamingToken{
            .type = *token_type,
            .enabled = token_object.value("enabled").toBool(true),
            .sequence_padding = bounded_int_from_json(token_object.value("padding"), 2, 1, 8)
        });
    }

    if (!tokens.empty()) {
        settings.tokens = std::move(tokens);
    }
    return settings;
}

QJsonObject sequence_counters_to_json(const std::map<std::string, int> &sequence_counters) {
    QJsonObject object;
    for (const auto &[key, value] : sequence_counters) {
        if (key.empty() || value <= 0) {
            continue;
        }
        object.insert(QString::fromStdString(key), value);
    }
    return object;
}

std::map<std::string, int> sequence_counters_from_json(const QJsonObject &object) {
    std::map<std::string, int> counters;
    for (auto iterator = object.begin(); iterator != object.end(); ++iterator) {
        const QString key = iterator.key().trimmed();
        if (key.isEmpty() || !iterator.value().isDouble()) {
            continue;
        }

        const int value = iterator.value().toInt(0);
        if (value > 0) {
            counters[key.toStdString()] = value;
        }
    }
    return counters;
}

QJsonDocument settings_to_json_document(const AppSettings &settings) {
    QJsonObject root;
    root.insert("version", AppSettings::kCurrentVersion);
    root.insert("lastUsed", encode_choices_to_json(settings.last_used));
    root.insert("outputNaming", output_naming_to_json(settings.output_naming));
    root.insert("uiFont", ui_font_to_json(settings.ui_font));
    root.insert("toshiMode", QJsonObject{
        {"enabled", settings.toshi_mode_enabled}
    });
    root.insert("encodingProfiles", encoding_profiles_to_json(settings.encoding_profiles));
    root.insert("lastUsedProfile", settings.last_used_profile);
    root.insert("sequenceCounters", sequence_counters_to_json(settings.sequence_counters));
    return QJsonDocument(root);
}

QString backup_invalid_config(const QString &config_path) {
    QFileInfo info(config_path);
    const QString timestamp = QDateTime::currentDateTimeUtc().toString("yyyyMMddHHmmss");
    const QString backup_path = info.absoluteFilePath() + ".invalid-" + timestamp;
    if (QFile::rename(config_path, backup_path)) {
        return backup_path;
    }
    return {};
}

}  // namespace

QString AppSettings::default_config_file_path() {
    QString config_root = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (config_root.trimmed().isEmpty()) {
        config_root = QDir::homePath() + "/.config/utsure";
    }

    return QDir(config_root).filePath("settings.json");
}

AppSettings AppSettings::defaults() {
    AppSettings settings;
    settings.version = kCurrentVersion;
    settings.output_naming = utsure::core::job::OutputNaming::default_template();
    settings.encoding_profiles = default_encoding_profiles();
    return settings;
}

AppSettings::LoadResult AppSettings::load(const QString &config_path) {
    LoadResult result{
        .settings = defaults(),
        .config_path = config_path
    };

    QFileInfo info(config_path);
    QDir().mkpath(info.absolutePath());

    if (!info.exists()) {
        QString save_error;
        if (!result.settings.save(config_path, &save_error)) {
            result.warning = "Could not create default settings file: " + save_error;
        }
        return result;
    }

    QFile file(config_path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.warning = "Could not read settings file. Defaults are active.";
        return result;
    }

    const QByteArray settings_bytes = file.readAll();
    file.close();

    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(settings_bytes, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        result.backup_path = backup_invalid_config(config_path);
        result.warning = result.backup_path.isEmpty()
            ? "Settings JSON was invalid. Defaults are active; the invalid file was left untouched."
            : "Settings JSON was invalid. Defaults are active and the invalid file was moved aside.";
        if (!result.backup_path.isEmpty()) {
            QString save_error;
            result.settings.save(config_path, &save_error);
        }
        return result;
    }

    const auto fallback = defaults();
    const QJsonObject root = document.object();
    result.settings.version = root.value("version").toInt(kCurrentVersion);
    result.settings.last_used = encode_choices_from_json(root.value("lastUsed").toObject(), fallback.last_used);
    result.settings.output_naming =
        output_naming_from_json(root.value("outputNaming").toObject(), fallback.output_naming);
    result.settings.ui_font = ui_font_from_json(root.value("uiFont").toObject(), fallback.ui_font);
    result.settings.toshi_mode_enabled =
        root.value("toshiMode").toObject().value("enabled").toBool(fallback.toshi_mode_enabled);
    result.settings.encoding_profiles = root.value("encodingProfiles").isArray()
        ? encoding_profiles_from_json(root.value("encodingProfiles").toArray())
        : fallback.encoding_profiles;
    result.settings.last_used_profile = root.value("lastUsedProfile").toString(fallback.last_used_profile).trimmed();
    if (!result.settings.last_used_profile.isEmpty()) {
        const auto matching_profile = std::find_if(
            result.settings.encoding_profiles.begin(),
            result.settings.encoding_profiles.end(),
            [&](const AppSettings::EncodingProfile &profile) {
                return profile.name.compare(result.settings.last_used_profile, Qt::CaseInsensitive) == 0;
            }
        );
        if (matching_profile == result.settings.encoding_profiles.end()) {
            result.settings.last_used_profile.clear();
        }
    }
    result.settings.sequence_counters = sequence_counters_from_json(root.value("sequenceCounters").toObject());
    return result;
}

AppSettings::LoadResult AppSettings::load_default_location() {
    return load(default_config_file_path());
}

bool AppSettings::save(const QString &config_path, QString *error_message) const {
    QFileInfo info(config_path);
    if (!QDir().mkpath(info.absolutePath())) {
        if (error_message != nullptr) {
            *error_message = "Could not create settings directory.";
        }
        return false;
    }

    QSaveFile file(config_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error_message != nullptr) {
            *error_message = file.errorString();
        }
        return false;
    }

    const QByteArray json = settings_to_json_document(*this).toJson(QJsonDocument::Indented);
    if (file.write(json) != json.size()) {
        if (error_message != nullptr) {
            *error_message = file.errorString();
        }
        return false;
    }

    if (!file.commit()) {
        if (error_message != nullptr) {
            *error_message = file.errorString();
        }
        return false;
    }

    return true;
}

bool AppSettings::save_default_location(QString *error_message) const {
    return save(default_config_file_path(), error_message);
}

int AppSettings::sequence_counter_value(const std::string &key) const {
    const auto iterator = sequence_counters.find(key.empty() ? "default" : key);
    return iterator == sequence_counters.end() ? 0 : iterator->second;
}

void AppSettings::set_sequence_counter_value(const std::string &key, const int value) {
    if (value <= 0) {
        return;
    }

    const std::string normalized_key = key.empty() ? "default" : key;
    const int current_value = sequence_counter_value(normalized_key);
    sequence_counters[normalized_key] = std::max(current_value, value);
}

void AppSettings::remember_encode_choices(const LastUsedEncodeChoices &choices) {
    last_used = choices;
}

std::vector<AppSettings::EncodingProfile> AppSettings::default_encoding_profiles() {
    return {
        EncodingProfile{
            .name = "Default",
            .encode = LastUsedEncodeChoices{
                .codec = utsure::core::media::OutputVideoCodec::h265,
                .preset = "fast",
                .crf = 22,
                .audio_mode = utsure::core::media::AudioOutputMode::encode_aac,
                .audio_bitrate_kbps = 128
            },
            .resize = utsure::core::job::EncodeResizeSettings{}
        },
        EncodingProfile{
            .name = "Low Size",
            .encode = LastUsedEncodeChoices{
                .codec = utsure::core::media::OutputVideoCodec::h264,
                .preset = "veryslow",
                .crf = 30,
                .audio_mode = utsure::core::media::AudioOutputMode::encode_aac,
                .audio_bitrate_kbps = 128
            },
            .resize = utsure::core::job::EncodeResizeSettings{
                .mode = utsure::core::job::EncodeResizeMode::target_height,
                .target_height = 540,
                .allow_upscale = false
            }
        },
        EncodingProfile{
            .name = "H.264 1080p Compatibility",
            .encode = LastUsedEncodeChoices{
                .codec = utsure::core::media::OutputVideoCodec::h264,
                .preset = "fast",
                .crf = 22,
                .audio_mode = utsure::core::media::AudioOutputMode::encode_aac,
                .audio_bitrate_kbps = 192
            },
            .resize = utsure::core::job::EncodeResizeSettings{
                .mode = utsure::core::job::EncodeResizeMode::target_height,
                .target_height = 1080,
                .allow_upscale = false
            }
        },
        EncodingProfile{
            .name = "HEVC 1080p Quality",
            .encode = LastUsedEncodeChoices{
                .codec = utsure::core::media::OutputVideoCodec::h265,
                .preset = "slow",
                .crf = 18,
                .audio_mode = utsure::core::media::AudioOutputMode::encode_aac,
                .audio_bitrate_kbps = 192
            },
            .resize = utsure::core::job::EncodeResizeSettings{
                .mode = utsure::core::job::EncodeResizeMode::target_height,
                .target_height = 1080,
                .allow_upscale = false
            }
        },
        EncodingProfile{
            .name = "HEVC 720p Smaller",
            .encode = LastUsedEncodeChoices{
                .codec = utsure::core::media::OutputVideoCodec::h265,
                .preset = "medium",
                .crf = 21,
                .audio_mode = utsure::core::media::AudioOutputMode::encode_aac,
                .audio_bitrate_kbps = 160
            },
            .resize = utsure::core::job::EncodeResizeSettings{
                .mode = utsure::core::job::EncodeResizeMode::target_height,
                .target_height = 720,
                .allow_upscale = false
            }
        },
        EncodingProfile{
            .name = "HEVC 540p Data Saver",
            .encode = LastUsedEncodeChoices{
                .codec = utsure::core::media::OutputVideoCodec::h265,
                .preset = "medium",
                .crf = 23,
                .audio_mode = utsure::core::media::AudioOutputMode::encode_aac,
                .audio_bitrate_kbps = 128
            },
            .resize = utsure::core::job::EncodeResizeSettings{
                .mode = utsure::core::job::EncodeResizeMode::target_height,
                .target_height = 540,
                .allow_upscale = false
            }
        },
        EncodingProfile{
            .name = "HEVC 480p Small",
            .encode = LastUsedEncodeChoices{
                .codec = utsure::core::media::OutputVideoCodec::h265,
                .preset = "medium",
                .crf = 25,
                .audio_mode = utsure::core::media::AudioOutputMode::encode_aac,
                .audio_bitrate_kbps = 128
            },
            .resize = utsure::core::job::EncodeResizeSettings{
                .mode = utsure::core::job::EncodeResizeMode::target_height,
                .target_height = 480,
                .allow_upscale = false
            }
        }
    };
}
