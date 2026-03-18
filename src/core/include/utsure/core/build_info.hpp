#pragma once

#include <string_view>

namespace utsure::core {

struct BuildInfo final {
    static std::string_view project_name() noexcept;
    static std::string_view project_version() noexcept;
    static std::string_view project_state() noexcept;
};

}  // namespace utsure::core
