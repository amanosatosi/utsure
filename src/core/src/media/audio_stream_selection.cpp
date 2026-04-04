#include "utsure/core/media/audio_stream_selection.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace utsure::core::media {

namespace {

std::string trim_ascii(std::string value) {
    const auto first = std::find_if_not(
        value.begin(),
        value.end(),
        [](const unsigned char character) {
            return std::isspace(character) != 0;
        }
    );
    if (first == value.end()) {
        return {};
    }

    const auto last = std::find_if_not(
        value.rbegin(),
        value.rend(),
        [](const unsigned char character) {
            return std::isspace(character) != 0;
        }
    ).base();
    return std::string(first, last);
}

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

std::string normalize_language_metadata(std::string value) {
    value = lowercase_ascii(trim_ascii(std::move(value)));
    std::replace(value.begin(), value.end(), '_', '-');
    const auto separator = value.find_first_of("-.;, /\\");
    if (separator != std::string::npos) {
        value.erase(separator);
    }
    return value;
}

std::string normalize_title_metadata(std::string value) {
    value = lowercase_ascii(trim_ascii(std::move(value)));

    std::string normalized{};
    normalized.reserve(value.size());
    bool previous_was_space = false;
    for (const unsigned char character : value) {
        const bool separator = std::isspace(character) != 0 || character == '_' || character == '-';
        if (separator) {
            if (!previous_was_space && !normalized.empty()) {
                normalized.push_back(' ');
            }
            previous_was_space = true;
            continue;
        }

        normalized.push_back(static_cast<char>(character));
        previous_was_space = false;
    }

    if (!normalized.empty() && normalized.back() == ' ') {
        normalized.pop_back();
    }

    return normalized;
}

bool normalized_language_is_japanese(const std::string &language_tag) {
    return language_tag == "ja" ||
        language_tag == "jpn" ||
        language_tag == "japanese";
}

bool normalized_title_is_japanese(const std::string &title) {
    return title == "ja" ||
        title == "jpn" ||
        title == "japanese" ||
        title == "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E";
}

std::vector<const AudioStreamInfo *> decodable_audio_streams_by_index(
    const std::vector<AudioStreamInfo> &audio_streams
) {
    std::vector<const AudioStreamInfo *> ordered_streams{};
    ordered_streams.reserve(audio_streams.size());
    for (const auto &audio_stream : audio_streams) {
        if (!audio_stream.decoder_available) {
            continue;
        }
        ordered_streams.push_back(&audio_stream);
    }

    std::sort(
        ordered_streams.begin(),
        ordered_streams.end(),
        [](const AudioStreamInfo *left, const AudioStreamInfo *right) {
            return left->stream_index < right->stream_index;
        }
    );
    return ordered_streams;
}

}  // namespace

bool audio_stream_has_explicit_japanese_metadata(const AudioStreamInfo &audio_stream) noexcept {
    if (audio_stream.language_tag.has_value() &&
        normalized_language_is_japanese(normalize_language_metadata(*audio_stream.language_tag))) {
        return true;
    }

    if (audio_stream.title.has_value() &&
        normalized_title_is_japanese(normalize_title_metadata(*audio_stream.title))) {
        return true;
    }

    return false;
}

std::optional<int> select_preferred_audio_stream_index(const std::vector<AudioStreamInfo> &audio_streams) noexcept {
    const auto decodable_streams = decodable_audio_streams_by_index(audio_streams);
    if (decodable_streams.empty()) {
        return std::nullopt;
    }

    const auto choose_first_matching_stream_index =
        [&](const auto &predicate) -> std::optional<int> {
        for (const auto *audio_stream : decodable_streams) {
            if (predicate(*audio_stream)) {
                return audio_stream->stream_index;
            }
        }
        return std::nullopt;
    };

    const auto japanese_default_stream_index = choose_first_matching_stream_index(
        [](const AudioStreamInfo &audio_stream) {
            return audio_stream.disposition_default &&
                audio_stream_has_explicit_japanese_metadata(audio_stream);
        }
    );
    if (japanese_default_stream_index.has_value()) {
        return japanese_default_stream_index;
    }

    const auto japanese_stream_index = choose_first_matching_stream_index(
        [](const AudioStreamInfo &audio_stream) {
            return audio_stream_has_explicit_japanese_metadata(audio_stream);
        }
    );
    if (japanese_stream_index.has_value()) {
        return japanese_stream_index;
    }

    const auto default_stream_index = choose_first_matching_stream_index(
        [](const AudioStreamInfo &audio_stream) {
            return audio_stream.disposition_default;
        }
    );
    if (default_stream_index.has_value()) {
        return default_stream_index;
    }

    return decodable_streams.front()->stream_index;
}

}  // namespace utsure::core::media
