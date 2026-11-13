#include "../clipboard_sources/external_copy.hpp"
#include "../integration/history_fixture.hpp"
#include "target_gate.hpp"
#include "platform_win/delivery.hpp"
#include "platform_win/card_press_input.hpp"
#include "platform_win/target_session.hpp"
#include <cstring>

namespace {
using namespace cliprope::win;
using probe::require;
constexpr wchar_t sample[] = L"\u6837\u4f8b";
unsigned inputs{};
std::wstring received;
DWORD fixture_tick{};
bool host_sampled_clock{};
HWND selected_edit{}, other_edit{};
unsigned selected_pastes{}, other_pastes{};
WNDPROC edit_proc{};
LRESULT CALLBACK count_pastes(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_PASTE) {
        if (window == selected_edit) ++selected_pastes;
        if (window == other_edit) ++other_pastes;
    }
    return CallWindowProcW(edit_proc, window, message, w, l);
}
std::wstring field_text(HWND field) {
    wchar_t value[128]{};
    GetWindowTextW(field, value, int(std::size(value)));
    return value;
}
BOOL WINAPI read_fixture_input(PLASTINPUTINFO info) {
    host_sampled_clock = true;
    info->dwTime = fixture_tick;
    return TRUE;
}

LRESULT CALLBACK recipient(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_PASTE) {
        ++inputs;
        if (selected_edit) return SendMessageW(selected_edit, message, w, l);
        if (OpenClipboard(window)) {
            HANDLE handle = GetClipboardData(CF_UNICODETEXT);
            if (auto* text = handle ? static_cast<const wchar_t*>(GlobalLock(handle)) : nullptr) {
                received = text;
                GlobalUnlock(handle);
            }
            CloseClipboard();
        }
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
void publish(HWND window) {
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(sample));
    auto* bytes = memory ? GlobalLock(memory) : nullptr;
    require(bytes != nullptr, "No test memory");
    std::memcpy(bytes, sample, sizeof(sample));
    GlobalUnlock(memory);
    require(OpenClipboard(window), "Could not open private clipboard");
    bool ok = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
    CloseClipboard();
    if (!ok) GlobalFree(memory);
    require(ok, "Could not publish receiver sample");
}
void exercise(const wchar_t* point, DeliveryResult expected) {
    require(SetEnvironmentVariableW(L"CLIPROPE_TEST_TARGET_HOLD", point), "Fixture environment unavailable");
    HWND window = CreateWindowW(L"ClipRope.TargetFixture", L"", WS_POPUP | WS_VISIBLE, 0, 0, 50, 50,
                                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(window != nullptr, "No private receiver window");
    struct Close {
        HWND hwnd;
        ~Close() { if (IsWindow(hwnd)) DestroyWindow(hwnd); probe::clear_private_sample(sample); }
    } close{window};
    inputs = 0;
    received.clear();
    require(SetPropW(window, target_fixture::selection, reinterpret_cast<HANDLE>(1)), "No original field");
    EditGesture gesture;
    gesture.bind();
    const bool missing = expected == DeliveryResult::no_field;
    require((gesture.target_status() == "unbound") == missing, "Unexpected binding outcome");
    publish(window);
    const DWORD receipt = GetClipboardSequenceNumber();
    const std::wstring stage(point);
    if (stage.ends_with(L"-changed"))
        require(SetPropW(window, target_fixture::selection, reinterpret_cast<HANDLE>(2)), "Could not change field");
    if (stage.ends_with(L"-destroyed")) DestroyWindow(window);
    if (stage.ends_with(L"-replaced")) publish(window);
    if (stage.ends_with(L"-cancelled")) gesture.cancel();
    auto current = [&] { return receipt == GetClipboardSequenceNumber(); };
    const auto result = gesture.deliver(current, PastePayload::text);
    if (result != expected) std::wcerr << point << L" expected=" << int(expected) << L" actual=" << int(result) << L"\n";
    require(result == expected, "Unexpected final delivery");
    require(gesture.deliver(current, PastePayload::text) == DeliveryResult::consumed, "Duplicate dispatch");
    const bool sent = expected == DeliveryResult::shortcut_sent;
    require(inputs == unsigned(sent) && (sent ? received == sample : received.empty()), "Wrong receiver or contents");
    require(probe::clear_private_sample(sample), "Manual paste sample unavailable");
    std::wcout << point << L" received=" << inputs << L'\n';
}
enum class InputChange { local, pending_hook, keyboard, outside_click, unwitnessed, later_local, missing_hooks };
void uia_press(const wchar_t* stage, InputChange change) {
    require(SetEnvironmentVariableW(L"CLIPROPE_TEST_TARGET_HOLD", stage), "No synchronized UIA fixture");
    HWND field = CreateWindowW(L"ClipRope.TargetFixture", L"", WS_POPUP, 0, 0, 50, 50,
                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(field != nullptr, "Missing UIA receiver");
    struct Close {
        HWND field;
        ~Close() { DestroyWindow(field); selected_edit = other_edit = nullptr; probe::clear_private_sample(sample); }
    } close{field};
    auto edit = [&](const wchar_t* value) {
        return CreateWindowW(L"EDIT", value, WS_CHILD | ES_AUTOHSCROLL, 0, 0, 100, 30,
                             field, nullptr, GetModuleHandleW(nullptr), nullptr);
    };
    selected_edit = edit(L"\u5de6X\u53f3");
    other_edit = edit(L"other");
    require(selected_edit && other_edit, "Missing independent receiver fields");
    edit_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(selected_edit, GWLP_WNDPROC, LONG_PTR(count_pastes)));
    require(edit_proc && SetWindowLongPtrW(other_edit, GWLP_WNDPROC, LONG_PTR(count_pastes)), "Cannot count receiver pastes");
    SendMessageW(selected_edit, EM_SETSEL, 1, 2);
    selected_pastes = other_pastes = 0;
    target::Handle ready(CreateEventW(nullptr, FALSE, FALSE, target_fixture::press_event(field, L"ready").c_str()));
    target::Handle resume(CreateEventW(nullptr, FALSE, FALSE, target_fixture::press_event(field, L"resume").c_str()));
    require(ready.value && resume.value && SetPropW(field, target_fixture::selection, HANDLE(1)), "Missing press events");
    inputs = 0;
    received.clear();
    auto input = std::make_shared<CardPressInput>(nullptr, read_fixture_input);
    EditGesture gesture;
    gesture.begin_bind(false, change == InputChange::missing_hooks ? nullptr : input);
    require(WaitForSingleObject(ready.value, 1000) == WAIT_OBJECT_0, "Worker did not reach requested binding phase");
    require(SetPropW(field, target_fixture::changed_input, HANDLE(1)), "Could not advance worker input clock");
    if (change == InputChange::keyboard) input->interrupt();
    if (change == InputChange::outside_click) {
        HWND other = CreateWindowW(L"STATIC", nullptr, WS_POPUP, 0, 0, 20, 20, nullptr, nullptr, nullptr, nullptr);
        require(other != nullptr, "No external mouse target");
        const bool local = input->local_recipient(field, other, nullptr, WM_LBUTTONDOWN);
        DestroyWindow(other);
        require(!local, "External click counted as a workspace press");
        input->interrupt();
    }
    // The final local release must never erase an earlier interruption.
    fixture_tick = DWORD(reinterpret_cast<UINT_PTR>(GetPropW(field, target_fixture::next_tick)));
    const bool delayed = change == InputChange::pending_hook || change == InputChange::later_local;
    if (!delayed && change != InputChange::unwitnessed) input->local_pointer(fixture_tick);
    require(SetEvent(resume.value), "Worker did not resume");
    if (delayed) {
        host_sampled_clock = false;
        const auto limit = GetTickCount64() + 750;
        while (!host_sampled_clock && GetTickCount64() < limit) {
            require(!gesture.binding_ready(), "Binding completed before the hook witnessed the new tick");
            Sleep(1);
        }
        require(host_sampled_clock, "Host did not sample the changed input clock");
        if (change == InputChange::later_local) ++fixture_tick;
        input->local_pointer(fixture_tick);
    }
    publish(field);
    const auto sequence = GetClipboardSequenceNumber();
    auto current = [&] { return sequence == GetClipboardSequenceNumber(); };
    const auto result = gesture.deliver(current, PastePayload::text);
    const bool sent = change == InputChange::local || change == InputChange::pending_hook;
    std::wcout << stage << L" change=" << int(change) << L" outcome=" << int(result) << L" received=" << inputs << std::endl;
    require(result == (sent ? DeliveryResult::shortcut_sent : DeliveryResult::no_field), "UIA press binding rejected or misdirected");
    require(gesture.deliver(current, PastePayload::text) == DeliveryResult::consumed, "Repeated UIA dispatch");
    require(inputs == unsigned(sent) && selected_pastes == unsigned(sent) && other_pastes == 0 &&
            field_text(selected_edit) == (sent ? L"\u5de6\u6837\u4f8b\u53f3" : L"\u5de6X\u53f3") &&
            field_text(other_edit) == L"other", "Wrong UIA receiver field, selected text or paste count");
}
void office_press(bool interrupted) {
    require(SetEnvironmentVariableW(L"CLIPROPE_TEST_TARGET_HOLD", L"office-stable"), "No Office fixture");
    HWND field = CreateWindowW(L"ClipRope.TargetFixture", L"", WS_POPUP, 0, 0, 50, 50,
                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(field != nullptr, "No Office binding window");
    struct Close { HWND field; ~Close() { DestroyWindow(field); } } close{field};
    auto owned_popup = [&] {
        return CreateWindowW(L"STATIC", L"", WS_POPUP, 0, 0, 10, 10, field, nullptr, GetModuleHandleW(nullptr), nullptr);
    };
    const HWND drag = owned_popup(), sibling = owned_popup();
    require(drag && sibling, "No private capture windows");
    auto input = std::make_shared<CardPressInput>(drag, read_fixture_input);
    require(input->local_recipient(field, drag, nullptr, WM_LBUTTONUP) &&
            input->local_recipient(field, drag, drag, WM_MOUSEMOVE), "Card capture window rejected its own release or move");
    require(!input->local_recipient(field, sibling, nullptr, WM_LBUTTONUP) &&
            !input->local_recipient(field, drag, sibling, WM_LBUTTONUP) &&
            !input->local_recipient(field, drag, drag, WM_LBUTTONDOWN), "Unrelated capture input accepted");
    TargetSession session(field, field, GetCurrentProcessId(), GetCurrentThreadId(), false, input);
    input->local_pointer(++fixture_tick);
    if (interrupted) input->interrupt();
    std::optional<target::Kind> bound;
    const auto deadline = GetTickCount64() + 1500;
    while (!(bound = session.binding()) && GetTickCount64() < deadline) Sleep(1);
    require(bound == (interrupted ? target::Kind::none : target::Kind::office), "Wrong guarded Office binding");
    // Later input is handled by the worker's check of the retained selection.
    input->interrupt();
    require(session.binding() == bound, "Binding changed after its identity was retained");
}
}
int wmain(int argc, wchar_t** argv) {
    if (!probe::private_station()) return 2;
    try {
        CardPressInput input(nullptr, read_fixture_input);
        fixture_tick = 10;
        require(input.permits(10), "Unchanged input rejected");
        fixture_tick = 20;
        require(!input.permits(10), "Unobserved input accepted");
        input.local_pointer(20);
        require(input.permits(10), "Card release timestamp lost");
        fixture_tick = 30;
        require(!input.permits(10), "Unknown input accepted after a local pointer event");
        input.interrupt();
        input.local_pointer(30);
        require(!input.permits(10) && !input.permits(30), "Unrelated input was forgotten");
        WNDCLASSW wc{};
        wc.lpfnWndProc = recipient;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ClipRope.TargetFixture";
        require(RegisterClassW(&wc), "No receiver class");
        if (argc == 2 && std::wstring_view(argv[1]) == L"--uia-press") {
            for (const auto stage : {L"uia-before", L"uia-reading"})
                for (auto change : {InputChange::local, InputChange::pending_hook, InputChange::keyboard,
                                    InputChange::outside_click, InputChange::unwitnessed,
                                    InputChange::later_local, InputChange::missing_hooks})
                    uia_press(stage, change);
            return 0;
        }
        if (argc == 2 && std::wstring_view(argv[1]) == L"--missing") {
            exercise(L"uia-missing", DeliveryResult::no_field);
            return 0;
        }
        office_press(true);
        office_press(false);
        for (const auto* stage : {L"uia-stable", L"office-stable", L"shell-stable"})
            exercise(stage, DeliveryResult::shortcut_sent);
        for (const auto* stage : {L"uia-changed", L"office-changed", L"shell-changed", L"uia-stale-reply",
                                  L"uia-destroyed", L"uia-cancelled"})
            exercise(stage, DeliveryResult::lost_target);
        exercise(L"uia-replaced", DeliveryResult::replaced_publication);
        // Run the test client without its sibling helper. Keep the production runtime intact.
        history_fixture::Directory isolated;
        const auto client = isolated.path / "target-client.exe";
        std::filesystem::copy_file(std::filesystem::absolute(argv[0]), client);
        HANDLE child = probe::launch({client.wstring(), L"--missing"}, probe::desktop_path());
        require(child != nullptr, "Missing-worker client did not launch");
        const auto waited = WaitForSingleObject(child, 3500);
        DWORD status{};
        const bool completed = waited == WAIT_OBJECT_0 && GetExitCodeProcess(child, &status) && !status;
        if (waited == WAIT_TIMEOUT) { TerminateProcess(child, 1); WaitForSingleObject(child, 1000); }
        CloseHandle(child);
        require(completed, "Missing worker did not degrade to manual paste");
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
