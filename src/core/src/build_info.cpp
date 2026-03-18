#include "utsure/core/build_info.hpp"

#ifndef UTSURE_PROJECT_VERSION
#define UTSURE_PROJECT_VERSION "0.1.0"
#endif

namespace utsure::core {

std::string_view BuildInfo::project_name() noexcept {
    return "utsure";
}

std::string_view BuildInfo::project_version() noexcept {
    return UTSURE_PROJECT_VERSION;
}

std::string_view BuildInfo::project_state() noexcept {
    return "Project skeleton only; media pipeline, subtitle burn-in, and encoding are not implemented yet.";
}

}  // namespace utsure::core
