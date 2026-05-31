#include "utsure/core/media/audio_stream_selection.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

utsure::core::media::AudioStreamInfo make_stream(
    const int stream_index,
    std::optional<std::string> language,
    std::optional<std::string> title,
    const bool default_disposition
) {
    return utsure::core::media::AudioStreamInfo{
        .stream_index = stream_index,
        .codec_name = "aac",
        .sample_rate = 48000,
        .channel_count = 2,
        .language_tag = std::move(language),
        .title = std::move(title),
        .disposition_default = default_disposition,
        .decoder_available = true
    };
}

int run_audio_selection_assertions() {
    using utsure::core::media::audio_stream_has_explicit_japanese_metadata;
    using utsure::core::media::select_preferred_audio_stream_index;

    const std::vector japanese_plus_default_english{
        make_stream(1, std::optional<std::string>{"eng"}, std::optional<std::string>{"English"}, true),
        make_stream(2, std::optional<std::string>{"jpn"}, std::optional<std::string>{"Japanese"}, false)
    };
    const auto japanese_selected = select_preferred_audio_stream_index(japanese_plus_default_english);
    if (!japanese_selected.has_value() || *japanese_selected != 2) {
        return fail("Japanese audio was not selected ahead of default English audio.");
    }

    const std::vector default_no_japanese{
        make_stream(1, std::optional<std::string>{"eng"}, std::optional<std::string>{"English"}, false),
        make_stream(2, std::optional<std::string>{"deu"}, std::optional<std::string>{"Deutsch"}, true)
    };
    const auto default_selected = select_preferred_audio_stream_index(default_no_japanese);
    if (!default_selected.has_value() || *default_selected != 2) {
        return fail("Default disposition audio was not selected when no Japanese track was present.");
    }

    const std::vector no_metadata{
        make_stream(3, std::nullopt, std::nullopt, false),
        make_stream(4, std::nullopt, std::nullopt, false)
    };
    const auto first_selected = select_preferred_audio_stream_index(no_metadata);
    if (!first_selected.has_value() || *first_selected != 3) {
        return fail("First usable audio stream was not selected as fallback.");
    }

    const auto japanese_title = make_stream(5, std::nullopt, std::optional<std::string>{"日本語"}, false);
    if (!audio_stream_has_explicit_japanese_metadata(japanese_title)) {
        return fail("Japanese title metadata was not recognized.");
    }

    std::cout << "audio.selection=ok\n";
    return 0;
}

}  // namespace

int main() {
    return run_audio_selection_assertions();
}
