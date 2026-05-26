#include "app_settings.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <system_error>

namespace {

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

struct TempDirectoryGuard final {
    explicit TempDirectoryGuard(std::filesystem::path value) : path(std::move(value)) {}

    ~TempDirectoryGuard() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path{};
};

std::filesystem::path make_temp_directory() {
    const auto unique_suffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root =
        std::filesystem::temp_directory_path() / ("utsure-app-settings-tests-" + unique_suffix);
    std::filesystem::create_directories(root);
    return root;
}

QString path_to_qstring(const std::filesystem::path &path) {
    return QString::fromStdString(path.string());
}

QByteArray read_file_bytes(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

int assert_missing_config_loads_defaults(const std::filesystem::path &root) {
    const QString config_path = path_to_qstring(root / "missing" / "settings.json");
    const auto result = AppSettings::load(config_path);
    if (!result.warning.isEmpty()) {
        return fail("Missing settings config should load defaults without warning.");
    }

    if (!QFile::exists(config_path)) {
        return fail("Missing settings config did not create a default JSON file.");
    }

    if (result.settings.version != AppSettings::kCurrentVersion ||
        result.settings.output_naming.tokens.empty() ||
        result.settings.last_used.crf != 22) {
        return fail("Missing settings config did not return default settings.");
    }

    std::cout << "settings.missing=defaults\n";
    return 0;
}

int assert_invalid_config_falls_back_and_is_preserved(const std::filesystem::path &root) {
    const QString config_path = path_to_qstring(root / "invalid" / "settings.json");
    QDir().mkpath(QFileInfo(config_path).absolutePath());
    QFile file(config_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return fail("Could not prepare invalid settings fixture.");
    }
    file.write("{ invalid json");
    file.close();

    const auto result = AppSettings::load(config_path);
    if (result.warning.isEmpty() || result.backup_path.isEmpty() || !QFile::exists(result.backup_path)) {
        return fail("Invalid settings config did not warn and move the bad file aside.");
    }

    if (!QFile::exists(config_path) || result.settings.last_used.crf != 22) {
        return fail("Invalid settings config did not recreate defaults safely.");
    }

    std::cout << "settings.invalid=defaults\n";
    return 0;
}

int assert_encode_choices_round_trip(const std::filesystem::path &root) {
    const QString config_path = path_to_qstring(root / "roundtrip" / "settings.json");
    AppSettings settings = AppSettings::defaults();
    settings.remember_encode_choices(AppSettings::LastUsedEncodeChoices{
        .codec = utsure::core::media::OutputVideoCodec::h264,
        .preset = "slow",
        .crf = 18,
        .audio_mode = utsure::core::media::AudioOutputMode::copy_source,
        .audio_bitrate_kbps = 256
    });
    settings.output_naming.tokens = {
        utsure::core::job::OutputNamingToken{
            .type = utsure::core::job::OutputNamingTokenType::codec,
            .enabled = true
        },
        utsure::core::job::OutputNamingToken{
            .type = utsure::core::job::OutputNamingTokenType::sequence_number,
            .enabled = false,
            .sequence_padding = 2
        }
    };
    settings.set_sequence_counter_value("bdrip|show", 12);

    QString save_error;
    if (!settings.save(config_path, &save_error)) {
        return fail("Could not save round-trip settings fixture.");
    }

    const auto loaded = AppSettings::load(config_path);
    if (!loaded.warning.isEmpty() ||
        loaded.settings.last_used.codec != utsure::core::media::OutputVideoCodec::h264 ||
        loaded.settings.last_used.preset != "slow" ||
        loaded.settings.last_used.crf != 18 ||
        loaded.settings.last_used.audio_mode != utsure::core::media::AudioOutputMode::copy_source ||
        loaded.settings.last_used.audio_bitrate_kbps != 256 ||
        loaded.settings.output_naming.tokens.size() != 2U ||
        loaded.settings.output_naming.tokens[0].type != utsure::core::job::OutputNamingTokenType::codec ||
        loaded.settings.output_naming.tokens[1].enabled ||
        loaded.settings.sequence_counter_value("bdrip|show") != 12) {
        return fail("Settings JSON did not round-trip encode choices, naming tokens, and counters.");
    }

    std::cout << "settings.roundtrip=ok\n";
    return 0;
}

int assert_invalid_values_fall_back(const std::filesystem::path &root) {
    const QString config_path = path_to_qstring(root / "invalid-values" / "settings.json");
    QDir().mkpath(QFileInfo(config_path).absolutePath());

    QJsonObject last_used;
    last_used.insert("codec", "future-codec");
    last_used.insert("preset", "not-a-preset");
    last_used.insert("crf", 999);

    QJsonArray tokens;
    QJsonObject unknown_token;
    unknown_token.insert("type", "futureToken");
    unknown_token.insert("enabled", true);
    tokens.append(unknown_token);
    QJsonObject known_token;
    known_token.insert("type", "sourceFolderName");
    known_token.insert("enabled", true);
    tokens.append(known_token);

    QJsonObject output_naming;
    output_naming.insert("tokens", tokens);

    QJsonObject root_object;
    root_object.insert("version", 1);
    root_object.insert("lastUsed", last_used);
    root_object.insert("outputNaming", output_naming);

    QFile file(config_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return fail("Could not prepare invalid-value settings fixture.");
    }
    file.write(QJsonDocument(root_object).toJson(QJsonDocument::Indented));
    file.close();

    const auto loaded = AppSettings::load(config_path);
    if (loaded.settings.last_used.codec != utsure::core::media::OutputVideoCodec::h265 ||
        loaded.settings.last_used.preset != "fast" ||
        loaded.settings.last_used.crf != 22 ||
        loaded.settings.output_naming.tokens.size() != 1U ||
        loaded.settings.output_naming.tokens[0].type != utsure::core::job::OutputNamingTokenType::source_folder_name) {
        return fail("Invalid saved settings did not fall back to valid defaults safely.");
    }

    std::cout << "settings.invalid_values=defaults\n";
    return 0;
}

int assert_json_does_not_store_last_output_directory(const std::filesystem::path &root) {
    const QString config_path = path_to_qstring(root / "no-output-dir" / "settings.json");
    AppSettings settings = AppSettings::defaults();
    settings.set_sequence_counter_value("default", 3);
    QString save_error;
    if (!settings.save(config_path, &save_error)) {
        return fail("Could not save no-output-directory settings fixture.");
    }

    const QByteArray json = read_file_bytes(config_path);
    if (json.contains("lastOutput") ||
        json.contains("outputDirectory") ||
        json.contains("outputDir") ||
        json.contains("C:/") ||
        json.contains("C:\\") ||
        json.contains("/tmp/")) {
        return fail("Settings JSON persisted an output-directory-like field or path.");
    }

    std::cout << "settings.last_output_directory=absent\n";
    return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);

    const auto root = make_temp_directory();
    const TempDirectoryGuard cleanup(root);

    if (assert_missing_config_loads_defaults(root) != 0) {
        return 1;
    }

    if (assert_invalid_config_falls_back_and_is_preserved(root) != 0) {
        return 1;
    }

    if (assert_encode_choices_round_trip(root) != 0) {
        return 1;
    }

    if (assert_invalid_values_fall_back(root) != 0) {
        return 1;
    }

    if (assert_json_does_not_store_last_output_directory(root) != 0) {
        return 1;
    }

    return 0;
}
