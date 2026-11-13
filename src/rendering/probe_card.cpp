#include "probe_card.hpp"

namespace cliprope::rendering {

void paint_probe_card(HDC dc, RECT card) {
    SetDCBrushColor(dc, probe_card_color);
    FillRect(dc, &card, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, L"ClipRope card", -1, &card, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
}

}
