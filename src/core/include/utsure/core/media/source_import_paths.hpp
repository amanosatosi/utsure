#pragma once

#include <filesystem>
#include <vector>

namespace utsure::core::media {

struct SourceImportPathExpansionResult final {
    std::vector<std::filesystem::path> accepted_source_paths{};
};

class SourceImportPaths final {
public:
    [[nodiscard]] static bool is_supported_video_file(const std::filesystem::path &path);
    [[nodiscard]] static bool is_supported_drop_candidate(const std::filesystem::path &path);
    [[nodiscard]] static SourceImportPathExpansionResult expand_drop_candidates(
        const std::vector<std::filesystem::path> &paths
    );
};

}  // namespace utsure::core::media
