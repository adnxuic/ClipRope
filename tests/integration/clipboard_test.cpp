#include "capture_worker/capture.hpp"
#include "../clipboard_sources/external_copy.hpp"
#include "../clipboard_sources/sample.hpp"
#include <algorithm>
#include <functional>
#include <stdexcept>

using namespace cliprope;
namespace {
using probe::ExternalCopy;
using probe::pump_until;
using probe::require;

struct Events {
    ClipboardPublisher publisher;
    unsigned own{}, external{}, timer{};
};

LRESULT CALLBACK window_proc(HWND window, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<Events*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        state = static_cast<Events*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state && msg == WM_CLIPBOARDUPDATE) {
        if (state->publisher.is_current()) ++state->own;
        else ++state->external;
        return 0;
    }
    if (state && msg == WM_TIMER) { ++state->timer; return 0; }
    return DefWindowProcW(window, msg, wparam, lparam);
}

CaptureResult collect(std::future<CaptureResult> result) {
    pump_until([&] { return result.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }, "Capture STA timed out");
    return result.get();
}

void expect_saved(const CaptureResult& result, const std::vector<Representation>& expected, const char* outcome) {
    std::cout << "capture=" << result.outcome << " saved=" << result.saved.size() << '\n';
    for (const auto& row : result.formats)
        std::cout << "format=" << row.format << " medium=" << row.tymed << " outcome=" << row.outcome << '\n';
    require(result.outcome == outcome, "Capture outcome mismatch");
    require(std::is_permutation(result.saved.begin(), result.saved.end(), expected.begin(), expected.end()),
            "Recovered representations differ from source");
}

void paste(HWND edit, bool second, bool use_shortcut) {
    SetWindowTextW(edit, L"");
    if (use_shortcut) {
        BYTE prior[256]{}, keyboard[256]{};
        require(GetKeyboardState(prior) != FALSE, "Could not snapshot thread keyboard state");
        keyboard[VK_CONTROL] = keyboard[VK_LCONTROL] = 0x80;
        require(SetKeyboardState(keyboard) != FALSE, "Could not select Ctrl in test thread");
        MSG key{};
        key.hwnd = edit;
        key.message = WM_KEYDOWN;
        key.wParam = 'V';
        key.lParam = (MapVirtualKeyW('V', MAPVK_VK_TO_VSC) << 16) | 1;
        TranslateMessage(&key);
        DispatchMessageW(&key);
        MSG character{};
        const bool translated = PeekMessageW(&character, edit, WM_CHAR, WM_CHAR, PM_REMOVE) &&
                                character.wParam == 22;
        if (translated) DispatchMessageW(&character);
        SetKeyboardState(prior);
        require(translated, "Windows did not translate Ctrl+V to the paste command");
    } else {
        SendMessageW(edit, WM_PASTE, 0, 0);
    }
    wchar_t content[128]{};
    GetWindowTextW(edit, content, 128);
    require(sample::text(second) == content, "Native edit paste mismatch");
}

void run(const wchar_t* producer, const std::wstring& scenario) {
    Events state;
    CaptureSta worker;
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = L"ClipRope.Test.ClipboardListener";
    require(RegisterClassW(&wc) != 0, "Listener class failed");
    auto listener = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 300, 100,
                                    nullptr, nullptr, instance, &state);
    require(listener && AddClipboardFormatListener(listener), "Listener creation failed");
    auto edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_MULTILINE, 0, 0, 250, 80,
                                listener, nullptr, instance, nullptr);
    require(edit && SetTimer(listener, 1, 15, nullptr), "Native edit or timer creation failed");
    auto capture = [&] { return collect(worker.submit(GetClipboardSequenceNumber())); };
    auto expected = sample::formats(false);
    if (scenario == L"capture") {
        {
            ExternalCopy copy(producer, L"delayed");
            auto capture_job = worker.submit(GetClipboardSequenceNumber());
            pump_until([&] { return WaitForSingleObject(copy.reading, 0) == WAIT_OBJECT_0; }, "HTML read did not start");
            std::vector<std::future<CaptureResult>> queued;
            for (int i = 0; i < 4; ++i) queued.push_back(worker.submit(GetClipboardSequenceNumber()));
            require(worker.submit(GetClipboardSequenceNumber()).get().outcome == "queue-full", "Queue limit not enforced");
            const auto before = state.timer;
            pump_until([&] { return state.timer >= before + 3; }, "UI stopped pumping during delayed read");
            SetEvent(copy.proceed);
            const auto result = collect(std::move(capture_job));
            expect_saved(result, expected, "captured");
            for (auto& future : queued)
                require(collect(std::move(future)).saved == expected, "Queued ticket lost during external COM call");
            require(std::any_of(result.formats.begin(), result.formats.end(), [](const auto& item) {
                return item.format == RegisterClipboardFormatW(L"ClipRope Synthetic Private") && item.outcome == "unsupported-format";
            }), "Unsupported external format missing from inventory");
            paste(edit, false, false);
            std::cout << "UI timers during delay >=3; native paste intact\n";
        }
        {
            ExternalCopy copy(producer, L"partial");
            expect_saved(capture(), {expected.front()}, "partial");
            paste(edit, false, false);
        }
        {
            ExternalCopy old(producer, L"delayed");
            auto obsolete = worker.submit(GetClipboardSequenceNumber());
            pump_until([&] { return WaitForSingleObject(old.reading, 0) == WAIT_OBJECT_0; }, "Replacement read did not start");
            ExternalCopy replacement(producer, L"second");
            SetEvent(old.proceed);
            expect_saved(collect(std::move(obsolete)), {}, "superseded");
            expect_saved(capture(), sample::formats(true), "captured");
            paste(edit, true, true);
        }
    } else if (scenario == L"publication") {
        ExternalCopy initial(producer, L"normal");
        auto saved = capture().saved;
        require(saved == expected, "Initial synthetic capture failed");
        require(SUCCEEDED(state.publisher.publish(saved)), "Republish failed");
        pump_until([&] { return state.own > 0; }, "No self publication observed");
        require(FAILED(state.publisher.publish({{CF_UNICODETEXT, {0x41, 0x00}}})) && state.publisher.is_current(),
                "Invalid format changed the current publication");
        paste(edit, false, true);
        expect_saved(capture(), expected, "captured");
        const auto prior = state.external;
        ExternalCopy equal(producer, L"normal");
        pump_until([&] { return state.external > prior; }, "Equal genuine copy was suppressed");
        require(!state.publisher.is_current(), "Publisher claimed external object");
        expect_saved(capture(), expected, "captured");
        ExternalCopy different(producer, L"second");
        require(state.publisher.flush_if_current() == S_FALSE, "Publisher flushed a newer clipboard");
        paste(edit, true, true);
        std::cout << "self suppressed; equal external admitted; next Ctrl+V uses newer copy\n";
    } else throw std::runtime_error("Unknown scenario");
    RemoveClipboardFormatListener(listener);
    DestroyWindow(listener);
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3 || !probe::private_station()) {
        std::cerr << "Run in the private window station launcher.\n";
        return 2;
    }
    if (FAILED(OleInitialize(nullptr))) return 1;
    int result = 0;
    try { run(argv[1], argv[2]); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    OleUninitialize();
    return result;
}
