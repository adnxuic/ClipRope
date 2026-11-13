#pragma once
#include "preview_worker/protocol.hpp"
#include <filesystem>
#include <span>
#include <source_location>

namespace cliprope::preview_worker {
class SandboxFailure : public std::runtime_error {
  public:
    explicit SandboxFailure(DWORD code = GetLastError(), unsigned line = 0)
        : std::runtime_error("Screenshot sandbox unavailable: code=" + std::to_string(code) +
                             " stage=" + std::to_string(line)), native_code(code) {}
    DWORD native_code;
};
class ShutdownFailure : public std::runtime_error {
  public:
    ShutdownFailure(DWORD terminate, DWORD wait, bool confirmed)
        : std::runtime_error("Preview shutdown: terminate=" + std::to_string(terminate) +
              " wait=" + std::to_string(wait) + " confirmed=" + std::to_string(confirmed)),
          termination_error(terminate), wait_error(wait), confirmed_exit(confirmed) {}
    DWORD termination_error, wait_error;
    bool confirmed_exit;
};
#ifdef CLIPROPE_PREVIEW_FAULTS
struct StopProbe {
    Handle keep_job, child;
    std::stop_source cancel;
    bool deny_termination{}, fail_wait{};
};
// Set only on the thread calling the qualification decoder.
void observe_stop(StopProbe*);
bool holding_late_child();
#endif
// One ticket per process: termination is the synchronization boundary for immutable output.
domain::ImagePreview decode(const Request &, std::span<const unsigned char>, std::stop_token,
                            application::PreviewDeadline,
                            const std::filesystem::path &executable = {});
} // namespace cliprope::preview_worker
