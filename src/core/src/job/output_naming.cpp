#include "utsure/core/job/output_naming.hpp"

#include "utsure/core/filesystem/path_format.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <set>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

namespace utsure::core::job {

namespace {

constexpr std::string_view kSequenceMarker = "\x1FUTSURE_SEQUENCE\x1F";
constexpr std::uint32_t kCrc32Polynomial = 0xEDB88320U;

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

std::uint32_t update_crc32(const std::uint32_t current_crc, const unsigned char byte) noexcept {
    std::uint32_t crc = current_crc ^ byte;
    for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 1U) != 0U
            ? (crc >> 1U) ^ kCrc32Polynomial
            : (crc >> 1U);
    }
    return crc;
}

std::string format_crc32_hex(const std::uint32_t crc) {
    std::ostringstream stream;
    stream << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << crc;
    return stream.str();
}

std::filesystem::path path_from_utf8_string(const std::string_view value) {
#if defined(_WIN32)
    std::u8string utf8{};
    utf8.reserve(value.size());
    for (const unsigned char character : value) {
        utf8.push_back(static_cast<char8_t>(character));
    }
    return std::filesystem::path{utf8};
#else
    return std::filesystem::path{std::string{value}};
#endif
}

bool is_hex_digit_ascii(const char character) noexcept {
    return (character >= '0' && character <= '9') ||
        (character >= 'a' && character <= 'f') ||
        (character >= 'A' && character <= 'F');
}

