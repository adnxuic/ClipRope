#pragma once
#include <filesystem>
#include <string_view>

namespace cliprope::win {
void export_diagnostics(const std::filesystem::path &profile, std::string_view report);
}
