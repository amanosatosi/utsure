#include "utsure/core/job/output_naming.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>

namespace {

using utsure::core::job::OutputNaming;
using utsure::core::job::OutputNamingRequest;
using utsure::core::job::OutputNamingResult;
using utsure::core::media::AudioEncodeSettings;
using utsure::core::media::AudioOutputMode;
using utsure::core::media::AudioStreamInfo;
using utsure::core::media::OutputAudioCodec;
using utsure::core::media::OutputVideoCodec;

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
        std::filesystem::temp_directory_path() / ("utsure-output-naming-tests-" + unique_suffix);
    std::filesystem::create_directories(root);
    return root;
}

void touch_file(const std::filesystem::path &path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream << "test";
}

int assert_default_name_generation(const std::filesystem::path &root) {
    const auto source_directory = root / "My Show";
    const auto output_directory = root / "encodes";
    const auto source_path = source_directory / "episode-01.mkv";

    std::filesystem::create_directories(source_directory);
    std::filesystem::create_directories(output_directory);
    touch_file(source_path);

    const OutputNamingResult result = OutputNaming::suggest(OutputNamingRequest{
        .source_path = source_path,
        .output_directory = output_directory,
        .custom_text = "BDRip",
        .extension_hint = {},
        .video_codec = OutputVideoCodec::h265,
        .audio_settings = AudioEncodeSettings{
            .mode = AudioOutputMode::encode_aac,
            .codec = OutputAudioCodec::aac
        },
        .source_audio_known = true,
        .source_audio_stream = AudioStreamInfo{
            .codec_name = "aac"
        }
    });

    const auto expected_name = std::string("[BDRip] My Show - 01 [x265] [AAC].mp4");
    if (result.file_name != expected_name) {
        std::cerr << "Expected name:\n" << expected_name << "\n";
        std::cerr << "Actual name:\n" << result.file_name << "\n";
        return 1;
    }

    if (result.output_path != (output_directory / expected_name).lexically_normal()) {
        return fail("The generated default output path did not use the requested output directory.");
    }

    if (result.sequence_number != 1 || result.video_codec_tag != "x265" || result.audio_codec_tag != "AAC" ||
        result.extension != ".mp4" || result.source_folder_name != "My Show") {
        return fail("The generated default naming metadata did not match the expected values.");
    }

    std::cout << "default.name=" << result.file_name << '\n';
    return 0;
}

int assert_numbering_skips_only_matching_files(const std::filesystem::path &root) {
    const auto source_directory = root / "Anime";
    const auto output_directory = root / "batch";
    const auto source_path = source_directory / "episode-02.mp4";

    std::filesystem::create_directories(source_directory);
    std::filesystem::create_directories(output_directory);
    touch_file(source_path);
    touch_file(output_directory / "[BDRip] Anime - 01 [x265] [AAC].mkv");
    touch_file(output_directory / "[BDRip] Anime - 03 [x265] [AAC].mkv");
    touch_file(output_directory / "[BDRip] Anime - x [x265] [AAC].mkv");
    touch_file(output_directory / "Anime - 2 [x265] [AAC].mkv");

    const OutputNamingResult result = OutputNaming::suggest(OutputNamingRequest{
        .source_path = source_path,
        .output_directory = output_directory,
        .custom_text = "BDRip",
        .extension_hint = ".mkv",
        .video_codec = OutputVideoCodec::h265,
        .audio_settings = AudioEncodeSettings{
            .mode = AudioOutputMode::encode_aac,
            .codec = OutputAudioCodec::aac
        },
        .source_audio_known = true,
        .source_audio_stream = AudioStreamInfo{
            .codec_name = "aac"
        }
    });

    if (result.sequence_number != 2 || result.file_name != "[BDRip] Anime - 02 [x265] [AAC].mkv") {
        return fail("The output naming helper did not choose the next available sequence number.");
    }

    std::cout << "numbering.name=" << result.file_name << '\n';
    return 0;
}

int assert_codec_tags_follow_selected_settings(const std::filesystem::path &root) {
    const auto output_directory = root / "music";
    const auto source_path = root / "Concert" / "track-01.mp4";

    std::filesystem::create_directories(output_directory);
    std::filesystem::create_directories(source_path.parent_path());
    touch_file(source_path);

    const OutputNamingResult copied_audio = OutputNaming::suggest(OutputNamingRequest{
        .source_path = source_path,
        .output_directory = output_directory,
        .custom_text = {},
        .extension_hint = "MKV",
        .video_codec = OutputVideoCodec::h264,
        .audio_settings = AudioEncodeSettings{
            .mode = AudioOutputMode::copy_source,
            .codec = OutputAudioCodec::aac
        },
        .source_audio_known = true,
        .source_audio_stream = AudioStreamInfo{
            .codec_name = "opus"
        }
    });

    if (copied_audio.file_name != "Concert - 01 [x264] [OPUS].mkv") {
        return fail("The copy-source audio tag or normalized extension did not match the inspected source codec.");
    }

    const OutputNamingResult silent_source = OutputNaming::suggest(OutputNamingRequest{
        .source_path = source_path,
        .output_directory = output_directory,
        .custom_text = "Silent",
        .extension_hint = ".mp4",
        .video_codec = OutputVideoCodec::h264,
        .audio_settings = AudioEncodeSettings{
            .mode = AudioOutputMode::encode_aac,
            .codec = OutputAudioCodec::aac
        },
        .source_audio_known = true,
        .source_audio_stream = std::nullopt
    });

    if (silent_source.file_name != "[Silent] Concert - 01 [x264] [NoAudio].mp4") {
        return fail("The output naming helper did not switch the audio tag to NoAudio for a known silent source.");
    }

    std::cout << "copy.name=" << copied_audio.file_name << '\n';
    std::cout << "silent.name=" << silent_source.file_name << '\n';
    return 0;
}

}  // namespace

int main() {
    const auto root = make_temp_directory();
    const TempDirectoryGuard cleanup(root);

    if (assert_default_name_generation(root) != 0) {
        return 1;
    }

    if (assert_numbering_skips_only_matching_files(root) != 0) {
        return 1;
    }

    if (assert_codec_tags_follow_selected_settings(root) != 0) {
        return 1;
    }

    return 0;
}
