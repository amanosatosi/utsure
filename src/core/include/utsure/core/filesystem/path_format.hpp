#pragma once

#include <filesystem>
#include <string>

namespace utsure::core::filesystem {

[[nodiscard]] inline std::string path_to_utf8_string(const std::filesystem::path &path) {
#if defined(_WIN32)
    const auto text = path.lexically_normal().u8string();
    return std::string(reinterpret_cast<const char *>(text.c_str()), text.size());
#else
    return path.lexically_normal().string();
#endif
}

[[nodiscard]] inline std::string path_component_to_utf8_string(const std::filesystem::path &path) {
#if defined(_WIN32)
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char *>(text.c_str()), text.size());
#else
    return path.string();
#endif
}

}  // namespace utsure::core::filesystem
