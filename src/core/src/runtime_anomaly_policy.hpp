#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace utsure::core::runtime_policy {

enum class RuntimeAnomalyClass : std::uint8_t {
    harmless_noop = 0,
    recoverable_normalization,
    reduced_fidelity,
    unsupported_early,
    unsafe_or_corrupt
};

[[nodiscard]] inline const char *to_string(const RuntimeAnomalyClass classification) noexcept {
    switch (classification) {
    case RuntimeAnomalyClass::harmless_noop:
        return "harmless_noop";
    case RuntimeAnomalyClass::recoverable_normalization:
        return "recoverable_normalization";
    case RuntimeAnomalyClass::reduced_fidelity:
        return "reduced_fidelity";
    case RuntimeAnomalyClass::unsupported_early:
        return "unsupported_early";
    case RuntimeAnomalyClass::unsafe_or_corrupt:
    default:
        return "unsafe_or_corrupt";
    }
}

[[nodiscard]] inline bool can_continue(const RuntimeAnomalyClass classification) noexcept {
    switch (classification) {
    case RuntimeAnomalyClass::harmless_noop:
    case RuntimeAnomalyClass::recoverable_normalization:
    case RuntimeAnomalyClass::reduced_fidelity:
        return true;
    case RuntimeAnomalyClass::unsupported_early:
    case RuntimeAnomalyClass::unsafe_or_corrupt:
    default:
        return false;
    }
}

[[nodiscard]] inline std::string format_operation_message(
    const RuntimeAnomalyClass classification,
    const std::string_view operation,
    const std::string_view detail = {}
) {
    if (detail.starts_with("Harmless no-op skipped; ") ||
        detail.starts_with("Recoverable input normalized; ") ||
        detail.starts_with("Reduced-fidelity fallback applied; ") ||
        detail.starts_with("Unsupported setup detected; ") ||
        detail.starts_with("Unsafe or corrupt runtime state detected; ")) {
        return std::string(detail);
    }

    std::string message{};
    switch (classification) {
    case RuntimeAnomalyClass::harmless_noop:
        message = "Harmless no-op skipped; ";
        break;
    case RuntimeAnomalyClass::recoverable_normalization:
        message = "Recoverable input normalized; ";
        break;
    case RuntimeAnomalyClass::reduced_fidelity:
        message = "Reduced-fidelity fallback applied; ";
        break;
    case RuntimeAnomalyClass::unsupported_early:
        message = "Unsupported setup detected; ";
        break;
    case RuntimeAnomalyClass::unsafe_or_corrupt:
    default:
        message = "Unsafe or corrupt runtime state detected; ";
        break;
    }

    message += std::string(operation);
    message += can_continue(classification) ? " continues" : " cannot continue";
    if (!detail.empty()) {
        message += ": ";
        message += detail;
    } else {
        message += '.';
    }

    return message;
}

class RuntimeAnomalyError : public std::runtime_error {
public:
    RuntimeAnomalyError(RuntimeAnomalyClass classification, std::string detail)
        : std::runtime_error(std::move(detail)),
          classification_(classification) {
    }

    [[nodiscard]] RuntimeAnomalyClass classification() const noexcept {
        return classification_;
    }

private:
    RuntimeAnomalyClass classification_{RuntimeAnomalyClass::unsafe_or_corrupt};
};

}  // namespace utsure::core::runtime_policy
