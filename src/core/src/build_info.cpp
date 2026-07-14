#include "utsure/core/build_info.hpp"

#ifndef UTSURE_PROJECT_VERSION
#define UTSURE_PROJECT_VERSION "1.0"
#endif

namespace utsure::core {

std::string_view BuildInfo::project_name() noexcept {
    return "utsure";
}

std::string_view BuildInfo::project_version() noexcept {
    return UTSURE_PROJECT_VERSION;
}

std::string_view BuildInfo::project_state() noexcept {
    return "Utsure v1.0 Windows desktop video encoder.";
}

}  // namespace utsure::core