bool has_trailing_crc32_tag(const std::string_view stem) noexcept {
    if (stem.size() < 10U || stem[stem.size() - 10U] != '[' || stem.back() != ']') {
        return false;
    }

    const auto hex_start = stem.size() - 9U;
    for (std::size_t index = 0; index < 8U; ++index) {
        if (!is_hex_digit_ascii(stem[hex_start + index])) {
            return false;
        }
    }
    return true;
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

std::string remove_trailing_crc32_tag(std::string stem) {
    if (has_trailing_crc32_tag(stem)) {
        stem.resize(stem.size() - 10U);
        stem = trim_ascii_whitespace(std::move(stem));
    }
    return stem;
}

std::filesystem::path build_crc32_suffix_path(
    const std::filesystem::path &path,
    std::string stem,
    const std::string_view crc32_hex
) {
    const std::string normalized_crc = uppercase_ascii(std::string(crc32_hex));
    std::filesystem::path renamed = path;
    renamed.replace_filename(path_from_utf8_string(
        std::move(stem) + " [" + normalized_crc + "]" +
            filesystem::path_component_to_utf8_string(path.extension())
    ));
    return renamed.lexically_normal();
}

std::string collapse_ascii_spaces(std::string value) {
    std::string collapsed{};
    collapsed.reserve(value.size());

    bool previous_was_space = false;
    for (const unsigned char character : value) {
        if (std::isspace(character)) {
            if (!collapsed.empty() && !previous_was_space) {
                collapsed.push_back(' ');
                previous_was_space = true;
            }
            continue;
        }

        collapsed.push_back(static_cast<char>(character));
        previous_was_space = false;
    }

    return trim_ascii_whitespace(std::move(collapsed));
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

std::string sanitize_separator(std::string separator) {
    separator = collapse_ascii_spaces(std::move(separator));
    if (separator.empty()) {
        return " - ";
    }

    std::string sanitized{};
    sanitized.reserve(separator.size() + 2U);
    for (const unsigned char character : separator) {
        if (is_invalid_filename_character(character) || std::iscntrl(character)) {
            sanitized.push_back(' ');
            continue;
        }
        sanitized.push_back(static_cast<char>(character));
    }

    sanitized = collapse_ascii_spaces(std::move(sanitized));
    if (sanitized.empty()) {
        return " - ";
    }

    return " " + sanitized + " ";
}

std::string normalize_extension(std::string extension) {
    extension = trim_ascii_whitespace(lowercase_ascii(std::move(extension)));
    if (extension.empty()) {
        return ".mp4";
    }

    if (!extension.starts_with('.')) {
        extension.insert(extension.begin(), '.');
    }

    std::string normalized{"."};
    for (std::size_t index = 1; index < extension.size(); ++index) {
        const unsigned char character = static_cast<unsigned char>(extension[index]);
        if (std::isalnum(character)) {
            normalized.push_back(static_cast<char>(character));
        }
    }

    return normalized.size() > 1U ? normalized : ".mp4";
}

std::string normalize_path_key(const std::filesystem::path &path) {
    if (path.empty()) {
        return {};
    }

    std::error_code error{};
    auto absolute_path = std::filesystem::absolute(path, error);
    if (error) {
        absolute_path = path;
    }

    std::string normalized = absolute_path.lexically_normal().generic_string();
#ifdef _WIN32
    normalized = lowercase_ascii(std::move(normalized));
#endif
    return normalized;
}

std::string resolve_source_folder_name(const std::filesystem::path &source_path) {
    const auto source_stem = sanitize_filename_fragment(source_path.stem().string());
    if (!source_stem.empty()) {
        return source_stem;
    }

    const auto source_name = sanitize_filename_fragment(source_path.filename().string());
    if (!source_name.empty()) {
        return source_name;
    }

    const auto folder_name = sanitize_filename_fragment(source_path.parent_path().filename().string());
    if (!folder_name.empty()) {
        return folder_name;
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

std::string resolve_resolution_tag(const OutputNamingRequest &request) {
    if (!request.output_width.has_value() || !request.output_height.has_value() ||
        *request.output_width <= 0 || *request.output_height <= 0) {
        return {};
    }

    return std::to_string(*request.output_width) + "x" + std::to_string(*request.output_height);
}

bool is_positive_integer(const std::string_view text) {
    return !text.empty() &&
        std::all_of(text.begin(), text.end(), [](const unsigned char character) { return std::isdigit(character); });
}

std::optional<int> parse_positive_sequence_number(const std::string_view text) {
    if (!is_positive_integer(text)) {
        return std::nullopt;
    }

    int value = 0;
    for (const unsigned char character : text) {
        const int digit = static_cast<int>(character - '0');
        if (value > (std::numeric_limits<int>::max() - digit) / 10) {
            return std::nullopt;
        }
        value = (value * 10) + digit;
    }

    return value > 0 ? std::optional<int>{value} : std::nullopt;
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
    for (std::filesystem::directory_iterator iterator(directory, iteration_error), end; iterator != end;
         iterator.increment(iteration_error)) {
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

        const auto sequence_number = parse_positive_sequence_number(number_text);
        if (sequence_number.has_value()) {
            sequence_numbers.insert(*sequence_number);
        }
    }

    return sequence_numbers;
}

void add_excluded_sequence_numbers(
    std::set<int> &sequence_numbers,
    const std::filesystem::path &output_directory,
    const std::string_view prefix,
    const std::string_view suffix,
    const std::vector<std::filesystem::path> &excluded_output_paths
) {
    const std::string output_directory_key = normalize_path_key(output_directory);
    for (const auto &excluded_path : excluded_output_paths) {
        if (excluded_path.empty()) {
            continue;
        }

        const auto excluded_directory = excluded_path.parent_path();
        if (!output_directory_key.empty() &&
            normalize_path_key(excluded_directory) != output_directory_key) {
            continue;
        }

        const std::string file_name = excluded_path.filename().string();
        if (file_name.size() <= prefix.size() + suffix.size() ||
            !file_name.starts_with(prefix) ||
            !file_name.ends_with(suffix)) {
            continue;
        }

        const std::string_view number_text(
            file_name.data() + static_cast<std::ptrdiff_t>(prefix.size()),
            file_name.size() - prefix.size() - suffix.size()
        );
        const auto sequence_number = parse_positive_sequence_number(number_text);
        if (sequence_number.has_value()) {
            sequence_numbers.insert(*sequence_number);
        }
    }
}

int max_detected_sequence_number(const std::set<int> &used_sequence_numbers) {
    if (used_sequence_numbers.empty()) {
        return 0;
    }

    return *used_sequence_numbers.rbegin();
}

int next_sequence_at_or_after(const int first_candidate, const std::set<int> &used_sequence_numbers) {
    int sequence_number = std::max(first_candidate, 1);
    while (used_sequence_numbers.contains(sequence_number)) {
        ++sequence_number;
    }

    return sequence_number;
}

int normalize_sequence_padding(const int padding) noexcept {
    return std::clamp(padding, 1, 8);
}

std::string format_sequence_number(const int sequence_number, const int padding) {
    std::ostringstream stream;
    stream << std::setw(normalize_sequence_padding(padding)) << std::setfill('0') << sequence_number;
    return stream.str();
}

std::string normalize_counter_key_fragment(std::string value) {
    value = lowercase_ascii(sanitize_filename_fragment(value));
    std::string normalized{};
    normalized.reserve(value.size());

    bool previous_was_dash = false;
    for (const unsigned char character : value) {
        if (std::isalnum(character)) {
            normalized.push_back(static_cast<char>(character));
            previous_was_dash = false;
            continue;
        }

        if (!normalized.empty() && !previous_was_dash) {
            normalized.push_back('-');
            previous_was_dash = true;
        }
    }

    while (!normalized.empty() && normalized.back() == '-') {
        normalized.pop_back();
    }

    if (normalized.size() > 96U) {
        normalized.resize(96U);
        while (!normalized.empty() && normalized.back() == '-') {
            normalized.pop_back();
        }
    }

    return normalized;
}

struct OutputNamingFragments final {
    std::string source_folder_name{};
    std::string custom_text{};
    std::string video_codec_tag{};
    std::string audio_codec_tag{};
    std::string resolution_tag{};
    std::string extension{};
    std::string stem_prefix{};
    std::string stem_suffix{};
    std::string full_suffix{};
    std::string sequence_counter_key{"default"};
    int sequence_padding{2};
    bool has_sequence_number{false};
};

std::string token_value(
    const OutputNamingTokenType token_type,
    const OutputNamingFragments &fragments,
    const std::string_view rendered_sequence
) {
    switch (token_type) {
    case OutputNamingTokenType::selected_text:
        return fragments.custom_text.empty() ? std::string{} : "[" + fragments.custom_text + "]";
    case OutputNamingTokenType::source_folder_name:
        return fragments.source_folder_name;
    case OutputNamingTokenType::sequence_number:
        return std::string(rendered_sequence);
    case OutputNamingTokenType::codec:
        return fragments.video_codec_tag;
    case OutputNamingTokenType::resolution:
        return fragments.resolution_tag;
    default:
        return {};
    }
}

std::string render_template_stem(
    const OutputNamingTemplate &naming_template,
    const OutputNamingFragments &fragments,
    const std::string_view rendered_sequence
) {
    if (!naming_template.enabled) {
        const auto source_stem = sanitize_filename_fragment(fragments.source_folder_name);
        return source_stem.empty() ? "Output" : source_stem;
    }

    const auto tokens = naming_template.tokens.empty()
        ? OutputNaming::default_template().tokens
        : naming_template.tokens;
    const std::string separator = sanitize_separator(naming_template.separator);
    std::string stem{};
    stem.reserve(96U);

    for (const auto &token : tokens) {
        if (!token.enabled) {
            continue;
        }

        const std::string value = token_value(token.type, fragments, rendered_sequence);
        if (value.empty()) {
            continue;
        }

        if (stem.empty()) {
            stem = value;
            continue;
        }

        stem += token.type == OutputNamingTokenType::sequence_number ? separator : " ";
        stem += value;
    }

    stem = collapse_ascii_spaces(std::move(stem));
    return stem.empty() ? "Output" : stem;
}

std::string build_sequence_counter_key(
    const std::string_view custom_text,
    const std::string_view source_folder_name
) {
    (void)source_folder_name;
    const std::string custom_key = normalize_counter_key_fragment(std::string(custom_text));

    if (!custom_key.empty()) {
        return custom_key;
    }

    return "default";
}

OutputNamingFragments build_output_naming_fragments(
    const OutputNamingRequest &request,
    const OutputNamingTemplate &naming_template
) {
    OutputNamingFragments fragments{
        .source_folder_name = resolve_source_folder_name(request.source_path),
        .custom_text = sanitize_filename_fragment(request.custom_text),
        .video_codec_tag = resolve_video_codec_tag(request.video_codec),
        .audio_codec_tag = resolve_audio_codec_tag(request),
        .resolution_tag = resolve_resolution_tag(request),
        .extension = normalize_extension(request.extension_hint)
    };
    fragments.sequence_counter_key =
        build_sequence_counter_key(fragments.custom_text, fragments.source_folder_name);

    const auto tokens = naming_template.tokens.empty()
        ? OutputNaming::default_template().tokens
        : naming_template.tokens;
    for (const auto &token : tokens) {
        if (token.enabled && token.type == OutputNamingTokenType::sequence_number) {
            fragments.has_sequence_number = true;
            fragments.sequence_padding = normalize_sequence_padding(token.sequence_padding);
            break;
        }
    }

    if (fragments.has_sequence_number) {
        const std::string marked_stem = render_template_stem(naming_template, fragments, kSequenceMarker);
        const auto marker_position = marked_stem.find(kSequenceMarker);
        if (marker_position != std::string::npos) {
            fragments.stem_prefix = marked_stem.substr(0, marker_position);
            fragments.stem_suffix = marked_stem.substr(marker_position + kSequenceMarker.size());
        } else {
            fragments.has_sequence_number = false;
            fragments.stem_prefix = marked_stem;
        }
    } else {
        fragments.stem_prefix = render_template_stem(naming_template, fragments, {});
    }

    fragments.full_suffix = fragments.stem_suffix + fragments.extension;
    return fragments;
}

OutputNamingResult build_output_naming_result(
    const std::filesystem::path &output_directory,
    const OutputNamingFragments &fragments,
    const int sequence_number
) {
    const std::string rendered_sequence_number = fragments.has_sequence_number
        ? format_sequence_number(sequence_number, fragments.sequence_padding)
        : std::string{};
    const std::string file_stem = fragments.has_sequence_number
        ? fragments.stem_prefix + rendered_sequence_number + fragments.stem_suffix
        : fragments.stem_prefix;
    const std::string file_name = file_stem + fragments.extension;
    const std::filesystem::path output_path = output_directory.empty()
        ? std::filesystem::path(file_name)
        : (output_directory / file_name).lexically_normal();

    return OutputNamingResult{
        .output_path = output_path,
        .file_name = file_name,
        .source_folder_name = fragments.source_folder_name,
        .custom_text = fragments.custom_text,
        .video_codec_tag = fragments.video_codec_tag,
        .audio_codec_tag = fragments.audio_codec_tag,
        .resolution_tag = fragments.resolution_tag,
        .extension = fragments.extension,
        .sequence_number = fragments.has_sequence_number ? sequence_number : 0,
        .sequence_counter_key = fragments.sequence_counter_key
    };
}

bool output_path_exists(const std::filesystem::path &path) {
    std::error_code error{};
    const bool exists = std::filesystem::exists(path, error);
    return error || exists;
}

bool output_path_is_excluded(
    const std::filesystem::path &path,
    const std::vector<std::filesystem::path> &excluded_output_paths
) {
    const std::string path_key = normalize_path_key(path);
    if (path_key.empty()) {
        return false;
    }

    return std::any_of(
        excluded_output_paths.begin(),
        excluded_output_paths.end(),
        [&path_key](const std::filesystem::path &excluded_path) {
            return normalize_path_key(excluded_path) == path_key;
        }
    );
}

OutputNamingResult make_unique_non_sequence_result(
    const OutputNamingResult &base_result,
    const std::filesystem::path &output_directory,
    const OutputNamingFragments &fragments,
    const std::vector<std::filesystem::path> &excluded_output_paths
) {
    if (!output_path_exists(base_result.output_path) &&
        !output_path_is_excluded(base_result.output_path, excluded_output_paths)) {
        return base_result;
    }

    // No-sequence templates have no persisted no-reuse counter; avoid only
    // the paths visible in the filesystem and the caller-provided exclusions.
    for (int copy_index = 2; copy_index < 1000; ++copy_index) {
        std::ostringstream suffix;
        suffix << '_' << std::setw(3) << std::setfill('0') << copy_index;
        const std::string file_stem = fragments.stem_prefix + suffix.str();
        const std::string file_name = file_stem + fragments.extension;
        const std::filesystem::path output_path = output_directory.empty()
            ? std::filesystem::path(file_name)
            : (output_directory / file_name).lexically_normal();
        if (output_path_exists(output_path) || output_path_is_excluded(output_path, excluded_output_paths)) {
            continue;
        }

        auto result = base_result;
        result.output_path = output_path;
        result.file_name = file_name;
        return result;
    }

    for (int copy_index = 1000; copy_index < 10000; ++copy_index) {
        const std::string file_stem = fragments.stem_prefix + "_" + std::to_string(copy_index);
        const std::string file_name = file_stem + fragments.extension;
        const std::filesystem::path output_path = output_directory.empty()
            ? std::filesystem::path(file_name)
            : (output_directory / file_name).lexically_normal();
        if (output_path_exists(output_path) || output_path_is_excluded(output_path, excluded_output_paths)) {
            continue;
        }

        auto result = base_result;
        result.output_path = output_path;
        result.file_name = file_name;
        return result;
    }

    return base_result;
}

OutputNamingReservationResult build_reservation_result(
    const std::filesystem::path &output_directory,
    const OutputNamingFragments &fragments,
    const int sequence_number,
    const std::vector<std::filesystem::path> &excluded_output_paths = {}
) {
    auto result = build_output_naming_result(output_directory, fragments, sequence_number);
    if (!fragments.has_sequence_number) {
        result = make_unique_non_sequence_result(result, output_directory, fragments, excluded_output_paths);
    }

    return OutputNamingReservationResult{
        .result = result,
        .sequence_counter_key = result.sequence_counter_key,
        .assigned_sequence_number = result.sequence_number,
        .persisted_sequence_number = result.sequence_number
    };
}

int choose_sequence_number(
    const OutputNamingFragments &fragments,
    const std::filesystem::path &output_directory,
    const int stored_sequence_number,
    const std::set<int> *additional_used_numbers = nullptr,
    const std::vector<std::filesystem::path> &excluded_output_paths = {}
) {
    if (!fragments.has_sequence_number) {
        return 0;
    }

    std::set<int> used_sequence_numbers =
        collect_used_sequence_numbers(output_directory, fragments.stem_prefix, fragments.full_suffix);
    const int first_candidate = std::max(
        stored_sequence_number + 1,
        max_detected_sequence_number(used_sequence_numbers) + 1
    );
    add_excluded_sequence_numbers(
        used_sequence_numbers,
        output_directory,
        fragments.stem_prefix,
        fragments.full_suffix,
        excluded_output_paths
    );
    if (additional_used_numbers != nullptr) {
        used_sequence_numbers.insert(additional_used_numbers->begin(), additional_used_numbers->end());
    }

    return next_sequence_at_or_after(first_candidate, used_sequence_numbers);
}

struct ReservationGroupKey final {
    std::string directory_key{};
    std::string stem_prefix{};
    std::string full_suffix{};
    std::string counter_key{};

    [[nodiscard]] auto tie() const noexcept {
        return std::tie(directory_key, stem_prefix, full_suffix, counter_key);
    }

    [[nodiscard]] bool operator<(const ReservationGroupKey &other) const noexcept {
        return tie() < other.tie();
    }
};

struct ReservationGroupState final {
    std::set<int> used_sequence_numbers{};
    int existing_next_candidate{1};
};

struct CounterReservationState final {
    int next_candidate{1};
};

}  // namespace

OutputNamingTemplate OutputNaming::default_template() {
    return OutputNamingTemplate{
        .enabled = true,
        .separator = " - ",
        .tokens = {
            OutputNamingToken{.type = OutputNamingTokenType::selected_text, .enabled = true},
            OutputNamingToken{.type = OutputNamingTokenType::source_folder_name, .enabled = true},
            OutputNamingToken{.type = OutputNamingTokenType::sequence_number, .enabled = true, .sequence_padding = 2},
            OutputNamingToken{.type = OutputNamingTokenType::codec, .enabled = true},
            OutputNamingToken{.type = OutputNamingTokenType::resolution, .enabled = true}
        },
        .crc32_suffix_enabled = false
    };
}

const char *OutputNaming::to_string(const OutputNamingTokenType type) noexcept {
    switch (type) {
    case OutputNamingTokenType::selected_text:
        return "selectedText";
    case OutputNamingTokenType::source_folder_name:
        return "sourceFolderName";
    case OutputNamingTokenType::sequence_number:
        return "sequenceNumber";
    case OutputNamingTokenType::codec:
        return "codec";
    case OutputNamingTokenType::resolution:
        return "resolution";
    default:
        return "unknown";
    }
}

std::optional<OutputNamingTokenType> OutputNaming::token_type_from_string(const std::string_view text) {
    if (text == "selectedText") {
        return OutputNamingTokenType::selected_text;
    }
    if (text == "sourceFolderName") {
        return OutputNamingTokenType::source_folder_name;
    }
    if (text == "sequenceNumber") {
        return OutputNamingTokenType::sequence_number;
    }
    if (text == "codec") {
        return OutputNamingTokenType::codec;
    }
    if (text == "resolution") {
        return OutputNamingTokenType::resolution;
    }

    return std::nullopt;
}

std::string OutputNaming::sequence_counter_key(
    const OutputNamingRequest &request,
    const OutputNamingTemplate &naming_template
) {
    return build_output_naming_fragments(request, naming_template).sequence_counter_key;
}

OutputNamingResult OutputNaming::suggest(const OutputNamingRequest &request) {
    return suggest(request, default_template(), 0);
}

OutputNamingResult OutputNaming::suggest(
    const OutputNamingRequest &request,
    const OutputNamingTemplate &naming_template
) {
    return suggest(request, naming_template, 0);
}

OutputNamingResult OutputNaming::suggest(
    const OutputNamingRequest &request,
    const OutputNamingTemplate &naming_template,
    const int stored_sequence_number
) {
    const OutputNamingFragments fragments = build_output_naming_fragments(request, naming_template);
    const int sequence_number = choose_sequence_number(fragments, request.output_directory, stored_sequence_number);
    return build_output_naming_result(request.output_directory, fragments, sequence_number);
}

OutputNamingReservationResult OutputNaming::reserve_next(
    const OutputNamingRequest &request,
    const OutputNamingTemplate &naming_template,
    const int stored_sequence_number
) {
    const OutputNamingFragments fragments = build_output_naming_fragments(request, naming_template);
    const int sequence_number = choose_sequence_number(fragments, request.output_directory, stored_sequence_number);
    return build_reservation_result(request.output_directory, fragments, sequence_number);
}

std::vector<OutputNamingResult> OutputNaming::reserve_batch(const std::vector<OutputNamingRequest> &requests) {
    std::vector<OutputNamingReservationRequest> reservation_requests{};
    reservation_requests.reserve(requests.size());
    for (const auto &request : requests) {
        reservation_requests.push_back(OutputNamingReservationRequest{
            .request = request,
            .naming_template = default_template(),
            .stored_sequence_number = 0,
            .excluded_output_paths = {}
        });
    }

    const auto reservations = reserve_batch(reservation_requests);
    std::vector<OutputNamingResult> results{};
    results.reserve(reservations.size());
    for (const auto &reservation : reservations) {
        results.push_back(reservation.result);
    }
    return results;
}

std::vector<OutputNamingReservationResult> OutputNaming::reserve_batch(
    const std::vector<OutputNamingReservationRequest> &requests
) {
    std::vector<OutputNamingReservationResult> results{};
    results.reserve(requests.size());

    std::map<ReservationGroupKey, ReservationGroupState> reservation_groups{};
    std::map<std::string, CounterReservationState> counter_groups{};
    std::vector<std::filesystem::path> reserved_output_paths{};
    for (const auto &request : requests) {
        const OutputNamingFragments fragments =
            build_output_naming_fragments(request.request, request.naming_template);
        std::vector<std::filesystem::path> excluded_output_paths = request.excluded_output_paths;
        excluded_output_paths.insert(
            excluded_output_paths.end(),
            reserved_output_paths.begin(),
            reserved_output_paths.end()
        );

        if (!fragments.has_sequence_number) {
            results.push_back(build_reservation_result(
                request.request.output_directory,
                fragments,
                0,
                excluded_output_paths
            ));
            reserved_output_paths.push_back(results.back().result.output_path);
            continue;
        }

        const ReservationGroupKey key{
            .directory_key = normalize_path_key(request.request.output_directory),
            .stem_prefix = fragments.stem_prefix,
            .full_suffix = fragments.full_suffix,
            .counter_key = fragments.sequence_counter_key
        };

        auto [iterator, inserted] = reservation_groups.try_emplace(key);
        if (inserted) {
            iterator->second.used_sequence_numbers = collect_used_sequence_numbers(
                request.request.output_directory,
                fragments.stem_prefix,
                fragments.full_suffix
            );
            iterator->second.existing_next_candidate =
                max_detected_sequence_number(iterator->second.used_sequence_numbers) + 1;
        }

        add_excluded_sequence_numbers(
            iterator->second.used_sequence_numbers,
            request.request.output_directory,
            fragments.stem_prefix,
            fragments.full_suffix,
            excluded_output_paths
        );

        auto [counter_iterator, counter_inserted] =
            counter_groups.try_emplace(fragments.sequence_counter_key);
        if (counter_inserted) {
            counter_iterator->second.next_candidate = request.stored_sequence_number + 1;
        } else {
            counter_iterator->second.next_candidate =
                std::max(counter_iterator->second.next_candidate, request.stored_sequence_number + 1);
        }

        const int first_candidate = std::max(
            counter_iterator->second.next_candidate,
            iterator->second.existing_next_candidate
        );
        const int sequence_number =
            next_sequence_at_or_after(first_candidate, iterator->second.used_sequence_numbers);
        iterator->second.used_sequence_numbers.insert(sequence_number);
        counter_iterator->second.next_candidate = sequence_number + 1;
        results.push_back(build_reservation_result(
            request.request.output_directory,
            fragments,
            sequence_number,
            excluded_output_paths
        ));
        reserved_output_paths.push_back(results.back().result.output_path);
    }

    return results;
}

std::string OutputNaming::crc32_hex_for_bytes(const std::string_view bytes) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const unsigned char byte : bytes) {
        crc = update_crc32(crc, byte);
    }
    return format_crc32_hex(crc ^ 0xFFFFFFFFU);
}

std::optional<std::string> OutputNaming::calculate_file_crc32_hex(
    const std::filesystem::path &path,
    std::string *error_message
) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        if (error_message != nullptr) {
            *error_message = "Could not open output file for CRC32 calculation.";
        }
        return std::nullopt;
    }

    std::uint32_t crc = 0xFFFFFFFFU;
    std::array<char, 1024 * 1024> buffer{};
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = stream.gcount();
        for (std::streamsize index = 0; index < count; ++index) {
            crc = update_crc32(crc, static_cast<unsigned char>(buffer[static_cast<std::size_t>(index)]));
        }
    }

    if (!stream.eof()) {
        if (error_message != nullptr) {
            *error_message = "Could not read output file for CRC32 calculation.";
        }
        return std::nullopt;
    }

    return format_crc32_hex(crc ^ 0xFFFFFFFFU);
}

