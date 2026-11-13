#pragma once
#include "persistence/database.hpp"
#include <memory>
#include <vector>

namespace cliprope::persistence {
class LocationError final : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
// One normal desktop session owns the locator. Explicit profiles bypass it.
class ProfileLocation final {
  public:
    explicit ProfileLocation(const std::filesystem::path& local_app_data);
    // Read-only setup preflight, including pending moves and unfinished cleanup.
    static std::vector<std::filesystem::path> reserved_directories(const std::filesystem::path& local_app_data);
    ~ProfileLocation();
    ProfileLocation(const ProfileLocation&) = delete;
    std::filesystem::path active() const;
    std::filesystem::path pending() const;
    void schedule(const std::filesystem::path& target);
    void resume(const Database::Fault& fault = {});
    void purge(); // Final operation. Removes the locator as well as the installed profile.
  private:
    struct State;
    std::unique_ptr<State> state_;
};

std::uint64_t profile_disk_usage(const std::filesystem::path& root);
} // namespace cliprope::persistence
