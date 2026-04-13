#include "utsure/core/media/audio_output.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>

namespace utsure::core::media {

namespace {

std::string lowercase_ascii(std::string value) {
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

std::string normalized_extension(const std::filesystem::path &path) {
    return lowercase_ascii(path.extension().string());
}

bool codec_name_equals(const std::string &codec_name, const std::string_view expected) {
    return lowercase_ascii(codec_name) == expected;
}

bool container_supports_safe_audio_copy(
    const std::string &extension,
    const std::string &codec_name
) {
    if (extension == ".mp4" || extension == ".m4v" || extension == ".m4a" || extension == ".mov") {
        return codec_name_equals(codec_name, "aac") || codec_name_equals(codec_name, "alac");
    }

    if (extension == ".mkv" || extension == ".mka") {
        return !codec_name.empty();
    }

    if (extension == ".webm") {
        return codec_name_equals(codec_name, "opus") || codec_name_equals(codec_name, "vorbis");
    }

    return false;
}

Rational sample_time_base(const int sample_rate_hz) {
    return Rational{
        .numerator = 1,
        .denominator = sample_rate_hz
    };
}

std::string default_channel_layout_name(const int channel_count) {
    switch (channel_count) {
    case 1:
        return "mono";
    case 2:
        return "stereo";
    case 6:
        return "5.1";
    default:
        return channel_count > 0 ? std::to_string(channel_count) + "ch" : "unknown";
    }
}

std::string describe_source_audio(const AudioStreamInfo &audio_stream) {
    std::ostringstream description;
    description << audio_stream.codec_name << ' ' << audio_stream.channel_count << "ch "
                << audio_stream.sample_rate << " Hz";
    return description.str();
}

std::string describe_encoded_audio(
    const ResolvedAudioOutputPlan &plan,
    const bool include_requested_mode
) {
    std::ostringstream description;
    description << "AAC " << plan.bitrate_kbps << "k";
    if (plan.channel_count > 0 && plan.sample_rate_hz > 0) {
        description << ' ' << plan.channel_count << "ch " << plan.sample_rate_hz << " Hz";
    }
    if (include_requested_mode) {
        description << " (" << to_string(plan.requested_mode) << ')';
    }
    return description.str();
}

std::string describe_copied_audio(
    const ResolvedAudioOutputPlan &plan,
    const bool include_requested_mode
) {
    std::ostringstream description;
    description << "Copy source";
    if (!plan.output_codec_name.empty()) {
        description << ' ' << plan.output_codec_name;
    }
    if (plan.channel_count > 0 && plan.sample_rate_hz > 0) {
        description << ' ' << plan.channel_count << "ch " << plan.sample_rate_hz << " Hz";
    }
    if (include_requested_mode) {
        description << " (" << to_string(plan.requested_mode) << ')';
    }
    return description.str();
}

}  // namespace

ResolvedAudioOutputPlan resolve_audio_output_plan(const AudioOutputResolveRequest &request) {
    ResolvedAudioOutputPlan plan{
        .requested_mode = request.settings.mode,
        .source_audio_present = request.main_source_audio_stream != nullptr,
        .output_present = false
    };

    if (request.main_source_audio_stream != nullptr) {
        plan.source_codec_name = request.main_source_audio_stream->codec_name;
    }

    if (request.settings.mode == AudioOutputMode::disable || request.main_source_audio_stream == nullptr) {
        plan.resolved_mode = ResolvedAudioOutputMode::disabled;
        plan.output_present = false;
        plan.decision_summary = request.main_source_audio_stream == nullptr
            ? "No source audio"
            : "Audio disabled";
        return plan;
    }

    const auto &source_audio = *request.main_source_audio_stream;
    const auto extension = normalized_extension(request.output_path);
    const bool copy_is_safe = request.segment_count == 1U &&
        !request.main_source_trimmed &&
        !request.settings.sample_rate_hz.has_value() &&
        !request.settings.channel_count.has_value() &&
        container_supports_safe_audio_copy(extension, source_audio.codec_name);
    plan.copy_is_safe = copy_is_safe;

    const auto output_sample_rate = request.settings.sample_rate_hz.value_or(source_audio.sample_rate);
    const auto output_channel_count = request.settings.channel_count.value_or(source_audio.channel_count);
    const auto output_channel_layout_name = request.settings.channel_count.has_value()
        ? default_channel_layout_name(output_channel_count)
        : source_audio.channel_layout_name;

    const auto build_encoded_plan = [&]() {
        plan.resolved_mode = ResolvedAudioOutputMode::encode_aac;
        plan.output_present = true;
        plan.output_codec_name = "aac";
        plan.bitrate_kbps = request.settings.bitrate_kbps;
        plan.sample_rate_hz = output_sample_rate;
        plan.channel_count = output_channel_count;
        plan.channel_layout_name = output_channel_layout_name;
        plan.time_base = sample_time_base(output_sample_rate);
        plan.decision_summary = describe_encoded_audio(plan, request.settings.mode == AudioOutputMode::auto_select);
    };

    if (request.settings.mode == AudioOutputMode::copy_source) {
        if (!copy_is_safe) {
            if (request.segment_count != 1U) {
                plan.requested_copy_blocker =
                    "Audio copy is only supported for single-segment jobs in the current streaming pipeline.";
            } else if (request.main_source_trimmed) {
                plan.requested_copy_blocker =
                    "Audio copy is not supported when the main source is trimmed in the current streaming pipeline.";
            } else if (request.settings.sample_rate_hz.has_value() || request.settings.channel_count.has_value()) {
                plan.requested_copy_blocker =
                    "Audio copy must keep the source sample rate and channel layout unchanged.";
            } else {
                plan.requested_copy_blocker =
                    "The selected output container is not a safe fit for copying the source audio codec '" +
                    source_audio.codec_name + "'.";
            }
            plan.decision_summary = "Copy blocked";
            return plan;
        }

        plan.resolved_mode = ResolvedAudioOutputMode::copy_source;
        plan.output_present = true;
        plan.output_codec_name = source_audio.codec_name;
        plan.sample_rate_hz = source_audio.sample_rate;
        plan.channel_count = source_audio.channel_count;
        plan.channel_layout_name = source_audio.channel_layout_name;
        plan.time_base = source_audio.timestamps.time_base;
        plan.decision_summary = describe_copied_audio(plan, false);
        return plan;
    }

    if (request.settings.mode == AudioOutputMode::encode_aac) {
        build_encoded_plan();
        return plan;
    }

    if (copy_is_safe) {
        plan.resolved_mode = ResolvedAudioOutputMode::copy_source;
        plan.output_present = true;
        plan.output_codec_name = source_audio.codec_name;
        plan.sample_rate_hz = source_audio.sample_rate;
        plan.channel_count = source_audio.channel_count;
        plan.channel_layout_name = source_audio.channel_layout_name;
        plan.time_base = source_audio.timestamps.time_base;
        plan.decision_summary = describe_copied_audio(plan, true);
        return plan;
    }

    build_encoded_plan();
    return plan;
}

std::string format_source_audio_summary(const std::optional<AudioStreamInfo> &audio_stream) {
    if (!audio_stream.has_value()) {
        return "None";
    }

    return describe_source_audio(*audio_stream);
}

std::string format_resolved_audio_output_summary(const ResolvedAudioOutputPlan &plan) {
    if (plan.requested_copy_blocker.has_value()) {
        return plan.decision_summary;
    }

    switch (plan.resolved_mode) {
    case ResolvedAudioOutputMode::copy_source:
        return describe_copied_audio(plan, plan.requested_mode == AudioOutputMode::auto_select);
    case ResolvedAudioOutputMode::encode_aac:
        return describe_encoded_audio(plan, plan.requested_mode == AudioOutputMode::auto_select);
    case ResolvedAudioOutputMode::disabled:
    default:
        return plan.source_audio_present ? "Disabled" : "None";
    }
}

const char *to_string(AudioOutputMode mode) noexcept {
    switch (mode) {
    case AudioOutputMode::auto_select:
        return "Auto";
    case AudioOutputMode::copy_source:
        return "Copy";
    case AudioOutputMode::encode_aac:
        return "AAC";
    case AudioOutputMode::disable:
        return "Disable";
    default:
        return "Unknown";
    }
}

const char *to_string(OutputAudioCodec codec) noexcept {
    switch (codec) {
    case OutputAudioCodec::aac:
        return "aac";
    default:
        return "unknown";
    }
}

const char *to_string(ResolvedAudioOutputMode mode) noexcept {
    switch (mode) {
    case ResolvedAudioOutputMode::disabled:
        return "disabled";
    case ResolvedAudioOutputMode::copy_source:
        return "copy_source";
    case ResolvedAudioOutputMode::encode_aac:
        return "encode_aac";
    default:
        return "unknown";
    }
}

}  // namespace utsure::core::media
