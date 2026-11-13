#pragma once

#include <windows.h>

namespace cliprope::rendering {
inline constexpr COLORREF probe_card_color = RGB(35, 90, 122);
void paint_probe_card(HDC dc, RECT card);
}
