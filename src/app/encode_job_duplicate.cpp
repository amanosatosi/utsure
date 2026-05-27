#include "encode_job_duplicate.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

std::filesystem::path qstring_to_path(const QString &text) {
#ifdef _WIN32
    return std::filesystem::path(text.toStdWString());
#else
    return std::filesystem::path(QFile::encodeName(text).constData());
#endif
}

QString path_to_qstring(const std::filesystem::path &path) {
#ifdef _WIN32
    return QDir::toNativeSeparators(QString::fromStdWString(path.native()));
#else
    const auto encoded = path.string();
    return QDir::toNativeSeparators(QFile::decodeName(encoded.c_str()));
#endif
}

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

std::string normalize_path_key(const std::filesystem::path &path) {
    if (path.empty()) {
        return {};
    }

    std::error_code error{};
    auto absolute_path = std::filesystem::absolute(path, error);
    if (error) {
        absolute_path = path;
    }

    std::string normalized = absolute_path.lexically_normal().generic_string();
#ifdef _WIN32
    normalized = lowercase_ascii(std::move(normalized));
#endif
    return normalized;
}

struct PathAvailability final {
    bool occupied{true};
    QString diagnostic{};
};

PathAvailability check_path_availability(const std::filesystem::path &path) {
    std::error_code error{};
    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        return PathAvailability{
            .occupied = true,
            .diagnostic = QString("Could not verify duplicate output path '%1': %2.")
                .arg(path_to_qstring(path), QString::fromStdString(error.message()))
        };
    }

    return PathAvailability{
        .occupied = exists,
        .diagnostic = {}
    };
}

struct ManualDuplicateOutputPathResult final {
    std::filesystem::path path{};
    QString diagnostic{};
};

std::filesystem::path copy_candidate_path(
    const std::filesystem::path &parent_path,
    const std::string &original_stem,
    const std::string &extension,
    const std::string &suffix
) {
    const std::string file_name = original_stem + suffix + extension;
    return parent_path.empty()
        ? std::filesystem::path(file_name)
        : (parent_path / file_name).lexically_normal();
}

ManualDuplicateOutputPathResult make_unique_manual_duplicate_output_path(const std::filesystem::path &original_path) {
    if (original_path.empty()) {
        return {};
    }

    const auto parent_path = original_path.parent_path();
    const std::string original_stem = original_path.stem().string().empty()
        ? original_path.filename().string()
        : original_path.stem().string();
    const std::string extension = original_path.has_extension() ? original_path.extension().string() : std::string{};
    const std::string original_key = normalize_path_key(original_path);
    QString diagnostic{};

    for (int copy_index = 1; copy_index < 1000; ++copy_index) {
        const std::string suffix = copy_index == 1
            ? " Copy"
            : " Copy " + std::to_string(copy_index);
        const std::filesystem::path candidate_path =
            copy_candidate_path(parent_path, original_stem, extension, suffix);
        const auto availability = check_path_availability(candidate_path);
        if (!availability.diagnostic.isEmpty() && diagnostic.isEmpty()) {
            diagnostic = availability.diagnostic;
        }
        if (normalize_path_key(candidate_path) == original_key || availability.occupied) {
            continue;
        }

        return ManualDuplicateOutputPathResult{
            .path = candidate_path,
            .diagnostic = std::move(diagnostic)
        };
    }

    const auto fallback_seed = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    for (int attempt = 0; attempt < 1000; ++attempt) {
        const std::string suffix = attempt == 0
            ? " Copy " + fallback_seed
            : " Copy " + fallback_seed + "-" + std::to_string(attempt);
        const std::filesystem::path candidate_path =
            copy_candidate_path(parent_path, original_stem, extension, suffix);
        const auto availability = check_path_availability(candidate_path);
        if (!availability.diagnostic.isEmpty() && diagnostic.isEmpty()) {
            diagnostic = availability.diagnostic;
        }
        if (normalize_path_key(candidate_path) == original_key || availability.occupied) {
            continue;
        }

        return ManualDuplicateOutputPathResult{
            .path = candidate_path,
            .diagnostic = std::move(diagnostic)
        };
    }

    return ManualDuplicateOutputPathResult{
        .path = {},
        .diagnostic = diagnostic.isEmpty()
            ? QString("Could not choose a safe non-colliding duplicate output path.")
            : diagnostic
    };
}

}  // namespace

