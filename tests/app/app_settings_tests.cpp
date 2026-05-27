#include "app_settings.hpp"
#include "encode_job_duplicate.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
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

void touch_file(const std::filesystem::path &path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream << "test";
}

utsure::core::job::OutputNamingRequest make_duplicate_naming_request(
    const std::filesystem::path &source_path,
    const std::filesystem::path &output_directory,
    const std::string &selected_text,
    const std::string &extension = ".mp4"
) {
    return utsure::core::job::OutputNamingRequest{
        .source_path = source_path,
        .output_directory = output_directory,
        .custom_text = selected_text,
        .extension_hint = extension,
        .video_codec = utsure::core::media::OutputVideoCodec::h265,
        .output_width = 1920,
        .output_height = 1080
    };
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
        result.settings.last_used.crf != 22 ||
        result.settings.toshi_mode_enabled) {
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
    settings.toshi_mode_enabled = true;
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
        !loaded.settings.toshi_mode_enabled ||
        loaded.settings.sequence_counter_value("bdrip|show") != 12) {
        return fail("Settings JSON did not round-trip encode choices, naming tokens, Toshi mode, and counters.");
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
    last_used.insert("audioBitrateKbps", 999);

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
        loaded.settings.last_used.audio_bitrate_kbps != 192 ||
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

int assert_duplicate_copies_job_choices_and_stays_independent(const std::filesystem::path &root) {
    const auto source_path = root / "Duplicate" / "episode01.mkv";
    const auto output_directory = root / "DuplicateOut";
    touch_file(source_path);
    std::filesystem::create_directories(output_directory);

    DuplicateEncodeEntryState original{
        .source_path = path_to_qstring(source_path),
        .source_name = "episode01.mkv",
        .output_name_custom_text = "OP",
        .output_path = path_to_qstring(output_directory / "OP Duplicate - 01 x265 1920x1080.mp4"),
        .output_path_manual_override = false,
        .same_as_input = false,
        .subtitle_enabled = true,
        .subtitle_path = path_to_qstring(source_path.parent_path() / "episode01 OP.ass"),
        .subtitle_manual_override = true,
        .video_codec = utsure::core::media::OutputVideoCodec::h264,
        .video_preset = "slow",
        .video_crf = 18,
        .audio_mode = utsure::core::media::AudioOutputMode::copy_source,
        .audio_bitrate_kbps = 256
    };

    const auto result = duplicate_encode_entry(DuplicateEncodeEntryRequest{
        .original = original,
        .automatic_output_request = make_duplicate_naming_request(source_path, output_directory, "OP"),
        .naming_template = utsure::core::job::OutputNaming::default_template(),
        .stored_sequence_number = 1
    });

    if (result.output_path_generation_failed ||
        result.duplicate.source_path != original.source_path ||
        result.duplicate.source_name != "episode01.mkv Copy" ||
        result.duplicate.output_name_custom_text != original.output_name_custom_text ||
        result.duplicate.same_as_input != original.same_as_input ||
        result.duplicate.subtitle_enabled != original.subtitle_enabled ||
        result.duplicate.subtitle_path != original.subtitle_path ||
        !result.duplicate.subtitle_manual_override ||
        result.duplicate.video_codec != original.video_codec ||
        result.duplicate.video_preset != original.video_preset ||
        result.duplicate.video_crf != original.video_crf ||
        result.duplicate.audio_mode != original.audio_mode ||
        result.duplicate.audio_bitrate_kbps != original.audio_bitrate_kbps) {
        return fail("Duplicate did not copy source, subtitle, and encode settings.");
    }

    auto edited_duplicate = result.duplicate;
    edited_duplicate.video_crf = 30;
    edited_duplicate.subtitle_path = path_to_qstring(source_path.parent_path() / "different.ass");
    if (original.video_crf != 18 ||
        original.subtitle_path == edited_duplicate.subtitle_path) {
        return fail("Duplicate state did not remain independent from the original state.");
    }

    std::cout << "duplicate.copy=ok\n";
    return 0;
}

int assert_duplicate_auto_output_uses_exclusion_without_extra_counter_skip(const std::filesystem::path &root) {
    const auto source_path = root / "DuplicateCounter" / "episode01.mkv";
    const auto output_directory = root / "DuplicateCounterOut";
    touch_file(source_path);
    std::filesystem::create_directories(output_directory);

    const QString original_output = path_to_qstring(output_directory / "OP DuplicateCounter - 05 x265 1920x1080.mp4");
    const auto result = duplicate_encode_entry(DuplicateEncodeEntryRequest{
        .original = DuplicateEncodeEntryState{
            .source_path = path_to_qstring(source_path),
            .source_name = "episode01.mkv",
            .output_name_custom_text = "OP",
            .output_path = original_output,
            .output_path_manual_override = false
        },
        .automatic_output_request = make_duplicate_naming_request(source_path, output_directory, "OP"),
        .naming_template = utsure::core::job::OutputNaming::default_template(),
        .stored_sequence_number = 4
    });

    if (result.output_path_generation_failed ||
        result.duplicate.output_path == original_output ||
        !result.sequence_counter_reserved ||
        result.persisted_sequence_number != 6 ||
        !result.duplicate.output_path.endsWith("OP DuplicateCounter - 06 x265 1920x1080.mp4")) {
        return fail("Duplicate auto output did not reserve exactly the next safe sequence number.");
    }

    std::cout << "duplicate.auto_output=" << result.duplicate.output_path.toStdString() << '\n';
    return 0;
}

int assert_duplicate_manual_output_override_is_preserved_safely(const std::filesystem::path &root) {
    const auto source_path = root / "DuplicateManual" / "episode01.mkv";
    const auto output_directory = root / "DuplicateManualOut";
    const auto original_output_path = output_directory / "manual-output.mkv";
    touch_file(source_path);
    touch_file(original_output_path);

    const auto result = duplicate_encode_entry(DuplicateEncodeEntryRequest{
        .original = DuplicateEncodeEntryState{
            .source_path = path_to_qstring(source_path),
            .source_name = "episode01.mkv",
            .output_path = path_to_qstring(original_output_path),
            .output_path_manual_override = true,
            .subtitle_enabled = true,
            .subtitle_path = path_to_qstring(source_path.parent_path() / "manual.ass"),
            .subtitle_manual_override = true
        },
        .automatic_output_request = make_duplicate_naming_request(source_path, output_directory, "Ignored"),
        .naming_template = utsure::core::job::OutputNaming::default_template(),
        .stored_sequence_number = 20
    });

    if (result.output_path_generation_failed ||
        !result.duplicate.output_path_manual_override ||
        result.duplicate.output_path == path_to_qstring(original_output_path) ||
        !result.duplicate.output_path.endsWith("manual-output Copy.mkv") ||
        result.sequence_counter_reserved ||
        !result.duplicate.subtitle_manual_override ||
        result.duplicate.subtitle_path != path_to_qstring(source_path.parent_path() / "manual.ass")) {
        return fail("Duplicate did not preserve manual output/subtitle intent safely.");
    }

    std::cout << "duplicate.manual_output=" << result.duplicate.output_path.toStdString() << '\n';
    return 0;
}

int assert_duplicate_manual_output_never_falls_back_to_original(const std::filesystem::path &root) {
    const auto source_path = root / "DuplicateManualExhausted" / "episode01.mkv";
    const auto output_directory = root / "DuplicateManualExhaustedOut";
    const auto original_output_path = output_directory / "manual-output.mkv";
    touch_file(source_path);
    touch_file(original_output_path);
    for (int copy_index = 1; copy_index < 1000; ++copy_index) {
        const std::string suffix = copy_index == 1
            ? " Copy"
            : " Copy " + std::to_string(copy_index);
        touch_file(output_directory / ("manual-output" + suffix + ".mkv"));
    }

    const auto result = duplicate_encode_entry(DuplicateEncodeEntryRequest{
        .original = DuplicateEncodeEntryState{
            .source_path = path_to_qstring(source_path),
            .source_name = "episode01.mkv",
            .output_path = path_to_qstring(original_output_path),
            .output_path_manual_override = true
        },
        .automatic_output_request = make_duplicate_naming_request(source_path, output_directory, "Ignored"),
        .naming_template = utsure::core::job::OutputNaming::default_template(),
        .stored_sequence_number = 20
    });

    if (result.output_path_generation_failed ||
        result.duplicate.output_path.isEmpty() ||
        result.duplicate.output_path == path_to_qstring(original_output_path)) {
        return fail("Manual duplicate fallback returned the original path or failed instead of choosing a unique fallback.");
    }

    std::cout << "duplicate.manual_output_exhausted=" << result.duplicate.output_path.toStdString() << '\n';
    return 0;
}

int assert_duplicate_manual_output_uncertain_filesystem_is_not_used(const std::filesystem::path &root) {
#ifdef _WIN32
    const auto source_path = root / "DuplicateManualInvalid" / "episode01.mkv";
    touch_file(source_path);
    const auto invalid_original_output = root / "Bad<Folder>:Name?" / "manual-output.mkv";

    const auto result = duplicate_encode_entry(DuplicateEncodeEntryRequest{
        .original = DuplicateEncodeEntryState{
            .source_path = path_to_qstring(source_path),
            .source_name = "episode01.mkv",
            .output_path = path_to_qstring(invalid_original_output),
            .output_path_manual_override = true
        },
        .automatic_output_request = make_duplicate_naming_request(source_path, root / "Unused", "Ignored"),
        .naming_template = utsure::core::job::OutputNaming::default_template(),
        .stored_sequence_number = 20
    });

    if (!result.output_path_generation_failed || result.diagnostic.isEmpty()) {
        return fail("Manual duplicate output treated an unverifiable filesystem path as available.");
    }
#else
    (void)root;
#endif

    std::cout << "duplicate.manual_output_uncertain=conservative\n";
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

    if (assert_duplicate_copies_job_choices_and_stays_independent(root) != 0) {
        return 1;
    }

    if (assert_duplicate_auto_output_uses_exclusion_without_extra_counter_skip(root) != 0) {
        return 1;
    }

    if (assert_duplicate_manual_output_override_is_preserved_safely(root) != 0) {
        return 1;
    }

    if (assert_duplicate_manual_output_never_falls_back_to_original(root) != 0) {
        return 1;
    }

    if (assert_duplicate_manual_output_uncertain_filesystem_is_not_used(root) != 0) {
        return 1;
    }

    return 0;
}
