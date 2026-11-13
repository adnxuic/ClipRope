#include "../integration/resident.hpp"
#include "../integration/diagnostic_report.hpp"
#include "../performance/process_metrics.hpp"
#include "../integration/history_fixture.hpp"
#include "persistence/history_store.hpp"
#include "persistence/privacy.hpp"
#include "platform_win/performance_counters.hpp"
#include "platform_win/target_protocol.hpp"
#include <algorithm>
#include <fstream>

namespace {
using namespace cliprope;
using probe::require;
using win::target::Handle;
unsigned inputs{};
LRESULT CALLBACK recipient(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_PASTE || (msg == WM_KEYDOWN && w == 'V')) ++inputs;
    return DefWindowProcW(hwnd, msg, w, l);
}
void exercise(const wchar_t* exe, const wchar_t* gate, bool close_while_held) {
    history_fixture::Directory profile;
    const std::wstring text = L"AR3 isolated target " + std::wstring(gate) + L" " + std::to_wstring(GetCurrentProcessId());
    struct ReleaseSample {
        const std::wstring& sample;
        ~ReleaseSample() { probe::clear_private_sample(sample); }
    } release{text}; // Destroy the host and flush its publication before private cleanup, also on failure.
    {
        persistence::HistoryStore store(profile.path);
        persistence::save_privacy(store.database(), {true, {}});
        const auto* bytes = reinterpret_cast<const unsigned char*>(text.c_str());
        store.admit("main", {{{CF_UNICODETEXT, {}, {bytes, bytes + (text.size() + 1) * sizeof(wchar_t)}}}});
    }
    const auto report = profile.path / "receipt.txt";
    require(SetEnvironmentVariableW(L"CLIPROPE_TEST_TARGET_HOLD", gate), "Could not select hold");
    probe::Resident host(exe, profile.path, true, report, nullptr, {}, true);
    SetEnvironmentVariableW(L"CLIPROPE_TEST_TARGET_HOLD", nullptr);
    Handle group(CreateJobObjectW(nullptr, nullptr));
    require(group.value && AssignProcessToJobObject(group.value, host.handle()), "Could not observe the desktop job");
    const HWND field = CreateWindowW(L"ClipRope.TargetFixture", L"", WS_POPUP | WS_VISIBLE, 0, 0, 100, 40,
                                     nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(field != nullptr, "No synthetic receiver");
    struct Destroy { HWND hwnd; ~Destroy() { DestroyWindow(hwnd); } } destroy{field};
    const auto event_name = L"Local\\ClipRope.TargetHeld." + std::to_wstring(GetCurrentProcessId());
    Handle held(CreateEventW(nullptr, TRUE, FALSE, event_name.c_str()));
    require(held.value != nullptr, "No hold event");
    const auto center = SendMessageW(host.listener(), win::performance_query, WPARAM(win::PerformanceCounter::first_card_point), 0);
    POINT p{short(LOWORD(center)), short(HIWORD(center))};
    require(center && ScreenToClient(host.workspace, &p), "No test card");
    const DWORD host_pid = GetProcessId(host.handle());
    const auto idle = performance::process_tree(host_pid);
    const auto start = GetTickCount64();
    for (auto msg : {WM_LBUTTONDOWN, WM_LBUTTONUP, WM_LBUTTONDBLCLK, WM_LBUTTONUP})
        require(PostMessageW(host.workspace, msg, msg == WM_LBUTTONUP ? 0 : MK_LBUTTON, MAKELPARAM(p.x, p.y)), "Gesture not queued");
    DWORD_PTR ignored{};
    ULONGLONG previous = start, max_gap{};
    const auto beat = [&] {
        require(SendMessageTimeoutW(host.listener(), WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 100, &ignored), "Desktop heartbeat timeout");
        const auto now = GetTickCount64();
        max_gap = std::max(max_gap, now - previous);
        previous = now;
    };
    try {
        probe::pump_until([&] { beat(); return WaitForSingleObject(held.value, 0) == WAIT_OBJECT_0; }, "Provider did not reach selected hold", 2500);
    } catch (...) {
        std::ifstream log(report);
        std::cerr << log.rdbuf();
        throw;
    }
    const auto entered = GetTickCount64() - start;
    const auto holding = performance::process_tree(host_pid);
    require(holding.worker_private > 0, "Target worker memory not observed at hold");
    if (!close_while_held) {
        probe::pump_until([&] {
            beat();
            std::ifstream in(report);
            const std::string log((std::istreambuf_iterator<char>(in)), {});
            return log.find("invoke double-click") != log.npos && log.find(" committed", log.find("invoke double-click")) != log.npos;
        }, "Missing terminal gesture receipt", 2300);
    }
    require(max_gap <= 100, "Heartbeat gap exceeded 100 ms");
    const auto quitting = GetTickCount64();
    host.exit();
    probe::pump_until([&] {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION counts{};
        require(QueryInformationJobObject(group.value, JobObjectBasicAccountingInformation, &counts, sizeof counts, nullptr), "Job query failed");
        return counts.ActiveProcesses == 0;
    }, "Target helper survived desktop exit", 2000);
    const auto shutdown = GetTickCount64() - quitting;
    const auto diagnostics = history_fixture::diagnostic_report(profile.path);
    require(diagnostics.find(close_while_held ? "Target verification timeouts: 0\r\n" :
                                                "Target verification timeouts: 1\r\n") != diagnostics.npos,
            "Verification timeout and cancellation were not distinguished");
    require(shutdown <= 2000 && !inputs, "Exit bound or zero-input contract failed");
    require(probe::clear_private_sample(text), "Recall did not leave the published text available");
    std::wcout << gate << (close_while_held ? L" close_during_hold" : L" expire")
               << L" before_quit_ms=" << quitting - start << L" enter_ms=" << entered << L" heartbeat_max_ms=" << max_gap << L" exit_ms=" << shutdown
               << L" idle_private=" << idle.total_private() << L" held_private=" << holding.total_private()
               << L" idle_handles=" << idle.handles << L" held_handles=" << holding.handles << L'\n';
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2 || !probe::private_station()) return 2;
    try {
        WNDCLASSW wc{};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ClipRope.TargetFixture";
        wc.lpfnWndProc = recipient;
        require(RegisterClassW(&wc) != 0, "Receiver class unavailable");
        for (const auto gate : {L"uia-bind",L"office-bind",L"shell-bind",L"uia-verify",L"office-verify",L"shell-verify"}) {
            exercise(argv[1],gate,false);
            exercise(argv[1],gate,true);
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
