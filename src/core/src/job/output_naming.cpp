#include "utsure/core/job/output_naming.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <set>
#include <string_view>
#include <system_error>

namespace utsure::core::job {

namespace {

std::string lowercase_ascii(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        }
    );
    return value;
}

std::string uppercase_ascii(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::toupper(character));
        }
    );
    return value;
}

bool is_invalid_filename_character(const unsigned char character) {
    return character < 32 || character == '<' || character == '>' || character == ':' || character == '"' ||
        character == '/' || character == '\\' || character == '|' || character == '?' || character == '*';
}

std::string trim_ascii_whitespace(std::string value) {
    const auto not_space = [](const unsigned char character) {
        return !std::isspace(character);
    };

    const auto begin = std::find_if(value.begin(), value.end(), not_space);
    if (begin == value.end()) {
        return {};
    }

    const auto end = std::find_if(value.rbegin(), value.rend(), not_space).base();
    return std::string(begin, end);
}

std::string sanitize_filename_fragment(const std::string_view text) {
    std::string sanitized{};
    sanitized.reserve(text.size());

    bool previous_was_space = false;
    for (const unsigned char character : text) {
        if (is_invalid_filename_character(character) || std::iscntrl(character)) {
            if (!sanitized.empty() && !previous_was_space) {
                sanitized.push_back(' ');
                previous_was_space = true;
            }
            continue;
        }

        if (std::isspace(character)) {
            if (!sanitized.empty() && !previous_was_space) {
                sanitized.push_back(' ');
                previous_was_space = true;
            }
            continue;
        }

        sanitized.push_back(static_cast<char>(character));
        previous_was_space = false;
    }

    return trim_ascii_whitespace(std::move(sanitized));
}

std::string normalize_extension(std::string extension) {
    extension = trim_ascii_whitespace(lowercase_ascii(std::move(extension)));
    if (extension.empty()) {
        return ".mp4";
    }

    if (!extension.starts_with('.')) {
        extension.insert(extension.begin(), '.');
    }

    return extension;
}

std::string resolve_source_folder_name(const std::filesystem::path &source_path) {
    const auto folder_name = sanitize_filename_fragment(source_path.parent_path().filename().string());
    if (!folder_name.empty()) {
        return folder_name;
    }

    const auto source_stem = sanitize_filename_fragment(source_path.stem().string());
    if (!source_stem.empty()) {
        return source_stem;
    }

    const auto source_name = sanitize_filename_fragment(source_path.filename().string());
    if (!source_name.empty()) {
        return source_name;
    }

    return "Output";
}

std::string resolve_video_codec_tag(const media::OutputVideoCodec codec) {
    switch (codec) {
    case media::OutputVideoCodec::h264:
        return "x264";
    case media::OutputVideoCodec::h265:
        return "x265";
    default:
        return uppercase_ascii(std::string(media::to_string(codec)));
    }
}

std::string resolve_audio_codec_tag(const OutputNamingRequest &request) {
    if (request.audio_settings.mode == media::AudioOutputMode::disable) {
        return "NoAudio";
    }

    if (request.source_audio_known && !request.source_audio_stream.has_value()) {
        return "NoAudio";
    }

    if (request.audio_settings.mode == media::AudioOutputMode::copy_source) {
        if (!request.source_audio_stream.has_value()) {
            return "Source";
        }

        const auto copied_codec = sanitize_filename_fragment(request.source_audio_stream->codec_name);
        return copied_codec.empty() ? "Source" : uppercase_ascii(copied_codec);
    }

    const auto encoded_codec = sanitize_filename_fragment(std::string(media::to_string(request.audio_settings.codec)));
    return encoded_codec.empty() ? "AAC" : uppercase_ascii(encoded_codec);
}

bool is_positive_integer(const std::string_view text) {
    return !text.empty() &&
        std::all_of(text.begin(), text.end(), [](const unsigned char character) { return std::isdigit(character); });
}

std::set<int> collect_used_sequence_numbers(
    const std::filesystem::path &directory,
    const std::string_view prefix,
    const std::string_view suffix
) {
    std::set<int> sequence_numbers{};
    if (directory.empty()) {
        return sequence_numbers;
    }

    std::error_code status_error{};
    if (!std::filesystem::exists(directory, status_error) || status_error ||
        !std::filesystem::is_directory(directory, status_error) || status_error) {
        return sequence_numbers;
    }

    std::error_code iteration_error{};
    for (std::filesystem::directory_iterator iterator(directory, iteration_error), end; iterator != end; iterator.increment(iteration_error)) {
        if (iteration_error) {
            break;
        }

        std::error_code entry_error{};
        if (!iterator->is_regular_file(entry_error) || entry_error) {
            continue;
        }

        const std::string file_name = iterator->path().filename().string();
        if (file_name.size() <= prefix.size() + suffix.size() ||
            !file_name.starts_with(prefix) ||
            !file_name.ends_with(suffix)) {
            continue;
        }

        const std::string_view number_text(
            file_name.data() + static_cast<std::ptrdiff_t>(prefix.size()),
            file_name.size() - prefix.size() - suffix.size()
        );
        if (!is_positive_integer(number_text)) {
            continue;
        }

        const int sequence_number = std::stoi(std::string(number_text));
        if (sequence_number > 0) {
            sequence_numbers.insert(sequence_number);
        }
    }

    return sequence_numbers;
}

int next_available_sequence_number(const std::set<int> &used_sequence_numbers) {
    int sequence_number = 1;
    while (used_sequence_numbers.contains(sequence_number)) {
        ++sequence_number;
    }

    return sequence_number;
}

std::string format_sequence_number(const int sequence_number) {
    std::ostringstream stream;
    stream << std::setw(2) << std::setfill('0') << sequence_number;
    return stream.str();
}

}  // namespace

OutputNamingResult OutputNaming::suggest(const OutputNamingRequest &request) {
    const std::string custom_text = sanitize_filename_fragment(request.custom_text);
    const std::string source_folder_name = resolve_source_folder_name(request.source_path);
    const std::string video_codec_tag = resolve_video_codec_tag(request.video_codec);
    const std::string audio_codec_tag = resolve_audio_codec_tag(request);
    const std::string extension = normalize_extension(request.extension_hint);

    std::string stem_prefix{};
    if (!custom_text.empty()) {
        stem_prefix += '[';
        stem_prefix += custom_text;
        stem_prefix += "] ";
    }
    stem_prefix += source_folder_name + " - ";

    const std::string stem_suffix = " [" + video_codec_tag + "] [" + audio_codec_tag + "]";
    const std::string full_suffix = stem_suffix + extension;

    const auto used_sequence_numbers =
        collect_used_sequence_numbers(request.output_directory, stem_prefix, full_suffix);
    const int sequence_number = next_available_sequence_number(used_sequence_numbers);
    const std::string rendered_sequence_number = format_sequence_number(sequence_number);
    const std::string file_name =
        stem_prefix + rendered_sequence_number + stem_suffix + extension;
    const std::filesystem::path output_path = request.output_directory.empty()
        ? std::filesystem::path(file_name)
        : (request.output_directory / file_name).lexically_normal();

    return OutputNamingResult{
        .output_path = output_path,
        .file_name = file_name,
        .source_folder_name = source_folder_name,
        .custom_text = custom_text,
        .video_codec_tag = video_codec_tag,
        .audio_codec_tag = audio_codec_tag,
        .extension = extension,
        .sequence_number = sequence_number
    };
}

}  // namespace utsure::core::job
