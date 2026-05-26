#include "utsure/core/job/output_naming.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using utsure::core::job::OutputNaming;
using utsure::core::job::OutputNamingRequest;
using utsure::core::job::OutputNamingReservationRequest;
using utsure::core::job::OutputNamingTemplate;
using utsure::core::job::OutputNamingToken;
using utsure::core::job::OutputNamingTokenType;
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
        },
        .output_width = 1920,
        .output_height = 1080
    });

    const auto expected_name = std::string("BDRip My Show - 01 x265 1920x1080.mp4");
    if (result.file_name != expected_name) {
        std::cerr << "Expected name:\n" << expected_name << "\n";
        std::cerr << "Actual name:\n" << result.file_name << "\n";
        return 1;
    }

    if (result.output_path != (output_directory / expected_name).lexically_normal()) {
        return fail("The generated default output path did not use the requested output directory.");
    }

    if (result.sequence_number != 1 || result.video_codec_tag != "x265" || result.audio_codec_tag != "AAC" ||
        result.resolution_tag != "1920x1080" || result.extension != ".mp4" ||
        result.source_folder_name != "My Show") {
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
    touch_file(output_directory / "BDRip Anime - 01 x265 1280x720.mkv");
    touch_file(output_directory / "BDRip Anime - 03 x265 1280x720.mkv");
    touch_file(output_directory / "BDRip Anime - x x265 1280x720.mkv");
    touch_file(output_directory / "Anime - 2 x265 1280x720.mkv");

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
        },
        .output_width = 1280,
        .output_height = 720
    });

    if (result.sequence_number != 4 || result.file_name != "BDRip Anime - 04 x265 1280x720.mkv") {
        return fail("The output naming helper did not choose the next sequence above the existing maximum.");
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
        },
        .output_width = 640,
        .output_height = 360
    });

    if (copied_audio.file_name != "Concert - 01 x264 640x360.mkv") {
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
        .source_audio_stream = std::nullopt,
        .output_width = 640,
        .output_height = 360
    });

    if (silent_source.file_name != "Silent Concert - 01 x264 640x360.mp4") {
        return fail("The output naming helper did not switch the audio tag to NoAudio for a known silent source.");
    }

    std::cout << "copy.name=" << copied_audio.file_name << '\n';
    std::cout << "silent.name=" << silent_source.file_name << '\n';
    return 0;
}

int assert_batch_reservation_avoids_parallel_collisions(const std::filesystem::path &root) {
    const auto source_directory = root / "Series";
    const auto output_directory = root / "parallel";

    std::filesystem::create_directories(source_directory);
    std::filesystem::create_directories(output_directory);
    touch_file(source_directory / "episode-01.mkv");
    touch_file(source_directory / "episode-02.mkv");
    touch_file(source_directory / "episode-03.mkv");
    touch_file(output_directory / "BDRip Series - 01 x265 1920x1080.mkv");
    touch_file(output_directory / "BDRip Series - 03 x265 1920x1080.mkv");

    const std::vector<OutputNamingRequest> requests{
        OutputNamingRequest{
            .source_path = source_directory / "episode-01.mkv",
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
            },
            .output_width = 1920,
            .output_height = 1080
        },
        OutputNamingRequest{
            .source_path = source_directory / "episode-02.mkv",
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
            },
            .output_width = 1920,
            .output_height = 1080
        },
        OutputNamingRequest{
            .source_path = source_directory / "episode-03.mkv",
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
            },
            .output_width = 1920,
            .output_height = 1080
        }
    };

    const auto results = OutputNaming::reserve_batch(requests);
    if (results.size() != requests.size()) {
        return fail("The batch reservation helper did not return one result per request.");
    }

    if (results[0].file_name != "BDRip Series - 04 x265 1920x1080.mkv" ||
        results[1].file_name != "BDRip Series - 05 x265 1920x1080.mkv" ||
        results[2].file_name != "BDRip Series - 06 x265 1920x1080.mkv") {
        return fail("The batch reservation helper did not reserve unique sequence numbers across the batch.");
    }

    std::cout << "batch.0=" << results[0].file_name << '\n';
    std::cout << "batch.1=" << results[1].file_name << '\n';
    std::cout << "batch.2=" << results[2].file_name << '\n';
    return 0;
}

