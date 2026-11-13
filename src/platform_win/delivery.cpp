#include "platform_win/delivery.hpp"
#include "platform_win/target_session.hpp"
#include "platform_win/office_view.hpp"
#include "platform_win/process_image.hpp"

#include <optional>
#ifdef CLIPROPE_DESKTOP_PROBES
#include "target_gate.hpp"
#endif
#include <string>
#include <vector>

namespace cliprope::win {
namespace {

bool native_edit(HWND field) {
    wchar_t name[32]{};
    return GetClassNameW(field, name, static_cast<int>(std::size(name))) &&
           (!_wcsicmp(name, L"Edit") || !_wcsnicmp(name, L"RichEdit", 8));
}

bool writable_edit(HWND field) {
    return IsWindowEnabled(field) && IsWindowVisible(field) &&
           !(GetWindowLongPtrW(field, GWL_STYLE) & (ES_READONLY | ES_PASSWORD));
}

std::optional<DWORD> integrity(HANDLE process) {
    HANDLE token{};
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return {};
    DWORD size{};
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    const bool ok = size && GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), size, &size);
    CloseHandle(token);
    if (!ok) return {};
    const PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer.data())->Label.Sid;
    if (!IsValidSid(sid) || !*GetSidSubAuthorityCount(sid)) return {};
    return *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
}

bool accessible(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    const auto target = integrity(process);
    CloseHandle(process);
    const auto own = integrity(GetCurrentProcess());
    return target && own && *target <= *own;
}

bool focused(HWND foreground, HWND field) {
    GUITHREADINFO gui{sizeof(gui)};
    return foreground && field && GetForegroundWindow() == foreground &&
           GetGUIThreadInfo(0, &gui) && !(gui.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE)) &&
           gui.hwndActive == foreground && gui.hwndFocus == field &&
           GetAncestor(field, GA_ROOT) == foreground;
}

bool available_window(HWND field) {
    return IsWindowEnabled(field) && IsWindowVisible(field);
}

bool keys_released() {
    for (int key : {VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN, int('V')})
        if (GetAsyncKeyState(key) & 0x8000) return false;
    return true;
}

} // namespace

DeliveryResult send_paste_shortcut(decltype(&SendInput) send) {
    INPUT keys[4]{};
    const WORD codes[]{VK_LCONTROL, 'V', 'V', VK_LCONTROL};
    for (size_t i = 0; i < std::size(keys); ++i) {
        keys[i].type = INPUT_KEYBOARD;
        keys[i].ki.wVk = codes[i];
        keys[i].ki.dwFlags = i >= 2 ? KEYEVENTF_KEYUP : 0;
    }
    const UINT sent = send(4, keys, sizeof(INPUT));
    if (sent == 4) return DeliveryResult::shortcut_sent;
    if (!sent) return DeliveryResult::send_failed;
    // Only release keys whose injected down event had no matching up.
    const UINT first_up = sent == 2 ? 2 : 3;
    send(4 - first_up, keys + first_up, sizeof(INPUT));
    return DeliveryResult::unconfirmed;
}

EditGesture::EditGesture() = default;
EditGesture::~EditGesture() = default;


