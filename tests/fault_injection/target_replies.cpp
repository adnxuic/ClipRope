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
constexpr wchar_t sample[] = L"ClipRope AR3 private receiver sample";
unsigned inputs{};
std::wstring received;
DWORD fixture_tick{};
BOOL WINAPI read_fixture_input(PLASTINPUTINFO info) {
    info->dwTime = fixture_tick;
    return TRUE;
}

LRESULT CALLBACK recipient(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_PASTE) {
        ++inputs;
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