std::filesystem::path OutputNaming::append_or_replace_crc32_suffix(
    const std::filesystem::path &path,
    const std::string_view crc32_hex
) {
    return build_crc32_suffix_path(
        path,
        remove_trailing_crc32_tag(filesystem::path_component_to_utf8_string(path.stem())),
        crc32_hex
    );
}

std::optional<std::filesystem::path> OutputNaming::choose_available_crc32_suffix_path(
    const std::filesystem::path &path,
    const std::string_view crc32_hex,
    std::string *error_message
) {
    const std::string base_stem =
        remove_trailing_crc32_tag(filesystem::path_component_to_utf8_string(path.stem()));
    const auto original_path = path.lexically_normal();
    bool availability_error = false;
    const auto try_candidate =
        [&original_path, &availability_error, error_message](const std::filesystem::path &candidate) {
        if (candidate.lexically_normal() == original_path) {
            return true;
        }
        std::error_code exists_error{};
        const bool exists = std::filesystem::exists(candidate, exists_error);
        if (exists_error) {
            availability_error = true;
            if (error_message != nullptr) {
                *error_message = "Could not verify CRC32 target filename availability.";
            }
            return false;
        }
        return !exists;
    };

    const auto direct_candidate = build_crc32_suffix_path(path, base_stem, crc32_hex);
    if (try_candidate(direct_candidate)) {
        return direct_candidate;
    }
    if (availability_error) {
        return std::nullopt;
    }

    for (int copy_index = 1; copy_index < 1000; ++copy_index) {
        const std::string copy_stem = base_stem + (copy_index == 1
            ? " Copy"
            : " Copy " + std::to_string(copy_index));
        const auto copy_candidate = build_crc32_suffix_path(path, copy_stem, crc32_hex);
        if (try_candidate(copy_candidate)) {
            return copy_candidate;
        }
        if (availability_error) {
            return std::nullopt;
        }
    }

    if (error_message != nullptr && error_message->empty()) {
        *error_message = "Could not find an available CRC32 target filename.";
    }
    return std::nullopt;
}

}  // namespace utsure::core::job