DuplicateEncodeEntryResult duplicate_encode_entry(const DuplicateEncodeEntryRequest &request) {
    DuplicateEncodeEntryState duplicate = request.original;
    const QString fallback_source_name = QFileInfo(request.original.source_path).fileName();
    const QString base_source_name = request.original.source_name.trimmed().isEmpty()
        ? fallback_source_name
        : request.original.source_name.trimmed();
    duplicate.source_name = base_source_name.trimmed().isEmpty() ? "Copy" : base_source_name + " Copy";

    const std::filesystem::path original_output_path = qstring_to_path(request.original.output_path.trimmed());
    if (request.original.output_path_manual_override && !original_output_path.empty()) {
        const auto manual_output = make_unique_manual_duplicate_output_path(original_output_path);
        duplicate.output_path_manual_override = true;
        duplicate.output_path = path_to_qstring(manual_output.path);
        return DuplicateEncodeEntryResult{
            .duplicate = std::move(duplicate),
            .sequence_counter_key = {},
            .persisted_sequence_number = 0,
            .sequence_counter_reserved = false,
            .output_path_generation_failed = manual_output.path.empty(),
            .diagnostic = manual_output.path.empty()
                ? (manual_output.diagnostic.isEmpty()
                    ? QString("Could not choose a safe non-colliding duplicate output path.")
                    : manual_output.diagnostic)
                : manual_output.diagnostic
        };
    }

    duplicate.output_path_manual_override = false;
    duplicate.output_path.clear();
    const std::string counter_key = utsure::core::job::OutputNaming::sequence_counter_key(
        request.automatic_output_request,
        request.naming_template
    );
    const auto reservations = utsure::core::job::OutputNaming::reserve_batch(
        std::vector<utsure::core::job::OutputNamingReservationRequest>{
            utsure::core::job::OutputNamingReservationRequest{
                .request = request.automatic_output_request,
                .naming_template = request.naming_template,
                .stored_sequence_number = request.stored_sequence_number,
                .excluded_output_paths = original_output_path.empty()
                    ? std::vector<std::filesystem::path>{}
                    : std::vector<std::filesystem::path>{original_output_path}
            }
        }
    );

    if (!reservations.empty()) {
        const auto duplicate_output_path = reservations.front().result.output_path;
        const auto availability = check_path_availability(duplicate_output_path);
        const bool collides_with_original = !original_output_path.empty() &&
            normalize_path_key(duplicate_output_path) == normalize_path_key(original_output_path);
        if (availability.occupied || collides_with_original) {
            return DuplicateEncodeEntryResult{
                .duplicate = std::move(duplicate),
                .sequence_counter_key = reservations.front().sequence_counter_key,
                .persisted_sequence_number = 0,
                .sequence_counter_reserved = false,
                .output_path_generation_failed = true,
                .diagnostic = availability.diagnostic.isEmpty()
                    ? QString("Could not choose a safe non-colliding duplicate output path.")
                    : availability.diagnostic
            };
        }

        duplicate.output_path = path_to_qstring(duplicate_output_path);
        return DuplicateEncodeEntryResult{
            .duplicate = std::move(duplicate),
            .sequence_counter_key = reservations.front().sequence_counter_key,
            .persisted_sequence_number = reservations.front().persisted_sequence_number,
            .sequence_counter_reserved = reservations.front().assigned_sequence_number > 0,
            .output_path_generation_failed = false,
            .diagnostic = availability.diagnostic
        };
    }

    return DuplicateEncodeEntryResult{
        .duplicate = std::move(duplicate),
        .sequence_counter_key = counter_key,
        .persisted_sequence_number = 0,
        .sequence_counter_reserved = false,
        .output_path_generation_failed = false,
        .diagnostic = {}
    };
}
