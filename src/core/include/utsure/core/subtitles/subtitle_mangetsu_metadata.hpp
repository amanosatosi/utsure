#pragma once

#include "utsure/core/subtitles/subtitle_renderer.hpp"

#include <filesystem>
#include <string_view>

namespace utsure::core::subtitles {

[[nodiscard]] MangetsuColorcodingMetadata scan_ass_mangetsu_colorcoding_metadata(
    std::string_view ass_text
);

[[nodiscard]] MangetsuColorcodingMetadata load_ass_mangetsu_colorcoding_metadata(
    const std::filesystem::path &subtitle_path
);

}  // namespace utsure::core::subtitles
