#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace utsure::core::subtitles::runtime {

enum class SubtitleBitmapTransferMode : std::uint8_t {
    copied = 0,
    direct
};

enum class SubtitleCompositionMode : std::uint8_t {
    serialized = 0
};

enum class SubtitleDiagnosticsMode : std::uint8_t {
    off = 0,
    frame,
    verbose
};

struct SubtitleRuntimeOptions final {
    SubtitleBitmapTransferMode bitmap_transfer_mode{SubtitleBitmapTransferMode::copied};
    SubtitleCompositionMode composition_mode{SubtitleCompositionMode::serialized};
    SubtitleDiagnosticsMode diagnostics_mode{SubtitleDiagnosticsMode::off};
};

inline std::string lowercase_ascii(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        }
    );
    return value;
}

inline std::optional<std::string> read_environment_variable(const char *name) {
    const char *value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return std::nullopt;
    }

    return lowercase_ascii(std::string(value));
}

inline bool environment_flag_enabled(const char *name) noexcept {
    const auto value = read_environment_variable(name);
    if (!value.has_value()) {
        return false;
    }

    return *value == "1" || *value == "true" || *value == "on" || *value == "yes";
}

inline bool copied_bitmap_mode_forced() noexcept {
    return environment_flag_enabled("UTSURE_SUBTITLE_SAFE_MODE") ||
        environment_flag_enabled("UTSURE_SUBTITLE_SYNC") ||
        environment_flag_enabled("UTSURE_SUBTITLE_BITMAP_COPY") ||
        environment_flag_enabled("UTSURE_DISABLE_DIRECT_SUBTITLE_BITMAPS");
}

inline bool global_libass_lock_enabled() noexcept {
    return environment_flag_enabled("UTSURE_SUBTITLE_SAFE_MODE") ||
        environment_flag_enabled("UTSURE_SUBTITLE_SYNC") ||
        environment_flag_enabled("UTSURE_LIBASS_GLOBAL_LOCK");
}

inline bool strict_same_thread_lifetime_enabled() noexcept {
    return environment_flag_enabled("UTSURE_SUBTITLE_STRICT_SAME_THREAD") ||
        environment_flag_enabled("UTSURE_SUBTITLE_SAFE_MODE");
}

inline SubtitleBitmapTransferMode resolve_bitmap_transfer_mode() noexcept {
    if (copied_bitmap_mode_forced()) {
        return SubtitleBitmapTransferMode::copied;
    }

    const auto value = read_environment_variable("UTSURE_SUBTITLE_BITMAP_MODE");
    if (!value.has_value()) {
        return SubtitleBitmapTransferMode::copied;
    }

    if (*value == "direct" || *value == "raw") {
        return SubtitleBitmapTransferMode::direct;
    }

    if (*value == "copied" || *value == "copy" || *value == "isolated") {
        return SubtitleBitmapTransferMode::copied;
    }

    return SubtitleBitmapTransferMode::copied;
}

inline SubtitleCompositionMode resolve_composition_mode() noexcept {
    if (environment_flag_enabled("UTSURE_SUBTITLE_SAFE_MODE") ||
        environment_flag_enabled("UTSURE_SUBTITLE_SYNC")) {
        return SubtitleCompositionMode::serialized;
    }

    const auto value = read_environment_variable("UTSURE_SUBTITLE_COMPOSITION_MODE");
    if (!value.has_value()) {
        return SubtitleCompositionMode::serialized;
    }

    return SubtitleCompositionMode::serialized;
}

inline SubtitleDiagnosticsMode resolve_diagnostics_mode() noexcept {
    const auto value = read_environment_variable("UTSURE_SUBTITLE_DIAGNOSTICS");
    if (!value.has_value()) {
        return SubtitleDiagnosticsMode::off;
    }

    if (*value == "1" || *value == "frame" || *value == "summary" || *value == "on" || *value == "true") {
        return SubtitleDiagnosticsMode::frame;
    }

    if (*value == "2" || *value == "verbose" || *value == "bitmap" || *value == "bitmaps") {
        return SubtitleDiagnosticsMode::verbose;
    }

    return SubtitleDiagnosticsMode::off;
}

inline SubtitleRuntimeOptions resolve_subtitle_runtime_options() noexcept {
    return SubtitleRuntimeOptions{
        .bitmap_transfer_mode = resolve_bitmap_transfer_mode(),
        .composition_mode = resolve_composition_mode(),
        .diagnostics_mode = resolve_diagnostics_mode()
    };
}

inline const char *to_string(const SubtitleBitmapTransferMode mode) noexcept {
    switch (mode) {
    case SubtitleBitmapTransferMode::copied:
        return "copied";
    case SubtitleBitmapTransferMode::direct:
        return "direct";
    default:
        return "unknown";
    }
}

inline const char *to_string(const SubtitleCompositionMode mode) noexcept {
    switch (mode) {
    case SubtitleCompositionMode::serialized:
        return "serialized";
    default:
        return "unknown";
    }
}

inline const char *to_string(const SubtitleDiagnosticsMode mode) noexcept {
    switch (mode) {
    case SubtitleDiagnosticsMode::off:
        return "off";
    case SubtitleDiagnosticsMode::frame:
        return "frame";
    case SubtitleDiagnosticsMode::verbose:
        return "verbose";
    default:
        return "unknown";
    }
}

inline bool should_log_frame_details(const SubtitleDiagnosticsMode mode) noexcept {
    return mode != SubtitleDiagnosticsMode::off;
}

inline bool should_log_bitmap_details(const SubtitleDiagnosticsMode mode) noexcept {
    return mode == SubtitleDiagnosticsMode::verbose;
}

}  // namespace utsure::core::subtitles::runtime