int assert_token_order_and_disabled_tokens(const std::filesystem::path &root) {
    const auto output_directory = root / "tokens";
    const auto source_path = root / "Token Series" / "episode-01.mkv";
    std::filesystem::create_directories(output_directory);
    std::filesystem::create_directories(source_path.parent_path());
    touch_file(source_path);

    const OutputNamingTemplate custom_template{
        .enabled = true,
        .separator = " - ",
        .tokens = {
            OutputNamingToken{.type = OutputNamingTokenType::codec, .enabled = true},
            OutputNamingToken{.type = OutputNamingTokenType::source_folder_name, .enabled = true},
            OutputNamingToken{.type = OutputNamingTokenType::sequence_number, .enabled = true, .sequence_padding = 3},
            OutputNamingToken{.type = OutputNamingTokenType::selected_text, .enabled = false},
            OutputNamingToken{.type = OutputNamingTokenType::resolution, .enabled = true}
        }
    };

    const auto result = OutputNaming::suggest(
        OutputNamingRequest{
            .source_path = source_path,
            .output_directory = output_directory,
            .custom_text = "Ignored",
            .extension_hint = ".mp4",
            .video_codec = OutputVideoCodec::h264,
            .output_width = 320,
            .output_height = 180
        },
        custom_template
    );

    if (result.file_name != "x264 Token Series - 001 320x180.mp4") {
        return fail("Token order or disabled-token output did not match the custom naming template.");
    }

    std::cout << "tokens.name=" << result.file_name << '\n';
    return 0;
}

int assert_filename_sanitization(const std::filesystem::path &root) {
    const auto output_directory = root / "sanitize";
    const auto source_path = root / "Bad<Folder>:Name?" / "episode-01.mkv";
    std::filesystem::create_directories(output_directory);

    const auto result = OutputNaming::suggest(OutputNamingRequest{
        .source_path = source_path,
        .output_directory = output_directory,
        .custom_text = "BD/Rip:*Name",
        .extension_hint = "M<P4",
        .video_codec = OutputVideoCodec::h265,
        .output_width = 640,
        .output_height = 360
    });

    if (result.file_name.find('<') != std::string::npos ||
        result.file_name.find('>') != std::string::npos ||
        result.file_name.find(':') != std::string::npos ||
        result.file_name.find('"') != std::string::npos ||
        result.file_name.find('/') != std::string::npos ||
        result.file_name.find('\\') != std::string::npos ||
        result.file_name.find('|') != std::string::npos ||
        result.file_name.find('?') != std::string::npos ||
        result.file_name.find('*') != std::string::npos) {
        return fail("The generated output filename still contains a Windows-invalid character.");
    }

    if (result.file_name != "BD Rip Name Bad Folder Name - 01 x265 640x360.mp4") {
        return fail("Filename sanitization did not produce the expected readable filename.");
    }

    std::cout << "sanitize.name=" << result.file_name << '\n';
    return 0;
}

int assert_stored_counter_is_authoritative_when_files_are_deleted(const std::filesystem::path &root) {
    const auto output_directory = root / "stored-counter";
    const auto source_path = root / "Stored" / "episode-01.mkv";
    std::filesystem::create_directories(output_directory);
    std::filesystem::create_directories(source_path.parent_path());
    touch_file(source_path);

    const auto request = OutputNamingRequest{
        .source_path = source_path,
        .output_directory = output_directory,
        .custom_text = "BDRip",
        .extension_hint = ".mp4",
        .video_codec = OutputVideoCodec::h265,
        .output_width = 1920,
        .output_height = 1080
    };

    const auto result = OutputNaming::reserve_next(request, OutputNaming::default_template(), 12);
    if (result.assigned_sequence_number != 13 ||
        result.persisted_sequence_number != 13 ||
        result.result.file_name != "BDRip Stored - 13 x265 1920x1080.mp4") {
        return fail("The stored sequence counter was not used when no old output files were present.");
    }

    std::cout << "stored_counter.name=" << result.result.file_name << '\n';
    return 0;
}

