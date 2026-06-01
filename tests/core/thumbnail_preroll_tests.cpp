#include "utsure/core/subtitles/thumbnail_preroll.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

bool contains_text(const std::string &text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

int run_parse_assertion() {
    const std::string script =
        "[Script Info]\n"
        "Title: sample\n"
        "\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:00.00,0:00:05.00,Default,IGNORED,0,0,0,,Not this\n"
        "Dialogue: 0,0:00:00.00,0:00:05.00,Default,utsure_data,0,0,0,,{\\an5}Episode 06\n";

    const std::string replaced =
        utsure::core::subtitles::ThumbnailPrerollResolver::replace_utsure_data_text(
            script,
            "{\\an5\\bord2}Episode 07"
        );

    if (!contains_text(replaced, "IGNORED,0,0,0,,Not this")) {
        return fail("Thumbnail ASS replacement changed an unrelated dialogue line.");
    }

    if (!contains_text(replaced, "utsure_data,0,0,0,,{\\an5\\bord2}Episode 07")) {
        return fail("Thumbnail ASS replacement did not update the utsure_data dialogue text.");
    }

    const auto temp_path =
        std::filesystem::temp_directory_path() /
        ("utsure-thumbnail-preroll-parse-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".ass");
    {
        std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
        output << script;
    }

    const auto extracted = utsure::core::subtitles::ThumbnailPrerollResolver::extract_utsure_data_text(temp_path);
    std::error_code remove_error{};
    std::filesystem::remove(temp_path, remove_error);

    if (!extracted.has_value() || *extracted != "{\\an5}Episode 06") {
        return fail("Thumbnail ASS parser did not return the utsure_data text including override tags.");
    }

    std::cout << "thumbnail.ass.utsure_data={\\an5}Episode 06\n";
    std::cout << "thumbnail.ass.replaced=yes\n";
    return 0;
}

int run_resolve_assertion(
    const std::filesystem::path &subtitle_path,
    const std::filesystem::path &large_thumbnail_path,
    const std::filesystem::path &four_by_three_thumbnail_path
) {
    const auto result = utsure::core::subtitles::ThumbnailPrerollResolver::resolve(
        utsure::core::subtitles::ThumbnailPrerollResolveRequest{
            .enabled = true,
            .subtitle_path = subtitle_path,
            .explicit_image_path = std::nullopt,
            .explicit_overlay_ass_path = std::nullopt,
            .required_width = 320,
            .required_height = 180
        }
    );

    if (!result.has_assets()) {
        return fail("Thumbnail resolver did not select the thumbnail.* and thumbnail.ass assets.");
    }

    if (result.diagnostics.empty() ||
        !contains_text(result.diagnostics.front(), "already matches 320x180")) {
        return fail("Thumbnail resolver did not report the exact same-size thumbnail case.");
    }

    if (result.assets->image_path.filename().string() != "thumbnail.png" ||
        result.assets->overlay_ass_path.filename().string() != "thumbnail.ass" ||
        result.assets->title_text != "{\\an5}Episode 06") {
        return fail("Thumbnail resolver selected unexpected assets or title text.");
    }

    const auto downscale_result = utsure::core::subtitles::ThumbnailPrerollResolver::resolve(
        utsure::core::subtitles::ThumbnailPrerollResolveRequest{
            .enabled = true,
            .subtitle_path = subtitle_path,
            .explicit_image_path = std::nullopt,
            .explicit_overlay_ass_path = std::nullopt,
            .required_width = 160,
            .required_height = 90
        }
    );

    if (!downscale_result.has_assets() ||
        downscale_result.decision != utsure::core::subtitles::ThumbnailPrerollDecisionCode::ready ||
        downscale_result.diagnostics.empty() ||
        !contains_text(downscale_result.diagnostics.front(), "normalized thumbnail source from 320x180 to 160x90")) {
        return fail("Thumbnail resolver did not accept a resize-compatible thumbnail that can be normalized to the output size.");
    }

    const auto large_downscale_result = utsure::core::subtitles::ThumbnailPrerollResolver::resolve(
        utsure::core::subtitles::ThumbnailPrerollResolveRequest{
            .enabled = true,
            .subtitle_path = subtitle_path,
            .explicit_image_path = large_thumbnail_path,
            .explicit_overlay_ass_path = large_thumbnail_path.parent_path() / "thumbnail.ass",
            .required_width = 960,
            .required_height = 540
        }
    );

    if (!large_downscale_result.has_assets() ||
        large_downscale_result.decision != utsure::core::subtitles::ThumbnailPrerollDecisionCode::ready ||
        large_downscale_result.diagnostics.empty() ||
        !contains_text(large_downscale_result.diagnostics.front(), "normalized thumbnail source from 1920x1080 to 960x540")) {
        return fail("Thumbnail resolver did not accept the 1920x1080 to 960x540 thumbnail normalization case.");
    }

    const auto rounded_downscale_result = utsure::core::subtitles::ThumbnailPrerollResolver::resolve(
        utsure::core::subtitles::ThumbnailPrerollResolveRequest{
            .enabled = true,
            .subtitle_path = subtitle_path,
            .explicit_image_path = large_thumbnail_path,
            .explicit_overlay_ass_path = large_thumbnail_path.parent_path() / "thumbnail.ass",
            .required_width = 854,
            .required_height = 480
        }
    );

    if (!rounded_downscale_result.has_assets() ||
        rounded_downscale_result.decision != utsure::core::subtitles::ThumbnailPrerollDecisionCode::ready ||
        rounded_downscale_result.diagnostics.empty() ||
        !contains_text(rounded_downscale_result.diagnostics.front(), "within resize aspect tolerance")) {
        return fail("Thumbnail resolver did not accept the 1920x1080 to 854x480 encoder-safe rounding case.");
    }

    const auto mismatch_result = utsure::core::subtitles::ThumbnailPrerollResolver::resolve(
        utsure::core::subtitles::ThumbnailPrerollResolveRequest{
            .enabled = true,
            .subtitle_path = subtitle_path,
            .explicit_image_path = four_by_three_thumbnail_path,
            .explicit_overlay_ass_path = four_by_three_thumbnail_path.parent_path() / "thumbnail.ass",
            .required_width = 854,
            .required_height = 480
        }
    );

    if (mismatch_result.has_assets() ||
        mismatch_result.decision != utsure::core::subtitles::ThumbnailPrerollDecisionCode::no_accepted_thumbnail ||
        mismatch_result.diagnostics.empty() ||
        !contains_text(mismatch_result.diagnostics.front(), "aspect mismatch exceeds resize tolerance")) {
        return fail("Thumbnail resolver did not clearly reject a 4:3 thumbnail for 16:9-ish output.");
    }

    std::cout << "thumbnail.resolve.image=thumbnail.png\n";
    std::cout << "thumbnail.resolve.overlay=thumbnail.ass\n";
    std::cout << "thumbnail.resolve.downscale=accepted\n";
    std::cout << "thumbnail.resolve.large_downscale=accepted\n";
    std::cout << "thumbnail.resolve.rounded_downscale=accepted\n";
    std::cout << "thumbnail.resolve.aspect_mismatch=rejected\n";
    return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
    if (argc < 2) {
        return fail("Usage: utsure_core_thumbnail_preroll_tests [--parse|--resolve <subtitle.ass> <large-thumbnail.png> <4x3-thumbnail.png>]");
    }

    const std::string_view mode(argv[1]);
    if (mode == "--parse") {
        return run_parse_assertion();
    }

    if (mode == "--resolve") {
        if (argc != 5) {
            return fail("Usage: utsure_core_thumbnail_preroll_tests --resolve <subtitle.ass> <large-thumbnail.png> <4x3-thumbnail.png>");
        }

        return run_resolve_assertion(argv[2], argv[3], argv[4]);
    }

    return fail("Unknown thumbnail pre-roll test mode.");
}
