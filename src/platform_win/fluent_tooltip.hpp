#pragma once
#include "platform_win/fluent.hpp"

namespace cliprope::win::fluent {
// One inert, owned popup. Binding callbacks run only on the owning UI thread.
class Tooltip final {
  public:
    Tooltip();
    ~Tooltip();
    Tooltip(const Tooltip&) = delete;
    Tooltip& operator=(const Tooltip&) = delete;
    void bind(HWND owner, HWND target, std::function<UiText()> text, int max_width = 320);
    void track(HWND owner, HWND target, UiText text, bool immediate = false,
               int max_width = 320, const RECT* client_area = nullptr);
    void hide();
    void clear();
    bool visible() const;
    HWND window() const;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace cliprope::win::fluent