int assert_sequence_uses_max_of_existing_and_stored_counter(const std::filesystem::path &root) {
    const auto output_directory = root / "max-counter";
    const auto source_path = root / "Max" / "episode-01.mkv";
    std::filesystem::create_directories(output_directory);
    std::filesystem::create_directories(source_path.parent_path());
    touch_file(source_path);
    touch_file(output_directory / "BDRip Max - 08 x265 1920x1080.mp4");

    const auto request = OutputNamingRequest{
        .source_path = source_path,
        .output_directory = output_directory,
        .custom_text = "BDRip",
        .extension_hint = ".mp4",
        .video_codec = OutputVideoCodec::h265,
        .output_width = 1920,
        .output_height = 1080
    };

    const auto stored_wins = OutputNaming::reserve_next(request, OutputNaming::default_template(), 12);
    if (stored_wins.assigned_sequence_number != 13) {
        return fail("The stored sequence counter did not win over lower existing files.");
    }

    touch_file(output_directory / "BDRip Max - 20 x265 1920x1080.mp4");
    const auto existing_wins = OutputNaming::reserve_next(request, OutputNaming::default_template(), 12);
    if (existing_wins.assigned_sequence_number != 21) {
        return fail("Existing output files did not win over a lower stored sequence counter.");
    }

    std::cout << "max_counter.stored_wins=" << stored_wins.result.file_name << '\n';
    std::cout << "max_counter.existing_wins=" << existing_wins.result.file_name << '\n';
    return 0;
}

int assert_batch_reservation_advances_persisted_counters(const std::filesystem::path &root) {
    const auto output_directory = root / "persisted-batch";
    const auto source_directory = root / "Persisted";
    std::filesystem::create_directories(output_directory);
    std::filesystem::create_directories(source_directory);
    touch_file(source_directory / "episode-01.mkv");
    touch_file(source_directory / "episode-02.mkv");

    const OutputNamingRequest request{
        .source_path = source_directory / "episode-01.mkv",
        .output_directory = output_directory,
        .custom_text = "BDRip",
        .extension_hint = ".mp4",
        .video_codec = OutputVideoCodec::h265,
        .output_width = 1920,
        .output_height = 1080
    };
    auto second_request = request;
    second_request.source_path = source_directory / "episode-02.mkv";

    const auto reservations = OutputNaming::reserve_batch(std::vector<OutputNamingReservationRequest>{
        OutputNamingReservationRequest{
            .request = request,
            .naming_template = OutputNaming::default_template(),
            .stored_sequence_number = 4
        },
        OutputNamingReservationRequest{
            .request = second_request,
            .naming_template = OutputNaming::default_template(),
            .stored_sequence_number = 4
        }
    });

    if (reservations.size() != 2U ||
        reservations[0].assigned_sequence_number != 5 ||
        reservations[1].assigned_sequence_number != 6 ||
        reservations[1].persisted_sequence_number != 6 ||
        reservations[0].sequence_counter_key != reservations[1].sequence_counter_key) {
        return fail("Persisted batch reservation counters did not advance deterministically.");
    }

    std::cout << "persisted_batch.0=" << reservations[0].result.file_name << '\n';
    std::cout << "persisted_batch.1=" << reservations[1].result.file_name << '\n';
    return 0;
}

