#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace utsure::core::process {

struct ExternalToolRunRequest final {
    std::filesystem::path executable{};
    std::vector<std::string> arguments{};
    std::function<bool()> cancellation_requested{};
};

struct ExternalToolRunResult final {
    bool launched{false};
    int exit_code{-1};
    std::string failure_message{};
    std::string combined_output{};

    [[nodiscard]] bool succeeded() const noexcept;
};

[[nodiscard]] std::optional<std::filesystem::path> find_executable_on_path(
    const std::vector<std::string> &candidate_names
);

[[nodiscard]] ExternalToolRunResult run_external_tool(const ExternalToolRunRequest &request) noexcept;

}  // namespace utsure::core::process
