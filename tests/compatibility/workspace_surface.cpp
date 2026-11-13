#include "workspace_fixture_frame.hpp"
#include "platform_win/workspace.hpp"
#include "../integration/clipboard_process.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>

using namespace cliprope;
namespace {
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}

template <class F> bool until(F done) {
    const auto end = GetTickCount64() + 3500;
    do {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (GetTickCount64() >= end) {
                std::cerr << "Input queue did not drain, last message " << msg.message << '\n';
                return false;
            }
        }
        if (done())
            return true;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    } while (GetTickCount64() < end);
    return false;
}

struct Underlay {
    HANDLE process{};
    HWND hwnd{};
    void open(const std::wstring &exe) {
        const auto pid = std::to_wstring(GetCurrentProcessId());
        process = probe::launch({exe, L"--receiver", pid}, probe::desktop_path());
        require(process != nullptr, "Could not launch underlay");
        const auto title = L"ClipRope underlay " + pid;
        require(until([&] {
                    hwnd = FindWindowW(L"ClipRope.Probe.Receiver", title.c_str());
                    DWORD owner{};
                    if (hwnd)
                        GetWindowThreadProcessId(hwnd, &owner);
                    return owner == GetProcessId(process);
                }),
                "Underlay did not open");
    }
    int count() const {
        DWORD_PTR clicks{};
        require(SendMessageTimeoutW(hwnd, WM_APP + 1, 0, 0, SMTO_ABORTIFHUNG, 500, &clicks) != 0,
                "Underlay failed to acknowledge");
        return static_cast<int>(clicks);
    }
    ~Underlay() {
        if (hwnd)
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        if (process) {
            WaitForSingleObject(process, 1200);
            CloseHandle(process);
        }
    }
};

