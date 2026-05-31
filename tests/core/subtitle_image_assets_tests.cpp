#include "utsure/core/subtitles/subtitle_image_assets.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace {

int fail(const std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

bool contains_reference(
    const std::vector<utsure::core::subtitles::SubtitleImageAssetReference> &references,
    const std::string_view name
) {
    for (const auto &reference : references) {
        if (reference.name == name) {
            return true;
        }
    }
    return false;
}

std::filesystem::path make_temp_dir() {
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("utsure-subtitle-image-assets-" + std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()
         ));
    std::filesystem::create_directories(directory);
    return directory;
}

void write_text(const std::filesystem::path &path, const std::string_view text) {
    std::ofstream stream(path, std::ios::binary);
    stream << text;
}

std::string ass_with_text(const std::string_view text) {
    return std::string{
        "[Script Info]\n"
        "Title: img asset test\n"
        "\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,"
    } + std::string{text} + "\n";
}

int run_scan_assertions() {
    const auto no_assets = utsure::core::subtitles::find_subtitle_image_asset_references_in_text(
        ass_with_text("{\\pos(10,10)}No image")
    );
    if (!no_assets.empty()) {
        return fail("ASS without img tags reported image assets.");
    }

    const auto ignored_references = utsure::core::subtitles::find_subtitle_image_asset_references_in_text(
        "[Script Info]\n"
        "; {\\img(script-comment.png)}\n"
        "Title: {\\img(title.png)}\n"
        "\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Comment: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,{\\img(comment.png)}Ignored\n"
        "Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,{\\img(rendered.png)}Rendered\n"
    );
    if (ignored_references.size() != 1U || !contains_reference(ignored_references, "rendered.png")) {
        return fail("ASS img scanner should only require rendered Dialogue event text assets.");
    }

    const auto references = utsure::core::subtitles::find_subtitle_image_asset_references_in_text(
        ass_with_text("{\\img(logo.png)}A {\\1img(\"assets/tile.png\", 2, 3)}B {\\2img(assets\\tile2.png)}C {\\img(logo.png)}D")
    );
    if (references.size() != 3U ||
        !contains_reference(references, "logo.png") ||
        !contains_reference(references, "assets/tile.png") ||
        !contains_reference(references, "assets\\tile2.png")) {
        return fail("ASS img scanner did not extract and de-duplicate expected references.");
    }

    std::cout << "subtitle_img.scan=ok\n";
    return 0;
}

int run_resolve_decode_assertions(const std::filesystem::path &sample_png) {
    const auto root = make_temp_dir();
    const auto sidecar = root / "assets";
    std::filesystem::create_directories(sidecar);
    std::filesystem::copy_file(sample_png, sidecar / "logo.png", std::filesystem::copy_options::overwrite_existing);
    write_text(sidecar / "logo", "not an image");

    const auto subtitle_path = root / "sample.ass";
    write_text(subtitle_path, ass_with_text("{\\img(logo)}Image"));
    const auto result = utsure::core::subtitles::load_subtitle_image_assets(subtitle_path);
    if (!result.succeeded()) {
        return fail("Valid subtitle img asset did not load: " + result.error->message);
    }
    if (result.references.size() != 1U ||
        result.assets.size() != 1U ||
        result.assets[0].name != "logo" ||
        result.assets[0].source_path.filename() != "logo.png" ||
        result.assets[0].width <= 0 ||
        result.assets[0].height <= 0 ||
        result.assets[0].stride < result.assets[0].width * 4 ||
        result.assets[0].rgba.empty()) {
        return fail("Valid subtitle img asset loaded with unexpected metadata.");
    }

    const auto missing_path = root / "missing.ass";
    write_text(missing_path, ass_with_text("{\\img(missing.png)}Missing"));
    const auto missing_result = utsure::core::subtitles::load_subtitle_image_assets(missing_path);
    if (missing_result.succeeded() ||
        !missing_result.error.has_value() ||
        missing_result.error->message.find("Missing subtitle image asset") == std::string::npos) {
        return fail("Missing subtitle img asset did not produce a clear failure.");
    }

    const auto unsafe_path = root / "unsafe.ass";
    write_text(unsafe_path, ass_with_text("{\\img(../evil.png)}Unsafe"));
    const auto unsafe_result = utsure::core::subtitles::load_subtitle_image_assets(unsafe_path);
    if (unsafe_result.succeeded() ||
        !unsafe_result.error.has_value() ||
        unsafe_result.error->message.find("Unsafe subtitle image asset path rejected") == std::string::npos) {
        return fail("Unsafe subtitle img asset path traversal was not rejected.");
    }

    std::cout << "subtitle_img.resolve_decode=ok\n";
    return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
    if (argc < 2) {
        return fail("Usage: utsure_core_subtitle_image_assets_tests [--scan|--resolve-decode <sample.png>]");
    }

    const std::string mode = argv[1];
    if (mode == "--scan") {
        return run_scan_assertions();
    }
    if (mode == "--resolve-decode") {
        if (argc != 3) {
            return fail("Usage: utsure_core_subtitle_image_assets_tests --resolve-decode <sample.png>");
        }
        return run_resolve_decode_assertions(argv[2]);
    }

    return fail("Unknown subtitle image assets test mode.");
}