int assert_batch_counter_key_prevents_reuse_across_filename_patterns(const std::filesystem::path &root) {
    const auto output_directory = root / "mixed-patterns";
    const auto source_directory = root / "Mixed";
    std::filesystem::create_directories(output_directory);
    std::filesystem::create_directories(source_directory);
    touch_file(source_directory / "episode-01.mkv");
    touch_file(source_directory / "episode-02.mkv");

    const OutputNamingRequest first_request{
        .source_path = source_directory / "episode-01.mkv",
        .output_directory = output_directory,
        .custom_text = "BDRip",
        .extension_hint = ".mp4",
        .video_codec = OutputVideoCodec::h265,
        .output_width = 1920,
        .output_height = 1080
    };
    auto second_request = first_request;
    second_request.source_path = source_directory / "episode-02.mkv";
    second_request.output_width = 1280;
    second_request.output_height = 720;

    const auto reservations = OutputNaming::reserve_batch(std::vector<OutputNamingReservationRequest>{
        OutputNamingReservationRequest{
            .request = first_request,
            .naming_template = OutputNaming::default_template(),
            .stored_sequence_number = 4
        },
        OutputNamingReservationRequest{
            .request = second_request,
            .naming_template = OutputNaming::default_template(),
            .stored_sequence_number = 4
        }
    });

    if (reservations.size() != 2U ||
        reservations[0].assigned_sequence_number != 5 ||
        reservations[1].assigned_sequence_number != 6 ||
        reservations[1].persisted_sequence_number != 6 ||
        reservations[0].sequence_counter_key != reservations[1].sequence_counter_key ||
        reservations[0].result.file_name != "BDRip Mixed - 05 x265 1920x1080.mp4" ||
        reservations[1].result.file_name != "BDRip Mixed - 06 x265 1280x720.mp4") {
        return fail("A single persisted counter key reused a number across different filename patterns.");
    }

    std::cout << "mixed_pattern_batch.0=" << reservations[0].result.file_name << '\n';
    std::cout << "mixed_pattern_batch.1=" << reservations[1].result.file_name << '\n';
    return 0;
}

int assert_duplicate_exclusion_does_not_drive_counter_high_water_mark(const std::filesystem::path &root) {
    const auto output_directory = root / "duplicate-exclusion";
    const auto source_path = root / "Excluded" / "episode-01.mkv";
    std::filesystem::create_directories(output_directory);
    std::filesystem::create_directories(source_path.parent_path());
    touch_file(source_path);

    const OutputNamingRequest request{
        .source_path = source_path,
        .output_directory = output_directory,
        .custom_text = "BDRip",
        .extension_hint = ".mp4",
        .video_codec = OutputVideoCodec::h265,
        .output_width = 1920,
        .output_height = 1080
    };

    const auto reservations = OutputNaming::reserve_batch(std::vector<OutputNamingReservationRequest>{
        OutputNamingReservationRequest{
            .request = request,
            .naming_template = OutputNaming::default_template(),
            .stored_sequence_number = 4,
            .excluded_output_paths = {
                output_directory / "BDRip Excluded - 99 x265 1920x1080.mp4"
            }
        }
    });

    if (reservations.size() != 1U ||
        reservations[0].assigned_sequence_number != 5 ||
        reservations[0].persisted_sequence_number != 5 ||
        reservations[0].result.file_name != "BDRip Excluded - 05 x265 1920x1080.mp4") {
        return fail("A duplicate exclusion incorrectly drove the stored counter high-water mark.");
    }

    std::cout << "duplicate_exclusion.name=" << reservations[0].result.file_name << '\n';
    return 0;
}