void click(HWND expected, POINT p, bool physical) {
    require(WindowFromPoint(p) == expected, "OS hit selected a different window");
    if (physical) {
        require(SetCursorPos(p.x, p.y) != 0, "Cannot position pointer");
        POINT q{};
        // The previous click's receiver can acknowledge DOWN before Windows
        // processes its paired UP. Pump that release before starting a new press.
        require(until([&] {
                    return GetCursorPos(&q) && q.x == p.x && q.y == p.y && WindowFromPoint(q) == expected &&
                        !(GetAsyncKeyState(VK_LBUTTON) & 0x8000) && !(GetAsyncKeyState(VK_RBUTTON) & 0x8000);
                }),
                "Pointer or target changed");
        INPUT pair[2]{};
        pair[0].type = pair[1].type = INPUT_MOUSE;
        pair[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        pair[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        require(SendInput(2, pair, sizeof(INPUT)) == 2, "Test input denied");
    } else {
        ScreenToClient(expected, &p);
        PostMessageW(expected, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(p.x, p.y));
        PostMessageW(expected, WM_LBUTTONUP, 0, MAKELPARAM(p.x, p.y));
    }
}

void rapid_hover(const win::Workspace &workspace, RECT fourth, RECT fifth) {
    const auto hwnd = workspace.window();
    const auto dpi = GetDpiForWindow(hwnd);
    const auto body = [](RECT r) { return POINT{(r.left + r.right) / 2, (r.top + r.bottom) / 2}; };
    const auto move = [](POINT p) { require(SetCursorPos(p.x, p.y) != FALSE, "Could not move hover pointer"); };
    const POINT gap{(fourth.right + fifth.left) / 2, body(fourth).y};
    move(gap);
    auto frames = workspace.painted_frames();
    auto changed = GetTickCount64();
    require(until([&] {
        if (frames != workspace.painted_frames()) {
            frames = workspace.painted_frames();
            changed = GetTickCount64();
        }
        return GetTickCount64() - changed >= 200;
    }), "Opening swing did not settle before the hover sweep");

    // Sample the dark center of the close cross on otherwise plain paper.
    const auto marks = [&] {
        HDC dc = GetDC(nullptr);
        const auto sample = [&](RECT r) {
            return GetPixel(dc, r.right - MulDiv(13, dpi, 96), r.top + MulDiv(15, dpi, 96));
        };
        const auto four = sample(fourth), five = sample(fifth);
        ReleaseDC(nullptr, dc);
        require(four != CLR_INVALID && five != CLR_INVALID, "Could not sample the presented hover marks");
        return std::pair{GetRValue(four) < 185, GetRValue(five) < 185};
    };
    const auto expect_marks = [&](bool four, bool five, const char *failure) {
        auto since = GetTickCount64();
        const auto before = workspace.painted_frames();
        if (!until([&] {
            const bool matches = marks() == std::pair{four, five};
            if (!matches) since = GetTickCount64();
            return matches && GetTickCount64() - since >= 100;
        })) {
            POINT pt{};
            GetCursorPos(&pt);
            const auto [on_four, on_five] = marks();
            std::cerr << "Hover frames " << before << " -> " << workspace.painted_frames()
                      << ", cursor over workspace " << (WindowFromPoint(pt) == hwnd)
                      << ", marks on 4/5 " << on_four << "/" << on_five << '\n';
            throw std::runtime_error(failure);
        }
    };
    move(body(fifth));
    expect_marks(false, true, "Card five did not show its hover mark");
    for (int i = 0; i < 20; ++i) {
        for (POINT p : {gap, body(fourth), gap, body(fifth)}) {
            move(p);
            MSG msg{};
            if (PeekMessageW(&msg, nullptr, WM_MOUSEMOVE, WM_MOUSEMOVE, PM_REMOVE))
                DispatchMessageW(&msg);
        }
    }
    move(body(fourth));
    expect_marks(true, false, "Rapid crossing left the hover mark on the wrong card");

    // A leave from a transparent gap and an older move can still be queued
    // when the physical cursor has already stopped back on the fourth card.
    auto stale = body(fifth);
    ScreenToClient(hwnd, &stale);
    PostMessageW(hwnd, WM_MOUSELEAVE, 0, 0);
    PostMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(stale.x, stale.y));
    expect_marks(true, false, "Delayed pointer messages overrode the stationary cursor");
    const auto idle_frames = workspace.painted_frames();
    expect_marks(true, false, "Stationary hover did not persist");
    require(workspace.painted_frames() == idle_frames, "Stationary hover kept repainting");
    move(gap);
    expect_marks(false, false, "Transparent gap retained a hover mark");
    std::cout << "Hover followed card 5 to card 4 across rapid moves and cleared in the gap\n";
}

void run(const std::wstring &receiver, bool physical, const std::filesystem::path &output) {
    Underlay under;
    under.open(receiver);
    std::vector<std::string> order;
    for (std::size_t i = 0; i < 1000; ++i)
        order.push_back(std::to_string(i));
    application::Anchor anchor;
    int presses = 0, errors = 0;
    win::WorkspaceReads reads{
        [&](auto) { return order.size(); },
        [&](auto, std::size_t pos, std::size_t count) {
            static constexpr std::wstring_view samples[] = {
                L"Meeting notes for Friday", L"Review chapter two", L"预算与采购清单",
                L"Send the updated draft",   L"Sample Ω · 12:30",   L"Design references"};
            std::vector<application::CardSummary> cards;
            for (std::size_t i = pos; i < pos + count; ++i) {
                application::CardSummary c;
                c.entry.id = order.at(i);
                c.style.background_color = 0xfff8e8;
                c.text = samples[i % std::size(samples)];
                cards.push_back(std::move(c));
            }
            return cards;
        },
        [&](auto, std::string_view id) -> std::optional<std::size_t> {
            auto it = std::find(order.begin(), order.end(), id);
            return it == order.end() ? std::nullopt : std::optional<std::size_t>(it - order.begin());
        },
        [&](auto) { return anchor; }, [&](auto, const auto &a) { anchor = a; }};
    win::WorkspaceActions actions;
    actions.press = [&](auto &) { ++presses; };
    actions.error = [&] { ++errors; };
    win::Workspace workspace(std::move(reads), std::move(actions));
    require(workspace.open(), "Workspace creation failed");
    workspace.show("main");
    auto hwnd = workspace.window();
    RECT r{};
    GetWindowRect(hwnd, &r);
    require(SetWindowPos(under.hwnd, hwnd, r.left, r.top, r.right - r.left, r.bottom - r.top,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW) != 0,
            "Could not place underlay");
    require(GetWindow(hwnd, GW_CHILD) == nullptr, "Unexpected per-card child HWND");
    const auto slots = workspace.slots();
    require(slots.size() >= 2, "No visible cards");
    const auto first = slots[0].second, second = slots[1].second;
    const POINT paper{first.left + 4, first.bottom - 6};
    const POINT gap{(first.right + second.left) / 2, paper.y};
    const auto foreground = GetForegroundWindow();
    workspace.always_on_top(false);
    require(SetWindowPos(under.hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE) &&
            SetWindowPos(under.hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE),
            "Could not raise ordinary receiver");
    require(WindowFromPoint(paper) == under.hwnd, "Ordinary receiver could not cover the workspace");
    workspace.display_changed();
    require(WindowFromPoint(paper) == under.hwnd, "Monitor placement raised a covered workspace");
    workspace.always_on_top(true);
    require(WindowFromPoint(paper) == hwnd && WindowFromPoint(gap) == under.hwnd &&
            GetForegroundWindow() == foreground, "Changing workspace Z order disturbed focus or hit testing");
    if (physical) {
        require(slots.size() >= 5, "Five visible cards required for hover sweep");
        rapid_hover(workspace, slots[3].second, slots[4].second);
        require(until([&] {
                    HDC dc = GetDC(nullptr);
                    const auto card = GetPixel(dc, paper.x, paper.y), empty = GetPixel(dc, gap.x, gap.y);
                    ReleaseDC(nullptr, dc);
                    return card == RGB(255, 248, 232) && empty == RGB(240, 224, 191);
                }),
                "Rendered card or gap pixel mismatch");
    }
    click(hwnd, paper, physical);
    require(until([&] { return presses == 1; }), "Workspace did not receive press");
    click(under.hwnd, gap, physical);
    require(until([&] { return under.count() == 1; }), "Underlay did not receive gap click");
    require(presses == 1 && under.count() == 1, "Extra press observed");
    const auto dpi = GetDpiForWindow(hwnd);
    int forwarded = under.count();
    auto check_toolbar_gap = [&] {
        for (int x : {80, 220, 380, 555, 680, 805, MulDiv(r.right - r.left, 96, dpi) - 22}) {
            POINT former_button{r.left + MulDiv(x, dpi, 96), r.top + MulDiv(22, dpi, 96)};
            click(under.hwnd, former_button, physical);
            ++forwarded;
            require(until([&] { return under.count() == forwarded; }), "Former button ate a click");
        }
    };
    check_toolbar_gap();
    if (!output.empty())
        workspace_fixture::frame(hwnd, output);
    SendMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA * 4), MAKELPARAM(paper.x, paper.y));
    const auto before = workspace.slots().front();
    order.insert(order.begin(), "new");
    workspace.refresh();
    const auto after = workspace.slots().front();
    require(before.first == after.first && before.second.left == after.second.left, "Anchor shifted");
    const auto start = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    std::size_t maximum = 0;
    for (int i = 0; i < 80; ++i) {
        SendMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA * 3), MAKELPARAM(paper.x, paper.y));
        UpdateWindow(hwnd);
        maximum = std::max(maximum, workspace.realized_count());
    }
    const auto finish = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    // A row realizes its visible cards, one on either side and one partial card.
    RECT client{};
    GetClientRect(hwnd, &client);
    const auto bound = static_cast<std::size_t>(std::ceil((double(client.right) * 96 / dpi - 28) / 200)) + 3;
    require(finish <= start + 3 && maximum <= bound && !errors, "Unbounded resources or raster failure");
    order.clear();
    workspace.refresh();
    require(workspace.slots().empty(), "Empty rope retained card slots");
    check_toolbar_gap();
    std::cout << "workspace=1 child_hwnds=0 card_presses=1 underlay_presses=" << forwarded << " anchor=" << after.first
              << " anchor_x=" << after.second.left << " max_realized=" << maximum << " gdi_before=" << start
              << " gdi_after=" << finish << " input=" << (physical ? "SendInput" : "OS hit plus queue")
              << '\n';
    workspace.hide();
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc < 3 || argc > 4)
        return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const bool physical = std::wstring_view(argv[1]) == L"--physical";
    struct Restore {
        POINT pos{};
        bool active{};
        ~Restore() {
            if (active)
                SetCursorPos(pos.x, pos.y);
        }
    } restore;
    restore.active = physical && GetCursorPos(&restore.pos);
    HDESK previous = GetThreadDesktop(GetCurrentThreadId()), desktop{};
    int result = 1;
    try {
        if (!physical) {
            require(std::wstring_view(argv[1]) == L"--isolated", "Unknown surface test mode");
            auto name = L"ClipRope.Surface." + std::to_wstring(GetCurrentProcessId());
            desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
            require(desktop && SetThreadDesktop(desktop), "Private desktop unavailable");
        }
        run(argv[2], physical, argc == 4 ? argv[3] : L"");
        result = 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
    }
    if (desktop) {
        SetThreadDesktop(previous);
        CloseDesktop(desktop);
    }
    return result;
}
