#pragma once

#include "utsure/core/media/media_info.hpp"

#include <string>

namespace utsure::core::media {

[[nodiscard]] std::string format_media_inspection_report(const MediaSourceInfo &media_source_info);

}  // namespace utsure::core::media
