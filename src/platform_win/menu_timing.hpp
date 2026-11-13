#pragma once
#include <array>
#include <windows.h>

namespace cliprope::win {
// QPC timestamps on the owning UI thread. The benchmark fences DWM on demand.
enum class MenuStage { input, focus, model, controls, frame, painted, count };
inline constexpr UINT menu_timing_query = WM_APP + 248;
inline thread_local std::array<LARGE_INTEGER, static_cast<size_t>(MenuStage::count)> menu_times{};
inline void menu_mark(MenuStage stage) {
    if (stage == MenuStage::input) menu_times.fill({});
    QueryPerformanceCounter(&menu_times[static_cast<size_t>(stage)]);
}
} // namespace cliprope::win
