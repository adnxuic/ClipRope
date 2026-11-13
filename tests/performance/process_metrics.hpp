#pragma once
#include <cstdint>
#include <pdh.h>
#include <vector>
#include <windows.h>

namespace performance {
struct Sample {
    std::uint64_t host_private{}, worker_private{}, working_set{}, cpu_ticks{}, handles{}, threads{}, gdi{}, user{};
    std::uint64_t host_working_set{}, worker_working_set{}, worker_peak_commit{};
    unsigned processes{};
    std::vector<DWORD> ids;
    std::uint64_t total_private() const { return host_private + worker_private; }
};
Sample process_tree(DWORD root);
struct GpuUsage {
    std::int64_t dedicated = -1, shared = -1, dwm_dedicated = -1, dwm_shared = -1, dwm_private = -1;
};
class GpuCounters {
  public:
    GpuCounters();
    ~GpuCounters();
    GpuUsage read(const std::vector<DWORD> &ids);

  private:
    PDH_HQUERY query_{};
    PDH_HCOUNTER dedicated_{}, shared_{}, dwm_process_{}, dwm_bytes_{};
    DWORD dwm_{};
};
} // namespace performance
