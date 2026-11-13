#pragma once
#include "platform_win/delivery.hpp"
#include <ole2.h>
#include <uiautomation.h>
#include <wrl/client.h>

namespace cliprope::win {
class FocusClient {
public:
    IUIAutomation* for_window(HWND foreground, DWORD pid);
private:
    HWND last_{};
    DWORD pid_{};
    Microsoft::WRL::ComPtr<IUIAutomation> automation_;
};
// Bind the focused UIA element even when multiple editors share an HWND.
class FocusField {
public:
    bool bind(HWND foreground, DWORD pid, FocusClient& client);
    bool accepts(PastePayload payload) const;
    bool revalidate(PastePayload payload);
    bool allows_file_paste() const;
    const char* reason() const { return reason_; }
    CONTROLTYPEID control_type() const { return type_; }
private:
    // Revalidation requires at least the original strength. Only editor accepts attachments.
    enum class Access { blocked, unknown, text_editor, editor };
    Access assess(IUIAutomationElement* field);
    Access result(Access access, const char* reason);
    HWND foreground_{};
    DWORD pid_{};
    CONTROLTYPEID type_{};
    Access bound_{Access::blocked};
    const char* reason_{"unbound"};
    Microsoft::WRL::ComPtr<IUIAutomation> automation_;
    Microsoft::WRL::ComPtr<IUIAutomationElement> element_;
};
}