int assert_duplicate_exclusion_blocks_current_candidate(const std::filesystem::path &root) {
    const auto output_directory = root / "duplicate-current";
    const auto source_path = root / "Current" / "episode-01.mkv";
    std::filesystem::create_directories(output_directory);
    std::filesystem::create_directories(source_path.parent_path());
    touch_file(source_path);

    const OutputNamingRequest request{
        .source_path = source_path,
        .output_directory = output_directory,
        .custom_text = "BDRip",
        .extension_hint = ".mp4",
        .video_codec = OutputVideoCodec::h265,
        .output_width = 1920,
        .output_height = 1080
    };

    const auto reservations = OutputNaming::reserve_batch(std::vector<OutputNamingReservationRequest>{
        OutputNamingReservationRequest{
            .request = request,
            .naming_template = OutputNaming::default_template(),
            .stored_sequence_number = 4,
            .excluded_output_paths = {
                output_directory / "BDRip Current - 05 x265 1920x1080.mp4"
            }
        }
    });

    if (reservations.size() != 1U ||
        reservations[0].assigned_sequence_number != 6 ||
        reservations[0].persisted_sequence_number != 6 ||
        reservations[0].result.file_name != "BDRip Current - 06 x265 1920x1080.mp4") {
        return fail("A duplicate exclusion did not block reuse of the original output path.");
    }

    std::cout << "duplicate_current.name=" << reservations[0].result.file_name << '\n';
    return 0;
}

int assert_non_sequence_template_uses_copy_suffix_for_excluded_path(const std::filesystem::path &root) {
    const auto output_directory = root / "duplicate-no-sequence";
    const auto source_path = root / "NoSequence" / "episode-01.mkv";
    std::filesystem::create_directories(output_directory);
    std::filesystem::create_directories(source_path.parent_path());
    touch_file(source_path);

    const OutputNamingTemplate no_sequence_template{
        .enabled = true,
        .separator = " - ",
        .tokens = {
            OutputNamingToken{.type = OutputNamingTokenType::selected_text, .enabled = true},
            OutputNamingToken{.type = OutputNamingTokenType::source_folder_name, .enabled = true}
        }
    };
    const OutputNamingRequest request{
        .source_path = source_path,
        .output_directory = output_directory,
        .custom_text = "BDRip",
        .extension_hint = ".mp4",
        .video_codec = OutputVideoCodec::h265
    };

    const auto reservations = OutputNaming::reserve_batch(std::vector<OutputNamingReservationRequest>{
        OutputNamingReservationRequest{
            .request = request,
            .naming_template = no_sequence_template,
            .stored_sequence_number = 12,
            .excluded_output_paths = {
                output_directory / "BDRip NoSequence.mp4"
            }
        }
    });

    if (reservations.size() != 1U ||
        reservations[0].assigned_sequence_number != 0 ||
        reservations[0].persisted_sequence_number != 0 ||
        reservations[0].result.file_name != "BDRip NoSequence Copy.mp4") {
        return fail("A no-sequence duplicate did not use a copy suffix to avoid path reuse.");
    }

    std::cout << "duplicate_no_sequence.name=" << reservations[0].result.file_name << '\n';
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

    if (assert_batch_reservation_avoids_parallel_collisions(root) != 0) {
        return 1;
    }

    if (assert_token_order_and_disabled_tokens(root) != 0) {
        return 1;
    }

    if (assert_filename_sanitization(root) != 0) {
        return 1;
    }

    if (assert_stored_counter_is_authoritative_when_files_are_deleted(root) != 0) {
        return 1;
    }

    if (assert_sequence_uses_max_of_existing_and_stored_counter(root) != 0) {
        return 1;
    }

    if (assert_batch_reservation_advances_persisted_counters(root) != 0) {
        return 1;
    }

    if (assert_batch_counter_key_prevents_reuse_across_filename_patterns(root) != 0) {
        return 1;
    }

    if (assert_duplicate_exclusion_does_not_drive_counter_high_water_mark(root) != 0) {
        return 1;
    }

    if (assert_duplicate_exclusion_blocks_current_candidate(root) != 0) {
        return 1;
    }

    if (assert_non_sequence_template_uses_copy_suffix_for_excluded_path(root) != 0) {
        return 1;
    }

    return 0;
}
