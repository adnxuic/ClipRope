#include "platform_win/focus_field.hpp"
#include "platform_win/process_image.hpp"
#include <string_view>

namespace cliprope::win {
using Microsoft::WRL::ComPtr;
namespace {
// A document or custom editor can expose a writable caret without ValuePattern.
enum class Selection { unavailable, writable, read_only };
Selection selection_access(IUIAutomationElement* field) {
    ComPtr<IUIAutomationTextPattern> text;
    ComPtr<IUIAutomationTextRangeArray> ranges;
    ComPtr<IUIAutomationTextRange> selected;
    int count{};
    if (FAILED(field->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&text))) || !text ||
        FAILED(text->GetSelection(&ranges)) || !ranges || FAILED(ranges->get_Length(&count)) || count != 1 ||
        FAILED(ranges->GetElement(0, &selected)) || !selected) return Selection::unavailable;
    VARIANT read_only{};
    const auto hr = selected->GetAttributeValue(UIA_IsReadOnlyAttributeId, &read_only);
    auto access = Selection::unavailable;
    if (SUCCEEDED(hr) && read_only.vt == VT_BOOL)
        access = read_only.boolVal ? Selection::read_only : Selection::writable;
    VariantClear(&read_only);
    return access;
}

bool editor_surface(CONTROLTYPEID type) {
    return type == UIA_EditControlTypeId || type == UIA_GroupControlTypeId ||
           type == UIA_CustomControlTypeId || type == UIA_DocumentControlTypeId ||
           type == UIA_PaneControlTypeId || type == UIA_WindowControlTypeId;
}
}

FocusField::Access FocusField::result(Access access, const char* reason) {
    reason_ = reason;
    return access;
}

FocusField::Access FocusField::assess(IUIAutomationElement* field) {
    if (!field) return result(Access::unknown, "native-focus-only");
    int pid{};
    if (FAILED(field->get_CurrentProcessId(&pid)) || static_cast<DWORD>(pid) != pid_)
        return result(Access::blocked, "foreign-or-stale-element");
    BOOL password{}, enabled{}, focus{};
    if (FAILED(field->get_CurrentControlType(&type_)) || FAILED(field->get_CurrentIsPassword(&password)) ||
        FAILED(field->get_CurrentIsEnabled(&enabled)) || FAILED(field->get_CurrentHasKeyboardFocus(&focus)))
        return result(Access::blocked, "unavailable-properties");
    if (password) return result(Access::blocked, "password");
    if (!enabled) return result(Access::blocked, "disabled");
    if (type_ == UIA_ButtonControlTypeId || type_ == UIA_MenuControlTypeId ||
        type_ == UIA_MenuItemControlTypeId || type_ == UIA_MenuBarControlTypeId)
        return result(Access::blocked, "command-control");
    ComPtr<IUIAutomationValuePattern> value;
    if (SUCCEEDED(field->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&value))) && value) {
        BOOL read_only{};
        if (FAILED(value->get_CurrentIsReadOnly(&read_only)) || read_only)
            return result(Access::blocked, "read-only-value");
        if (focus && type_ == UIA_EditControlTypeId) return result(Access::editor, "writable-edit");
    }
    const auto selection = selection_access(field);
    if (selection == Selection::read_only) return result(Access::blocked, "read-only-selection");
    if (focus && selection == Selection::writable) return result(Access::editor, "writable-selection");
    if (!editor_surface(type_)) return result(Access::blocked, "non-editor-control");
    if (focus && value && (type_ == UIA_GroupControlTypeId || type_ == UIA_CustomControlTypeId ||
                           type_ == UIA_DocumentControlTypeId))
        return result(Access::text_editor, "writable-surface");
    if (!focus && type_ == UIA_EditControlTypeId) return result(Access::blocked, "unfocused-edit");
    return result(Access::unknown, "opaque-surface");
}

IUIAutomation* FocusClient::for_window(HWND foreground, DWORD pid) {
    if (foreground == last_ && pid == pid_) return automation_.Get();
    last_ = foreground;
    pid_ = pid;
    automation_.Reset();
    if (!pid || pid == GetCurrentProcessId()) return nullptr;
    ComPtr<IUIAutomation2> client;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&client)))) return nullptr;
    client->put_ConnectionTimeout(350);
    client->put_TransactionTimeout(350);
    if (FAILED(client.As(&automation_))) return nullptr;
    return automation_.Get();
}

bool FocusField::bind(HWND foreground, DWORD pid, FocusClient& client) {
    foreground_ = foreground;
    pid_ = pid;
    type_ = 0;
    element_.Reset();
    automation_ = client.for_window(foreground, pid);
    if (automation_) automation_->GetFocusedElement(&element_);
    bound_ = assess(element_.Get());
    return bound_ != Access::blocked && GetForegroundWindow() == foreground_;
}

bool FocusField::accepts(PastePayload payload) const {
    return bound_ == Access::editor || (bound_ != Access::blocked && payload == PastePayload::text);
}

bool FocusField::revalidate(PastePayload payload) {
    if (!accepts(payload) || GetForegroundWindow() != foreground_) return false;
    FocusClient local;
    if (!automation_) automation_ = local.for_window(foreground_, pid_);
    ComPtr<IUIAutomationElement> now;
    if (automation_) automation_->GetFocusedElement(&now);
    if (element_) {
        BOOL same{};
        if (!now || FAILED(automation_->CompareElements(element_.Get(), now.Get(), &same)) || !same) {
            reason_ = "element-changed";
            return false;
        }
    }
    const auto current = assess(now.Get());
    // A known editor must still be verifiable. An opaque binding stays text-only.
    return current >= bound_ && GetForegroundWindow() == foreground_;
}

bool FocusField::allows_file_paste() const {
    // A browser's address bar is a writable UIA editor too. Only the page's
    // document subtree delegates attachment paste to web content.
    wchar_t host_class[64]{};
    if (!GetClassNameW(foreground_, host_class, static_cast<int>(std::size(host_class)))) return false;
    const std::wstring_view host(host_class);
    if (!host.starts_with(L"Chrome_WidgetWin_") && host != L"MozillaWindowClass")
        return true;
    CONTROLTYPEID type{};
    if (FAILED(element_->get_CurrentControlType(&type))) return false;
    if (type == UIA_DocumentControlTypeId) return true;
    VARIANT match{};
    match.vt = VT_I4;
    match.lVal = UIA_DocumentControlTypeId;
    ComPtr<IUIAutomationCondition> condition;
    ComPtr<IUIAutomationTreeWalker> documents;
    ComPtr<IUIAutomationElement> document;
    return SUCCEEDED(automation_->CreatePropertyCondition(UIA_ControlTypePropertyId, match, &condition)) &&
           SUCCEEDED(automation_->CreateTreeWalker(condition.Get(), &documents)) &&
           SUCCEEDED(documents->GetParentElement(element_.Get(), &document)) && document;
}

} // namespace cliprope::win
