#pragma once
#include "platform_win/fluent.hpp"

namespace cliprope::win::fluent {
// Raise the notification on the same provider that serves the viewport to automation clients.
void announce_in_viewport(HWND viewport, std::wstring_view action, const std::wstring& result);

// The owner supplies the authoritative range, in DIPs or document pixels.
class ScrollBar final {
  public:
    // A null viewport installs RangeValue only on the bar and preserves native
    // list/combobox Selection and ExpandCollapse providers.
    bool create(HWND parent, HWND viewport, const Palette& colors, bool panel, std::function<void(int)> scroll);
    HWND window() const { return window_; }
    void update(int position, int maximum, int page, int line);
    void wheel(WPARAM input);
    bool key(WPARAM key);
  private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    void paint(HDC dc) const;
    RECT thumb() const;
    void request(int position);
    HWND window_{};
    const Palette* colors_{};
    std::function<void(int)> scroll_;
    int position_{}, maximum_{}, page_{1}, line_{1}, grab_{-1}, wheel_remainder_{};
    bool panel_{}, hot_{};
};
} // namespace cliprope::win::fluent
