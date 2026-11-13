#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>

namespace cliprope::win {
inline RECT rope_placement(RECT work, UINT dpi, float row_height, double top_dip, bool overview) {
    const auto pixels = [dpi](double value) { return LONG(std::lround(value * dpi / 96.)); };
    const LONG available = std::max<LONG>(1, work.bottom - work.top);
    const LONG height = overview ? std::max<LONG>(1, available - pixels(48)) :
                                   std::min(available, pixels(row_height));
    const double max_top = (available - height) * 96. / dpi;
    const LONG top = work.top + pixels(std::clamp(overview ? 24. : top_dip, 0., max_top));
    return {work.left, top, work.right, top + height};
}
}
