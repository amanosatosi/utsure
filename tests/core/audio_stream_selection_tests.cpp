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
    const bool default_disposition,
    const bool decoder_available = true
) {
    return utsure::core::media::AudioStreamInfo{
        .stream_index = stream_index,
        .codec_name = "aac",
        .sample_rate = 48000,
        .channel_count = 2,
        .language_tag = std::move(language),
        .title = std::move(title),
        .disposition_default = default_disposition,
        .decoder_available = decoder_available
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

    const std::vector jp_language_tag{
        make_stream(1, std::optional<std::string>{"eng"}, std::optional<std::string>{"English"}, true),
        make_stream(2, std::optional<std::string>{"jp"}, std::optional<std::string>{"JPN Audio"}, false)
    };
    const auto jp_selected = select_preferred_audio_stream_index(jp_language_tag);
    if (!jp_selected.has_value() || *jp_selected != 2) {
        return fail("jp language tag was not recognized as Japanese.");
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

    const auto japanese_audio_title =
        make_stream(5, std::nullopt, std::optional<std::string>{"Japanese Audio"}, false);
    if (!audio_stream_has_explicit_japanese_metadata(japanese_audio_title)) {
        return fail("Japanese Audio title metadata was not recognized.");
    }

    const auto jpn_audio_title =
        make_stream(6, std::nullopt, std::optional<std::string>{"JPN Audio"}, false);
    if (!audio_stream_has_explicit_japanese_metadata(jpn_audio_title)) {
        return fail("JPN Audio title metadata was not recognized.");
    }

    const auto japanese_commentary_title =
        make_stream(7, std::nullopt, std::optional<std::string>{"Japanese Commentary"}, false);
    if (!audio_stream_has_explicit_japanese_metadata(japanese_commentary_title)) {
        return fail("Japanese Commentary title metadata was not recognized.");
    }

    const auto japanese_utf8_title =
        make_stream(8, std::nullopt, std::optional<std::string>{"\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E Main"}, false);
    if (!audio_stream_has_explicit_japanese_metadata(japanese_utf8_title)) {
        return fail("UTF-8 Japanese title metadata was not recognized.");
    }

    const std::vector undecodable_default_then_decodable{
        make_stream(1, std::optional<std::string>{"jpn"}, std::optional<std::string>{"Japanese"}, true, false),
        make_stream(2, std::optional<std::string>{"eng"}, std::optional<std::string>{"English"}, false)
    };
    const auto decodable_selected = select_preferred_audio_stream_index(undecodable_default_then_decodable);
    if (!decodable_selected.has_value() || *decodable_selected != 2) {
        return fail("Default auto-selection should prefer decodable tracks for normal encode paths.");
    }

    std::cout << "audio.selection=ok\n";
    return 0;
}

}  // namespace

int main() {
    return run_audio_selection_assertions();
}