void EditGesture::cancel() {
    canceled_ = true;
    folder_ = false;
    if (session_) session_->cancel();
}
void EditGesture::begin_bind(bool warm, std::shared_ptr<CardPressInput> input) {
    session_.reset();
    consumed_ = canceled_ = folder_ = verifying_ = native_ = binding_complete_ = provider_timeout_ = false;
    delivery_sequence_ = 0;
    deadline_ = restore_until_ = keys_until_ = 0;
    restore_status_ = "not-needed";
    field_ = surface_ = nullptr;
    file_status_ = "unbound";
    target_status_ = "no-native-focus";
    foreground_ = GetForegroundWindow();
#ifdef CLIPROPE_DESKTOP_PROBES
    if (!warm) {
        if (auto field = target_fixture::receiver()) {
            foreground_ = field;
            tid_ = GetWindowThreadProcessId(field, &pid_);
            // Supervisor fixtures post window messages without a physical card gesture.
            if (!target_fixture::synchronized_press()) input.reset();
            session_ = std::make_unique<TargetSession>(field, field, pid_, tid_, false, std::move(input));
            field_ = field;
            return;
        }
    }
#endif
    GUITHREADINFO gui{sizeof(gui)};
    if (!GetGUIThreadInfo(0, &gui) || !focused(foreground_, gui.hwndFocus)) return;
    tid_ = GetWindowThreadProcessId(gui.hwndFocus, &pid_);
    if (!tid_ || pid_ == GetCurrentProcessId() || window_process(foreground_) != pid_) return;
    if (!available_window(foreground_) || !available_window(gui.hwndFocus)) return;
    const auto app = process_basename(pid_);
    if (warm && _wcsicmp(app.c_str(), L"msedge.exe")) return;
    const auto office = office_app(app.c_str());
    const auto office_view = office_document_view(gui.hwndFocus, office);
    if (!warm && native_edit(gui.hwndFocus) && !office_view) {
        if (writable_edit(gui.hwndFocus)) { native_ = true; field_ = gui.hwndFocus; }
        target_status_ = field_ ? "native-edit" : "protected-native-edit";
        return;
    }
    wchar_t cls[64]{};
    GetClassNameW(gui.hwndFocus, cls, int(std::size(cls)));
    if (!_wcsicmp(cls, L"Button") || !_wcsicmp(cls, L"#32768") || !accessible(pid_) || IsHungAppWindow(foreground_)) return;
    try {
        // Explorer binds a Shell view under its own strict input policy.
        if (!_wcsicmp(app.c_str(), L"explorer.exe")) input.reset();
        session_ = std::make_unique<TargetSession>(foreground_, gui.hwndFocus, pid_, tid_, warm, std::move(input));
        field_ = gui.hwndFocus;
    } catch (const std::exception&) { target_status_ = "verification-unavailable"; }
}
bool EditGesture::binding_ready() {
    if (!session_ || binding_complete_) return true;
    auto kind = session_->binding();
    if (!kind) return false;
    binding_complete_ = true;
    target_status_ = session_->description();
    folder_ = !canceled_ && *kind == target::Kind::folder;
    if (folder_) file_status_ = "folder";
    if (*kind == target::Kind::none || *kind == target::Kind::warm) {
        provider_timeout_ = *kind == target::Kind::none && session_->expired();
        field_ = nullptr;
        session_.reset();
    }
    return true;
}
namespace {
// Only synchronous command-line/probe callers use this pump. Desktop uses its ordinary message loop.
bool wait_turn() {
    MsgWaitForMultipleObjectsEx(0, nullptr, 8, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    MSG msg{};
    for (unsigned i = 0; i < 32 && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE); ++i) {
        if (msg.message == WM_QUIT) { PostQuitMessage(int(msg.wParam)); return false; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}
}
void EditGesture::bind() {
    begin_bind();
    while (!binding_ready()) { if (!wait_turn()) { cancel(); break; } }
}
std::string EditGesture::target_status() const {
    return target_status_;
}
void EditGesture::restore_interrupted_focus() {
    if (canceled_ || !field_ || !surface_ || restore_until_ || GetForegroundWindow() != surface_) return;
    DWORD actual{};
    if (GetWindowThreadProcessId(field_, &actual) != tid_ || actual != pid_ ||
        window_process(foreground_) != pid_ || GetAncestor(field_, GA_ROOT) != foreground_) {
        restore_status_ = "stale"; cancel(); return;
    }
    const auto now = GetTickCount64();
    if (!deadline_) deadline_ = now + target::delivery_ms;
    restore_until_ = now + 350;
    if (!SetForegroundWindow(foreground_)) { restore_status_ = "refused"; cancel(); }
}
std::optional<DeliveryResult> EditGesture::poll_delivery(const std::function<bool()>& current, PastePayload payload) {
    if (consumed_) return DeliveryResult::consumed;
    auto end = [&](DeliveryResult result) {
        consumed_ = true;
        if (session_) {
            if (verifying_ && result == DeliveryResult::lost_target) provider_timeout_ = session_->expired();
            session_->cancel();
        }
        return result;
    };
    const auto now = GetTickCount64();
    if (!deadline_) deadline_ = now + target::delivery_ms;
    if (canceled_ || now >= deadline_) return end(DeliveryResult::lost_target);
    if (!binding_ready()) return {};
    if (!field_) return end(DeliveryResult::no_field);
    if (!current()) return end(DeliveryResult::replaced_publication);
    const auto sequence = GetClipboardSequenceNumber();
    if (!sequence || (delivery_sequence_ && sequence != delivery_sequence_)) return end(DeliveryResult::replaced_publication);
    delivery_sequence_ = sequence;
    if (!accessible(pid_)) return end(DeliveryResult::access_denied);
    restore_interrupted_focus();
    DWORD pid{};
    if (canceled_ || GetWindowThreadProcessId(field_, &pid) != tid_ || pid != pid_)
        return end(DeliveryResult::lost_target);
    bool original_focus = focused(foreground_, field_);
#ifdef CLIPROPE_DESKTOP_PROBES
    if (field_ == target_fixture::receiver()) original_focus = true;
#endif
    if (!original_focus) {
        const auto active = GetForegroundWindow();
        if (now < restore_until_ && (!active || active == foreground_ || active == surface_)) return {};
        return end(DeliveryResult::lost_target);
    }
    if (restore_until_) restore_status_ = "restored";
    if (!keys_released()) {
        if (!keys_until_) keys_until_ = now + 250;
        if (now < keys_until_) return {};
        return end(DeliveryResult::send_failed);
    }
    bool available = !IsHungAppWindow(foreground_) && available_window(field_) && available_window(foreground_);
#ifdef CLIPROPE_DESKTOP_PROBES
    if (field_ == target_fixture::receiver()) available = true; // A private station has no visible foreground.
#endif
    if (!available)
        return end(DeliveryResult::lost_target);
    if (session_) {
        if (!verifying_) {
            if (!session_->verify(payload, delivery_sequence_, deadline_)) return end(DeliveryResult::lost_target);
            verifying_ = true;
        }
        const auto verdict = session_->result();
        if (!verdict) return {};
        if (*verdict == target::Verdict::replaced) return end(DeliveryResult::replaced_publication);
        if (*verdict == target::Verdict::refused) return end(DeliveryResult::no_field);
        if (*verdict != target::Verdict::accepted) return end(DeliveryResult::lost_target);
    } else {
        if (!native_ || !writable_edit(field_) || !native_edit(field_)) return end(DeliveryResult::lost_target);
        if (payload == PastePayload::image_files) {
            wchar_t cls[32]{};
            if (!GetClassNameW(field_, cls, int(std::size(cls))) || _wcsnicmp(cls, L"RichEdit", 8) ||
                !_wcsicmp(process_basename(pid_).c_str(), L"notepad.exe")) return end(DeliveryResult::no_field);
        }
    }
    if (!current() || GetClipboardSequenceNumber() != delivery_sequence_) return end(DeliveryResult::replaced_publication);
    bool dispatchable = focused(foreground_, field_) && available_window(foreground_) && available_window(field_);
#ifdef CLIPROPE_DESKTOP_PROBES
    const bool fixture = field_ == target_fixture::receiver();
    if (fixture) dispatchable = true;
#endif
    if (canceled_ || consumed_ || GetTickCount64() >= deadline_ || !dispatchable ||
        GetWindowThreadProcessId(field_, &pid) != tid_ || pid != pid_ || window_process(foreground_) != pid_ ||
        !accessible(pid_) || !keys_released() ||
        (native_ && (!native_edit(field_) || !writable_edit(field_))))
        return end(DeliveryResult::lost_target);
    consumed_ = true;
    if (session_) session_->cancel();
#ifdef CLIPROPE_DESKTOP_PROBES
    if (fixture) {
        DWORD_PTR ignored{};
        return SendMessageTimeoutW(field_, WM_PASTE, 0, 0, SMTO_ABORTIFHUNG, 50, &ignored) ?
               DeliveryResult::shortcut_sent : DeliveryResult::send_failed;
    }
#endif
    return send_paste_shortcut();
}
DeliveryResult EditGesture::deliver(const std::function<bool()>& current, PastePayload payload) {
    for (;;) {
        if (auto result = poll_delivery(current, payload)) return *result;
        if (!wait_turn()) cancel();
    }
}
} // namespace cliprope::win
