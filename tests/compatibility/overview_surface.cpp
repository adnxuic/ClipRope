#include "platform_win/workspace_commands.hpp"
#include "platform_win/workspace.hpp"
#include "platform_win/floating.hpp"
#include "workspace_fixture_frame.hpp"
#include "presentation/overview.hpp"
#include "rendering/workspace_paint.hpp"
#include <commctrl.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <windowsx.h>

using namespace cliprope;
namespace {
void check(bool ok, const char *what) {
    if (!ok)
        throw std::runtime_error(what);
}
template <class F> bool until(F ready, DWORD timeout = 2500) {
    const auto end = GetTickCount64() + timeout;
    do {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (ready())
            return true;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    } while (GetTickCount64() < end);
    return false;
}
struct Data {
    std::vector<domain::Rope> ropes;
    std::map<std::string, std::vector<std::string>> ids;
    std::map<std::string, application::Anchor> anchors;
    std::map<std::string, bool> compact;
    std::size_t largest_rope_page = 0, largest_card_page = 0;
    std::set<std::string> queried;
    Data() {
        for (int n = 0; n < 40; ++n) {
            const auto id = "r" + std::to_string(n);
            const char *names[] = {"main", "Research", "Drafts", "Reference"};
            const std::uint32_t colors[] = {0xc29864, 0x468980, 0xbd794a, 0x8a72a8};
            ropes.push_back(
                {id, n < 4 ? names[n] : "Rope " + std::to_string(n), 1000, n == 0, colors[n % 4]});
            for (int i = 0; i < 100; ++i)
                ids[id].push_back(id + "-" + std::to_string(i));
        }
    }
    win::WorkspaceReads reads() {
        win::WorkspaceReads r;
        r.count = [this](auto id) { return ids.at(std::string(id)).size(); };
        r.page = [this](auto id, std::size_t offset, std::size_t n) {
            auto &all = ids.at(std::string(id));
            queried.insert(std::string(id));
            largest_card_page = std::max(largest_card_page, n);
            std::vector<application::CardSummary> result;
            for (std::size_t i = offset; i < offset + n; ++i) {
                application::CardSummary item;
                item.entry.id = all.at(i);
                item.entry.rope_id = id;
                const std::wstring_view examples[] = {
                    L"Diffraction notes", L"Review by Friday",  L"Budget · Q3", L"Figure revisions",
                    L"Reference sample",  L"Method comparison", L"Next steps",  L"From the archive"};
                item.text =
                    examples[(i + id.size() + static_cast<std::size_t>(id.back())) % std::size(examples)];
                result.push_back(std::move(item));
            }
            return result;
        };
        r.position = [this](auto rope, auto id) -> std::optional<std::size_t> {
            auto &all = ids.at(std::string(rope));
            auto found = std::find(all.begin(), all.end(), id);
            if (found == all.end())
                return {};
            return found - all.begin();
        };
        r.load_anchor = [this](auto id) { return anchors[std::string(id)]; };
        r.save_anchor = [this](auto id, const auto &anchor) { anchors[std::string(id)] = anchor; };
        r.load_compact = [this](auto id) { return compact[std::string(id)]; };
        r.save_compact = [this](auto id, bool enabled, const auto& anchor) {
            compact[std::string(id)] = enabled;
            anchors[std::string(id)] = anchor;
        };
        r.rope_count = [this] { return ropes.size(); };
        r.ropes = [this](std::size_t pos, std::size_t n) {
            largest_rope_page = std::max(largest_rope_page, n);
            check(pos + n <= ropes.size(), "Rope page past end");
            return std::vector(ropes.begin() + pos, ropes.begin() + pos + n);
        };
        r.rope_position = [this](auto id) -> std::optional<std::size_t> {
            for (std::size_t i = 0; i < ropes.size(); ++i)
                if (ropes[i].id == id)
                    return i;
            return {};
        };
        return r;
    }
};
POINT dip(HWND hwnd, int x, int y) {
    const auto dpi = GetDpiForWindow(hwnd);
    return {MulDiv(x, dpi, 96), MulDiv(y, dpi, 96)};
}
POINT local(HWND hwnd, POINT screen) {
    ScreenToClient(hwnd, &screen);
    return screen;
}
POINT center(RECT r) { return {(r.left + r.right) / 2, (r.top + r.bottom) / 2}; }
void mouse(HWND hwnd, UINT msg, POINT client, WPARAM state) {
    SendMessageW(hwnd, msg, state, MAKELPARAM(client.x, client.y));
}
void click(HWND hwnd, POINT client) {
    mouse(hwnd, WM_LBUTTONDOWN, client, MK_LBUTTON);
    mouse(hwnd, WM_LBUTTONUP, client, 0);
}
RECT find(const std::vector<std::pair<domain::Id, RECT>> &elements, const domain::Id &id) {
    for (auto &[key, r] : elements)
        if (key == id)
            return r;
    throw std::runtime_error("Expected node is not in the visible scene: " + id);
}
struct WheelReceiver {
    HWND edit{};
    int wheels{}, clicks{};
    static LRESULT CALLBACK proc(HWND w, UINT m, WPARAM k, LPARAM l, UINT_PTR, DWORD_PTR ref) {
        auto& receiver = *reinterpret_cast<WheelReceiver*>(ref);
        if (m == WM_MOUSEWHEEL) ++receiver.wheels;
        if (m == WM_LBUTTONDOWN) ++receiver.clicks;
        return DefSubclassProc(w, m, k, l);
    }
    void open() {
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        edit = CreateWindowExW(WS_EX_TOOLWINDOW, L"EDIT", L"Wheel receiver", WS_POPUP | WS_VISIBLE | ES_MULTILINE,
            work.left, work.top, work.right - work.left, work.bottom - work.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        check(edit && SetWindowSubclass(edit, proc, 1, reinterpret_cast<DWORD_PTR>(this)), "Could not open editor");
        SetForegroundWindow(edit);
        SetFocus(edit);
        if (GetForegroundWindow() != edit || GetFocus() != edit) {
            // Windows can decline foreground activation for a newly launched
            // test. Establish a real caret before creating the workspace.
            SetWindowPos(edit, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            const auto p = center(work);
            check(SetCursorPos(p.x, p.y) && WindowFromPoint(p) == edit, "Editor setup click is covered");
            INPUT events[2]{};
            events[0].type = events[1].type = INPUT_MOUSE;
            events[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            events[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            check(SendInput(2, events, sizeof(INPUT)) == 2 && until([&] {
                return clicks == 1 && GetForegroundWindow() == edit && GetFocus() == edit && !GetCapture();
            }), "Could not focus the test editor");
            SetWindowPos(edit, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            clicks = 0;
        }
    }
    ~WheelReceiver() { if (edit) DestroyWindow(edit); }
};
void wheel(HWND hwnd, POINT p, bool ctrl, bool physical, HWND recipient = nullptr) {
    if (!physical) {
        SendMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(ctrl ? MK_CONTROL : 0, -WHEEL_DELTA),
                     MAKELPARAM(p.x, p.y));
        return;
    }
    if (!recipient) recipient = hwnd;
    check(WindowFromPoint(p) == recipient && SetCursorPos(p.x, p.y), "Physical point is covered");
    POINT current{};
    check(GetCursorPos(&current) && current.x == p.x && current.y == p.y && WindowFromPoint(current) == recipient,
          "Pointer moved");
    check(!(GetAsyncKeyState(VK_CONTROL) & 0x8000) && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000),
          "Input is busy");
    INPUT events[3]{};
    events[0].type = events[2].type = INPUT_KEYBOARD;
    events[0].ki.wVk = events[2].ki.wVk = VK_CONTROL;
    events[2].ki.dwFlags = KEYEVENTF_KEYUP;
    events[1].type = INPUT_MOUSE;
    events[1].mi.dwFlags = MOUSEEVENTF_WHEEL;
    events[1].mi.mouseData = static_cast<DWORD>(-WHEEL_DELTA);
    const UINT n = ctrl ? 3 : 1;
    check(SendInput(n, ctrl ? events : &events[1], sizeof(INPUT)) == n, "Wheel input refused");
}
struct Keys {
    BYTE prior[256]{};
    explicit Keys(int modifier) {
        GetKeyboardState(prior);
        BYTE next[256]{};
        if (modifier)
            next[modifier] = 0x80;
        SetKeyboardState(next);
    }
    ~Keys() { SetKeyboardState(prior); }
};
void exercise(bool physical, const std::filesystem::path &frame_path) {
    WheelReceiver receiver;
    if (physical) receiver.open();
    Data data;
    domain::Id active = "r1", bound, moved, target;
    unsigned moves = 0, copies = 0, card_context = 0, rope_context = 0, errors = 0;
    win::Workspace *view = nullptr;
    win::WorkspaceActions actions;
    std::map<std::string, double> heights;
    actions.rope_top = [&](auto& id) { return heights.contains(id) ? heights.at(id) : 24.; };
    actions.save_rope_top = [&](auto& id, double value) { heights[id] = value; };
    actions.press = [&](auto &id) { bound = id; };
    actions.select = [&](auto &id) {
        active = id;
        view->set_active(id);
    };
    actions.transfer = [&](auto &id, auto &rope, bool copy, std::size_t, win::DropCompleted landed) {
        check(id == bound, "Transfer lost pressed identity");
        moved = id;
        target = rope;
        copy ? ++copies : ++moves;
        landed(id);
    };
    actions.context = [&](const auto&, POINT) { ++card_context; };
    actions.rope_menu = [&](const auto& rope, POINT, auto) {
        check(rope.id == "r3", "Menu bound a different rope");
        ++rope_context;
        return win::WorkspaceCommand::none;
    };
    actions.error = [&] { ++errors; };
    win::Workspace workspace(data.reads(), std::move(actions));
    view = &workspace;
    check(workspace.open(), "Workspace failed to open");
    workspace.show(active);
    const auto hwnd = workspace.window();
    SendMessageW(hwnd, WM_COMMAND, static_cast<WPARAM>(cliprope::win::WorkspaceCommand::overview), 0);
    check(workspace.in_overview() && workspace.rows().size() >= 3 && workspace.rows().front().first == "r1",
          "Expected visible test rows");
    std::set<std::string> visible;
    for (const auto& row : workspace.rows()) {
        visible.insert(row.first);
        const auto line = row.second;
        const auto above = dip(hwnd, 0, 25).y;
        for (int x : {80, 220, 380, 555, 680, 805}) {
            const POINT gap{line.left + dip(hwnd, x, 0).x, line.top - above};
            const auto p = local(hwnd, gap);
            const float scale = 96.f / GetDpiForWindow(hwnd);
            const presentation::RopeCurve curve((line.right - line.left) * scale);
            const auto row_top = (center(line).y - gap.y + p.y) * scale - curve.lowest();
            const presentation::RopePlaque plaque(curve, row_top);
            check((WindowFromPoint(gap) == hwnd) == plaque.contains(p.x * scale, p.y * scale),
                  "Plaque or transparent toolbar gap has the wrong input region");
        }
    }
    const auto initial = workspace.realized_count();
    if (!frame_path.empty())
        workspace_fixture::frame(hwnd, frame_path);
    data.queried.clear();
    for (int i = 0; i < 12000; ++i)
        data.ids["r34"].push_back("offscreen-" + std::to_string(i));
    workspace.refresh();
    check(workspace.realized_count() == initial && data.queried == visible,
          "Offscreen history inflated realized scene or triggered a card page");
    auto point = center(find(workspace.slots(), "r2-1"));
    if (physical) check(GetForegroundWindow() == receiver.edit && GetFocus() == receiver.edit, "Editor lost focus");
    wheel(hwnd, point, false, physical);
    check(until([&] { return data.anchors["r2"].entry == "r2-1"; }),
          "Normal wheel did not scroll the pointed row");
    check(active == "r1" && data.anchors["r1"].entry.empty() && data.anchors["r3"].entry.empty(),
          "Wheel changed collector or neighbor");
    const auto preserved = find(workspace.slots(), "r2-1").left;
    data.ids["r2"].insert(data.ids["r2"].begin(), "head");
    workspace.refresh();
    check(find(workspace.slots(), "r2-1").left == preserved, "Head displaced independent row anchor");
    auto &row = data.ids["r2"];
    row.erase(std::find(row.begin(), row.end(), "r2-1"));
    workspace.refresh();
    check(find(workspace.slots(), "r2-2").left == preserved, "Anchor eviction lost its neighbor");
    point = center(find(workspace.slots(), "r2-2"));
    wheel(hwnd, point, true, physical);
    check(until([&] { return workspace.rows().front().first == "r2"; }),
          "Ctrl+wheel did not scroll vertical rows");
    check(active == "r1" && data.anchors["r2"].entry == "r2-2",
          "Ctrl+wheel changed collector or horizontal anchor");
    if (physical) {
        auto line = center(find(workspace.rows(), "r3"));
        wheel(hwnd, line, false, true);
        check(until([&] { return data.anchors["r3"].entry == "r3-1"; }), "Wheel over line missed its row");
        const POINT gap{workspace.rows().front().second.left + 5, line.y + dip(hwnd, 0, 25).y};
        wheel(hwnd, gap, false, true, receiver.edit);
        check(until([&] { return data.anchors["r3"].entry == "r3-2"; }), "Wheel over transparent gap missed its row");
        INPUT click[2]{};
        click[0].type = click[1].type = INPUT_MOUSE;
        click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        check(WindowFromPoint(gap) == receiver.edit && SendInput(2, click, sizeof(INPUT)) == 2 &&
              until([&] { return receiver.clicks == 1; }), "Gap click did not reach the editor");
        check(!receiver.wheels && GetFocus() == receiver.edit, "Hover scrolling moved focus or duplicated wheel input");
        SetWindowPos(receiver.edit, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        wheel(hwnd, line, false, true, receiver.edit);
        check(until([&] { return receiver.wheels == 1; }) && data.anchors["r3"].entry == "r3-2",
              "Covered row stole editor wheel");
        SetWindowPos(receiver.edit, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        check(WindowFromPoint(line) == hwnd && SetCursorPos(line.x, line.y), "Double-click line is covered");
        auto button = [&](bool pressed) {
            INPUT e{};
            e.type = INPUT_MOUSE;
            e.mi.dwFlags = pressed ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
            check(SendInput(1, &e, sizeof(e)) == 1, "Physical rope press refused");
        };
        button(true);
        check(until([&] { return GetCapture() != nullptr; }), "Line missed the first press");
        button(false);
        check(until([&] { return GetCapture() == nullptr; }) && active == "r1", "Short press changed collector");
        button(true);
        button(false);
        check(until([&] { return !workspace.in_overview() && active == "r3"; }), "Physical double-click missed rope");
        line = center(find(workspace.rows(), "r3"));
        check(WindowFromPoint(line) == hwnd && SetCursorPos(line.x, line.y), "Single-rope line is covered");
        button(true);
        check(until([&] { return GetCapture() != nullptr; }), "Rope did not capture hold");
        Sleep(GetDoubleClickTime() + 20);
        line.y += dip(hwnd, 0, 60).y;
        check(SetCursorPos(line.x, line.y) && until([&] { return center(find(workspace.rows(), "r3")).y == line.y; }),
              "Unfocused drag did not move the workspace");
        button(false);
        check(until([&] { return heights.contains("r3"); }) && GetForegroundWindow() == receiver.edit,
              "Release failed to save height or stole focus");
        std::cout << "Physical wheel over card, rope and gap, editor focus, gap click, occlusion, double-click and placement passed\n";
        return;
    }
    for (bool copy : {false, true}) {
        // After the first move the old slot now contains its neighbor. Windows
        // can dispatch DBLCLK as the next physical button-down at those pixels.
        const char *expected = copy ? "r2-3" : "r2-2";
        auto p = local(hwnd, center(find(workspace.slots(), expected)));
        auto q = local(hwnd, center(find(workspace.rows(), "r3")));
        mouse(hwnd, copy ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN, p, MK_LBUTTON);
        mouse(hwnd, WM_MOUSEMOVE, q, MK_LBUTTON | (copy ? MK_CONTROL : 0));
        mouse(hwnd, WM_LBUTTONUP, q, copy ? MK_CONTROL : 0);
        if (!copy) {
            auto &source = data.ids["r2"];
            source.erase(std::find(source.begin(), source.end(), expected));
            workspace.refresh();
        }
    }
    check(moves == 1 && copies == 1 && moved == "r2-3" && target == "r3",
          "Cross-rope move/copy routed incorrectly");
    auto rope = center(find(workspace.rows(), "r3")), card = center(find(workspace.slots(), "r3-0"));
    mouse(hwnd, WM_CONTEXTMENU, card, reinterpret_cast<WPARAM>(hwnd));
    mouse(hwnd, WM_CONTEXTMENU, rope, reinterpret_cast<WPARAM>(hwnd));
    check(card_context == 1 && rope_context == 1, "Card/rope context precedence failed");
    const auto press = local(hwnd, rope);
    const POINT release{press.x, press.y + 1};
    mouse(hwnd, WM_LBUTTONDOWN, press, MK_LBUTTON);
    mouse(hwnd, WM_MOUSEMOVE, release, MK_LBUTTON);
    mouse(hwnd, WM_LBUTTONUP, release, 0);
    check(active == "r1", "Small movement within a row changed the collector");
    mouse(hwnd, WM_LBUTTONDBLCLK, release, MK_LBUTTON);
    check(!workspace.in_overview() && active == "r3" && workspace.rows().front().first == "r3",
          "Rope double-click did not enter single mode");
    SendMessageW(hwnd, WM_COMMAND, static_cast<WPARAM>(cliprope::win::WorkspaceCommand::keyboard), 0);
    check(GetFocus() == hwnd, "Explicit keyboard navigation could not take focus");
    {
        Keys key(0);
        SendMessageW(hwnd, WM_KEYDOWN, VK_TAB, 0);
    }
    check(active == "r4", "Local Tab did not select next");
    {
        Keys key(VK_SHIFT);
        SendMessageW(hwnd, WM_KEYDOWN, VK_TAB, 0);
    }
    check(active == "r4", "Local Shift+Tab changed the collector");
    {
        Keys key(VK_CONTROL);
        SendMessageW(hwnd, WM_KEYDOWN, VK_TAB, 0);
    }
    check(workspace.in_overview(), "Local Ctrl+Tab did not enter overview");
    for (int i = 0; i < 45; ++i)
        wheel(hwnd, center(workspace.rows().front().second), true, false);
    for (const auto &[id, r] : workspace.slots()) {
        auto p = workspace.rope_at(center(r));
        check(p && id.starts_with(*p + "-"), "Reused slot shows an entry for a different rope");
    }
    // Each row realizes its visible cards, one on either side and one partial card.
    RECT client{};
    GetClientRect(hwnd, &client);
    const auto row_bound =
        static_cast<std::size_t>(std::ceil((double(client.right) * 96 / GetDpiForWindow(hwnd) - 28) / 200)) + 3;
    check(!errors && data.largest_rope_page <= 3 && data.largest_card_page <= row_bound &&
              workspace.realized_count() <= workspace.rows().size() * row_bound,
          "Overview allocation bound exceeded");
    {
        Keys key(VK_CONTROL);
        SendMessageW(hwnd, WM_KEYDOWN, VK_TAB, 0);
    }
    check(!workspace.in_overview() && active == "r4" && workspace.rows().front().first == "r4",
          "Ctrl+Tab after scrolling returned to the viewport rather than the collector");
    std::cout << "40 ropes, 4000 initial cards: realized=" << initial
              << "; unchanged after 12000 offscreen inserts. "
              << "Largest rope/card query=" << data.largest_rope_page << "/" << data.largest_card_page
              << ". Wheel, Tab, double-click, stable transfer and rapid slot reuse passed.\n";
}
void card_edge_scrolling() {
    Data data;
    data.compact["r1"] = true;
    auto reads = data.reads();
    int writes{}, reorders{}, transfers{}, errors{};
    bool reject{};
    reads.save_anchor = [&](auto id, const auto& anchor) { ++writes; data.anchors[std::string(id)] = anchor; };
    const auto page = reads.page;
    reads.page = [page](auto id, size_t start, size_t n) {
        auto cards = page(id, start, n);
        if (id == "r1") for (size_t i = 0; i < cards.size(); ++i) {
            cards[i].kind = application::CardKind::image;
            cards[i].source = (start+i)%2 ? domain::ImageMetadata{420,120} : domain::ImageMetadata{100,280};
        }
        return cards;
    };
    win::Workspace* workspace{};
    win::WorkspaceActions actions;
    actions.reorder = [&](const auto& id, size_t index, win::DropCompleted landed) {
        ++reorders;
        if (reject) throw std::runtime_error("Injected reorder failure");
        auto& order = data.ids["r0"];
        check(std::erase(order, id) == 1 && index <= order.size(), "Invalid identity or insertion");
        order.insert(order.begin() + index, id);
        workspace->refresh();
        landed(id);
    };
    size_t insertion{};
    bool copied{};
    actions.transfer = [&](const auto& id, const auto& rope, bool copy, size_t index, win::DropCompleted landed) {
        check(id == "r0-0" && rope == "r1", "Transfer lost the source or target");
        ++transfers;
        insertion = index;
        copied = copy;
        landed({});
    };
    actions.error = [&] { ++errors; };
    win::Workspace shelf(std::move(reads), std::move(actions));
    workspace = &shelf;
    check(shelf.open(), "Could not open edge scrolling fixture");
    shelf.show("r0");
    HWND hwnd = shelf.window();
    RECT bounds{};
    GetClientRect(hwnd, &bounds);
    const auto width = bounds.right * 96. / GetDpiForWindow(hwnd);
    data.ids["r0"].resize(size_t(std::ceil(width / 208)) + 5);
    data.ids["r2"].resize(1);
    shelf.refresh();
    const auto initial = data.ids["r0"];
    const auto clip = GetClipboardSequenceNumber();
    auto edge = [&](const char* rope, bool right) {
        const auto row = local(hwnd, center(find(shelf.rows(), rope)));
        return POINT{right ? bounds.right - 1 : 1, row.y + dip(hwnd, 0, 70).y};
    };
    auto carry = [&](POINT to, bool ctrl = false) {
        auto from = local(hwnd, center(find(shelf.slots(), "r0-0")));
        mouse(hwnd, WM_LBUTTONDOWN, from, MK_LBUTTON);
        mouse(hwnd, WM_MOUSEMOVE, to, MK_LBUTTON | (ctrl ? MK_CONTROL : 0));
        check(FindWindowW(L"ClipRope.DragCard", nullptr), "Card was not lifted");
    };
    auto pause = [](DWORD ms) {
        const auto end = GetTickCount64() + ms;
        until([&] { return GetTickCount64() >= end; });
    };
    auto right = edge("r0", true), left = edge("r0", false);
    carry(right);
    check(until([&] { return writes == 1; }, 6000), "Stationary right edge did not reach the tail");
    shelf.refresh();  // Source is outside the realized page.
    mouse(hwnd, WM_LBUTTONUP, right, 0);
    check(reorders == 1 && data.ids["r0"].back() == "r0-0", "Drop missed the previewed last slot");
    carry(left);
    check(until([&] { return data.anchors["r0"].entry.empty(); }, 6000), "Left edge did not reach the head");
    mouse(hwnd, WM_LBUTTONUP, left, 0);
    check(reorders == 2 && data.ids["r0"] == initial, "Reverse drop missed the head");

    for (UINT cancel : {UINT(WM_KEYDOWN), UINT(WM_CAPTURECHANGED)}) {
        carry(right);
        pause(230);
        const auto saved = writes;
        if (cancel == WM_KEYDOWN) SendMessageW(hwnd, cancel, VK_ESCAPE, 0);
        else ReleaseCapture();
        check(writes == saved + 1 && GetCapture() == nullptr, "Cancellation lost the scroll anchor or capture");
        pause(100);
        check(writes == saved + 1 && data.ids["r0"] == initial, "Canceled drag continued scrolling");
        SendMessageW(hwnd, WM_COMMAND, WPARAM(win::WorkspaceCommand::latest), 0);
    }
    carry(right);
    pause(180);
    reject = true;
    mouse(hwnd, WM_LBUTTONUP, right, 0);
    check(errors == 1 && reorders == 3 && data.ids["r0"] == initial && !GetCapture(), "Rejected drop escaped rollback");
    SendMessageW(hwnd, WM_COMMAND, WPARAM(win::WorkspaceCommand::latest), 0);
    shelf.set_overview(true);
    right = edge("r1", true);
    const auto other = find(shelf.slots(), "r0-1");
    for (bool copy : {false,true}) {
        const auto saved = writes;
        carry(right, copy);
        pause(350);
        check(writes == saved, "Edge timer wrote an anchor per frame");
        // Leave the edge band but keep the same target and stop before committing.
        auto inner = right;
        inner.x -= dip(hwnd, 65, 0).x;
        mouse(hwnd, WM_MOUSEMOVE, inner, MK_LBUTTON | (copy ? MK_CONTROL : 0));
        const auto slots = shelf.slots();
        pause(150);
        const auto stable = shelf.slots();
        const auto sample = std::find_if(slots.begin(), slots.end(), [](const auto& slot) { return slot.first.starts_with("r1-"); });
        check(sample != slots.end() && writes == saved + 1 && slots.size() == stable.size() &&
              find(stable, sample->first).left == sample->second.left, "Middle of target kept scrolling");
        mouse(hwnd, WM_LBUTTONUP, inner, 0);
        check(transfers == (copy ? 2 : 1) && copied == copy && insertion > size_t(width/110),
              "Compact mixed-width transfer missed the offscreen insertion");
        const auto untouched = find(shelf.slots(), "r0-1");
        check(EqualRect(&other, &untouched), "Unrelated row scrolled");
    }
    for (bool copy : {false,true}) {
        win::FloatingActions fa;
        fa.visible_overview = [&] { return shelf.in_overview(); };
        std::optional<size_t> preview, landed;
        fa.preview_at = [&](POINT p, const auto& card) {
            auto slot = shelf.preview_floating(p, &card);
            if (slot) preview = slot->index;
            return slot;
        };
        fa.clear_preview = [&] { shelf.clear_floating_preview(); };
        auto commit = [&](const auto& id, const auto& slot) {
            check(id == "floater" && slot.rope.id == "r1" && !landed, "Invalid Floating drop");
            landed = slot.index;
        };
        if (copy) fa.copy_to_rope = commit;
        else fa.drop = commit;
        application::CardSummary summary;
        summary.entry.id = "floater";
        summary.text = L"Floating sample";
        win::Floating card(std::move(fa));
        check(card.open(summary, win::floating_layout_at({200,450})), "Floating source unavailable");
        HWND f = card.window();
        POINT target = right;
        ClientToScreen(hwnd, &target);
        mouse(f, WM_LBUTTONDOWN, dip(f, 40, 68), MK_LBUTTON);
        mouse(f, WM_MOUSEMOVE, local(f, target), MK_LBUTTON | (copy ? MK_CONTROL : 0));
        const auto first = preview;
        check(first && until([&] { return preview > first; }), "Stationary Floating face did not advance");
        const auto final = preview;
        mouse(f, WM_LBUTTONUP, local(f, target), 0);
        check(landed == final && !GetCapture(), "Floating release missed its last preview");
    }
    carry(right);
    pause(150);
    data.ropes.erase(data.ropes.begin()+1);
    shelf.refresh();
    mouse(hwnd, WM_LBUTTONUP, right, 0);
    check(!GetCapture() && transfers == 2 && reorders == 3, "Removed target rebound to a neighboring rope");
    shelf.set_overview(false);
    shelf.set_active("r2");
    const auto short_card = local(hwnd, center(find(shelf.slots(), "r2-0")));
    const auto saved = writes;
    mouse(hwnd, WM_LBUTTONDOWN, short_card, MK_LBUTTON);
    mouse(hwnd, WM_MOUSEMOVE, edge("r2", true), MK_LBUTTON);
    pause(150);
    shelf.hide();
    check(writes == saved && !GetCapture(), "A short rope scrolled or retained capture on hide");
    check(GetClipboardSequenceNumber() == clip, "Card edge scrolling published clipboard data");
    std::cout << "Stationary edge holds, both limits, target-only scrolling, Floating previews and cancellation passed at "
              << GetDpiForWindow(hwnd) << " DPI\n";
}
void rope_gestures() {
    Data data;
    std::map<std::string, double> tops;
    std::string active = "r0";
    win::Workspace* view{};
    bool fail = false;
    int errors = 0;
    int plaque_menus = 0;
    bool hold_reorder{};
    std::function<void()> resume_reorder;
    win::WorkspaceActions actions;
    actions.rope_top = [&](auto& id) { return tops.contains(id) ? tops.at(id) : 24.; };
    actions.save_rope_top = [&](auto& id, double value) {
        if (fail) throw std::runtime_error("Rejected placement");
        tops[id] = value;
    };
    actions.reorder_rope = [&](auto& from, auto& to, auto& head, win::DropCompleted landed) {
        auto apply = [&, from, to, head, landed = std::move(landed)] {
            if (fail) throw std::runtime_error("Rejected reorder");
            auto h = std::find_if(data.ropes.begin(), data.ropes.end(), [&](auto& r) { return r.id == head; });
            check(h != data.ropes.end() && active == head, "Stale collector in reorder");
            std::rotate(data.ropes.begin(), h, data.ropes.end());
            auto a = std::find_if(data.ropes.begin(), data.ropes.end(), [&](auto& r) { return r.id == from; });
            auto b = std::find_if(data.ropes.begin(), data.ropes.end(), [&](auto& r) { return r.id == to; });
            check(a != data.ropes.end() && b != data.ropes.end(), "Reorder lost identity");
            if (a < b) std::rotate(a, a + 1, b + 1);
            else std::rotate(b, a, a + 1);
            active = data.ropes.front().id;
            landed(active);
        };
        if (hold_reorder) resume_reorder = std::move(apply);
        else apply();
    };
    actions.select = [&](auto& id) { active = id; view->set_active(id); };
    actions.error = [&] { ++errors; };
    actions.rope_menu = [&](const auto& rope, POINT, auto) {
        check(rope.id == active, "Plaque context menu bound another rope");
        ++plaque_menus;
        return win::WorkspaceCommand::none;
    };
    win::Workspace workspace(data.reads(), std::move(actions));
    view = &workspace;
    check(workspace.open(), "Could not open gesture fixture");
    workspace.show(active);
    const auto hwnd = workspace.window();
    auto line = [&](const char* id) { return center(find(workspace.rows(), id)); };
    auto drag = [&](POINT from, POINT to) {
        mouse(hwnd, WM_LBUTTONDOWN, local(hwnd, from), MK_LBUTTON);
        mouse(hwnd, WM_MOUSEMOVE, local(hwnd, to), MK_LBUTTON);
    };
    auto up = [&](POINT p) { mouse(hwnd, WM_LBUTTONUP, local(hwnd, p), 0); };
    auto ctrl = [&](int amount) {
        const auto p = center(workspace.rows().front().second);
        SendMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, amount), MAKELPARAM(p.x, p.y));
    };
    const auto sequence = GetClipboardSequenceNumber();
    auto start = line("r0"), lower = start;
    auto plaque = start;
    plaque.y -= dip(hwnd, 0, int(presentation::RopePlaque::gap + presentation::RopePlaque::height / 2)).y;
    check(WindowFromPoint(plaque) == hwnd, "Plaque body is not interactive");
    mouse(hwnd, WM_CONTEXTMENU, plaque, reinterpret_cast<WPARAM>(hwnd));
    check(plaque_menus == 1, "Plaque did not open the rope menu");
    POINT plaque_lower{plaque.x, plaque.y + dip(hwnd, 0, 48).y};
    drag(plaque, plaque_lower);
    check(line("r0").y == start.y + dip(hwnd, 0, 48).y && tops.empty(), "Plaque did not carry its single rope");
    SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
    check(line("r0").y == start.y, "Plaque drag cancellation kept its height");
    lower.y += dip(hwnd, 0, 68).y;
    click(hwnd, local(hwnd, start));
    check(line("r0").y == start.y && tops.empty(), "A stationary click moved the rope");
    drag(start, lower);
    check(line("r0").y == lower.y && tops.empty(), "Single-rope preview was not provisional");
    SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
    check(line("r0").y == start.y && tops.empty(), "Escape kept a placement");
    drag(start, lower);
    fail = true;
    up(lower);
    fail = false;
    check(line("r0").y == start.y && tops.empty() && errors == 1, "Placement rollback failed");
    drag(start, lower);
    up(lower);
    check(tops.contains("r0") && line("r0").y == lower.y, "Release did not save rope position");
    ctrl(-60);
    check(active == "r0", "Fractional wheel switched prematurely");
    ctrl(-60);
    check(active == "r1" && line("r1").y == start.y, "Next rope inherited the previous height");
    ctrl(120);
    check(active == "r0" && line("r0").y == lower.y, "Previous rope lost its height");
    ctrl(120);
    check(active == "r39", "Previous did not wrap");
    ctrl(-120);
    workspace.hide();
    workspace.show(active);
    check(line("r0").y == lower.y, "Reopening lost rope height");
    workspace.set_overview(true);
    start = line("r0");
    const auto second = line("r1");
    auto third = line("r2");
    const auto card_y = [&](const char* id) { return center(find(workspace.slots(), id)).y; };
    const auto paper_a = card_y("r0-0"), paper_b = card_y("r1-0");
    const auto distance = (second.y - start.y) / 4;
    POINT quarter{start.x, start.y + distance};
    drag(start, quarter);
    check(line("r0").y == quarter.y && card_y("r0-0") == paper_a + distance,
          "Carried rope and cards did not follow the grip between slots");
    check(line("r1").y == second.y - distance && card_y("r1-0") == paper_b - distance &&
          line("r2").y == third.y, "Neighboring rope and cards did not give way during the drag");
    POINT across{start.x, second.y - distance};
    mouse(hwnd, WM_MOUSEMOVE, local(hwnd, across), MK_LBUTTON);
    check(line("r0").y == across.y && line("r1").y == start.y + distance && data.ropes[0].id == "r0",
          "Crossing the midpoint snapped the carried rope or committed early");
    mouse(hwnd, WM_MOUSEMOVE, local(hwnd, quarter), MK_LBUTTON);
    check(line("r1").y == second.y - distance && line("r0").y == quarter.y, "Reversing lost the insertion preview");
    SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
    check(line("r0").y == start.y && line("r1").y == second.y && card_y("r1-0") == paper_b,
          "Escape did not restore displaced ropes and cards");
    drag(start, third);
    check(workspace.rows()[2].first == "r0" && data.ropes[0].id == "r0", "Missing insertion preview");
    ReleaseCapture();
    check(workspace.rows()[0].first == "r0", "Capture loss retained projected order");
    drag(start, third);
    fail = true;
    up(third);
    fail = false;
    check(workspace.rows()[0].first == "r0" && errors == 2, "Failed reorder retained projected order");
    hold_reorder = true;
    drag(start, third);
    up(third);
    check(resume_reorder && active == "r0" && data.ropes[0].id == "r0" && workspace.rows()[0].first == "r0",
          "Deferred reorder exposed a projected collector before commit");
    hold_reorder = false;
    std::exchange(resume_reorder, {})();
    check(data.ropes[0].id == "r1" && data.ropes[1].id == "r2" && data.ropes[2].id == "r0" &&
          active == "r1", "Row move did not insert at target or select its new head");

    RECT bounds{};
    GetWindowRect(hwnd, &bounds);
    POINT edge = third;
    edge.y = bounds.bottom - 2;
    const auto first_hidden = workspace.rows().size();
    drag(line("r0"), edge);
    check(until([&] { return workspace.rows().front().first != "r1"; }), "Edge hold did not scroll");
    workspace.refresh();
    check(std::abs(line("r0").y - edge.y) <= dip(hwnd, 0, 4).y, "Edge scroll or refresh lost the grip");
    up(edge);
    auto reads = data.reads();
    check(reads.rope_position("r0").value() >= first_hidden, "Edge drop could not reach a hidden row");
    check(GetClipboardSequenceNumber() == sequence && errors == 2, "Rope gestures changed the clipboard or reported errors");

    data.ropes.resize(5);
    presentation::Overview list(reads);
    constexpr float row = presentation::Hanging::row;
    list.open(data.ropes.back().id, 900, row * 5);
    check(list.first() == 0 && list.rows().size() == 5, "Overview did not use all available height");
    check(list.rows().front().rope.id == data.ropes.back().id && list.rows()[1].rope.id == data.ropes.front().id,
          "Last saved rope did not wrap the complete overview");
    list.scroll_rows(1, 900, row * 5);
    check(list.first() == 0, "An exact-fit list scrolled");
    list.refresh(900, row * 4);
    list.scroll_rows(1, 900, row * 4);
    list.scroll_rows(1, 900, row * 4);
    check(list.first() == 1 && list.rows().size() == 4, "One overflowing row left a trailing empty page");

    const auto tall = data.ropes.front().id;
    reads.page = [page = reads.page, tall](auto rope, auto offset, auto count) {
        auto cards = page(rope, offset, count);
        if (rope == tall)
            for (auto& card : cards) card.kind = application::CardKind::image;
        return cards;
    };
    presentation::Overview mixed(reads);
    mixed.row_height = 400;
    mixed.open(tall, 900, 1200);
    auto layout = mixed.drag_layout(0, row / 4, 400);
    check(layout.insertion == 0 && layout.tops[1] == 300 && layout.tops[2] == 400 + row,
          "Tall carried row displaced the wrong distance");
    mixed.preview_move(0, 1, 900, 1200);
    layout = mixed.drag_layout(1, 3 * row / 4, 400);
    check(layout.insertion == 1 && layout.tops[0] == 100 && layout.tops[1] == 3 * row / 4,
          "Variable row height changed the grip across the midpoint");
    mixed.clear_move();
    mixed.open(tall, 900, 1200);
    layout = mixed.drag_layout(1, 100, row);
    check(layout.insertion == 0 && layout.tops[0] == 3 * row / 4, "Dragging upward failed to push the tall row down");
}

std::vector<std::uint32_t> count_pixels(HWND hwnd, RECT row) {
    RECT bounds{};
    GetClientRect(hwnd, &bounds);
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), bounds.right, -bounds.bottom, 1, 32, BI_RGB};
    HDC dc = CreateCompatibleDC(nullptr);
    void* bits{};
    const auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    check(dc && bitmap && bits, "Plaque frame buffer unavailable");
    const auto old = SelectObject(dc, bitmap);
    PatBlt(dc, 0, 0, bounds.right, bounds.bottom, BLACKNESS);
    SendMessageW(hwnd, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    GdiFlush();
    auto p = local(hwnd, center(row));
    const auto half = dip(hwnd, 48, 0).x;
    const auto top = p.y - dip(hwnd, 0, 26).y, bottom = p.y - dip(hwnd, 0, 13).y;
    const auto* pixels = static_cast<const std::uint32_t*>(bits);
    std::vector<std::uint32_t> result;
    for (LONG y = top; y < bottom; ++y)
        for (LONG x = p.x - half; x < p.x + half; ++x)
            result.push_back(pixels[y * bounds.right + x] & 0xffffff);
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return result;
}
void plaques(const std::filesystem::path& frame_path = {}) {
    Data data;
    {
        presentation::Overview list(data.reads());
        list.open("r0", 1180, 300);
        list.rows().front().viewport.set_compact(true);
        list.rows().front().viewport.scroll(500);
        const auto anchor = list.rows().front().viewport.anchor();
        list.scroll_rows(20, 1180, 300);
        list.scroll_rows(-20, 1180, 300);
        const auto& restored = list.rows().front().viewport;
        check(restored.compact() && restored.anchor().entry == anchor.entry && restored.anchor().offset == anchor.offset,
              "Virtualizing a rope lost its density or anchor");
        data.compact.clear();
        data.anchors.clear();
    }
    data.ropes.resize(3);
    data.ropes[0].name = "main";
    data.ropes[1].name = "\xe8\xb5\x84\xe6\x96\x99";
    data.ropes[2].name = "A very long rope name that must be ellipsized";
    domain::Id active = "r1", menu;
    win::Workspace* surface{};
    unsigned errors = 0;
    win::WorkspaceActions actions;
    actions.select = [&](auto& id) { active = id; surface->set_active(id); };
    actions.rope_menu = [&](const auto& rope, POINT, auto) { menu = rope.id; return win::WorkspaceCommand::none; };
    actions.error = [&] { ++errors; };
    win::Workspace workspace(data.reads(), std::move(actions));
    surface = &workspace;
    workspace.always_on_top(false);
    check(workspace.open(), "Plaque workspace unavailable");
    auto show = [&] {
        const auto start = std::chrono::steady_clock::now();
        const auto targets = rendering::WorkspacePaint::target_creations();
        workspace.show(active);
        if (!frame_path.empty())
            std::cout << "Single rope show (ms): "
                      << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()
                      << ", render targets created: " << rendering::WorkspacePaint::target_creations() - targets << '\n';
    };
    show();
    workspace.hide();
    show();
    workspace.set_overview(true);
    check(!(GetWindowLongPtrW(workspace.window(), GWL_EXSTYLE) & WS_EX_TOPMOST), "Overview restored topmost");
    workspace.hide();
    workspace.show(active);
    check(!(GetWindowLongPtrW(workspace.window(), GWL_EXSTYLE) & WS_EX_TOPMOST), "Reopening restored topmost");
    workspace.always_on_top(true);
    const auto hwnd = workspace.window();
    check(workspace.rows().size() == 3 && workspace.rows()[0].first == "r1" &&
          workspace.rows()[1].first == "r2" && workspace.rows()[2].first == "r0", "Plaque overview order is not circular");
    SendMessageW(hwnd, WM_COMMAND, static_cast<WPARAM>(win::WorkspaceCommand::density), 0);
    const auto compact = find(workspace.slots(), "r1-0");
    workspace.set_active("r2");
    workspace.set_active("r1");
    const auto retained = find(workspace.slots(), "r1-0");
    check(retained.right - retained.left == compact.right - compact.left, "Collector rotation lost a visible rope's density");
    workspace.set_overview(false);
    const auto single = find(workspace.slots(), "r1-0");
    check(single.right - single.left == compact.right - compact.left, "Returning to single view lost density");
    workspace.set_active("r2");
    const auto regular = find(workspace.slots(), "r2-0");
    check(regular.right - regular.left > compact.right - compact.left, "Density leaked into another single rope");
    workspace.set_active("r1");
    workspace.hide();
    workspace.show("r1");
    workspace.set_overview(true);
    const auto reopened = find(workspace.slots(), "r1-0");
    check(reopened.right - reopened.left == compact.right - compact.left, "Reopening the workspace lost density");
    SendMessageW(hwnd, WM_COMMAND, static_cast<WPARAM>(win::WorkspaceCommand::density), 0);
    if (!frame_path.empty()) workspace_fixture::frame(hwnd, frame_path);
    std::vector<std::vector<std::uint32_t>> before;
    const auto rows = workspace.rows();
    for (const auto& row : rows) before.push_back(count_pixels(hwnd, row.second));
    workspace.set_active("r2");
    check(count_pixels(hwnd, workspace.rows().front().second) == before.front(),
          "Changing the first overview rope altered the total");
    workspace.set_overview(false);
    check(count_pixels(hwnd, workspace.rows().front().second) == before.front(),
          "Switching to single view changed the plaque total");
    workspace.set_active("r1");
    workspace.set_overview(true);
    data.ropes.push_back({"r3", "Added", 1000, false});
    workspace.refresh();
    for (std::size_t i = 0; i < rows.size(); ++i)
        check(count_pixels(hwnd, workspace.rows()[i].second) != before[i], "Plaque total did not update after creation");
    data.ropes.pop_back();
    workspace.refresh();
    for (std::size_t i = 0; i < rows.size(); ++i)
        check(count_pixels(hwnd, find(workspace.rows(), rows[i].first)) == before[i], "Plaque total did not update after deletion");
    auto plaque = center(find(workspace.rows(), "r2"));
    plaque.y -= dip(hwnd, 0, 34).y;
    check(WindowFromPoint(plaque) == hwnd, "Overview plaque does not receive input");
    mouse(hwnd, WM_CONTEXTMENU, plaque, reinterpret_cast<WPARAM>(hwnd));
    check(menu == "r2" && active == "r1", "Plaque menu changed the collector or bound the wrong rope");
    plaque = local(hwnd, plaque);
    click(hwnd, plaque);
    mouse(hwnd, WM_LBUTTONDBLCLK, plaque, MK_LBUTTON);
    check(!workspace.in_overview() && active == "r2" && workspace.rows().front().first == "r2" && !errors,
          "Plaque double-click did not select and open its rope");
    std::cout << "Plaques: circular rows, Chinese/long names, live total pixels, menu and double-click passed\n";
}
} // namespace
int main(int argc, char **argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const bool physical = argc == 2 && std::string_view(argv[1]) == "--physical-wheel";
    int status = 1;
    HDESK private_desk{};
    const auto old = GetThreadDesktop(GetCurrentThreadId());
    POINT cursor{};
    const bool restore = physical && GetCursorPos(&cursor);
    try {
        if (!physical) {
            const auto name = L"ClipRope.OverviewTest." + std::to_wstring(GetCurrentProcessId());
            private_desk = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
            check(private_desk && SetThreadDesktop(private_desk), "Private desktop unavailable");
        }
        if (argc == 2 && std::string_view(argv[1]) == "--card-edge")
            card_edge_scrolling();
        else if (argc == 3 && (std::string_view(argv[1]) == "--plaque-frame" || std::string_view(argv[1]) == "--cloud-frame"))
            plaques(argv[2]);
        else {
            exercise(physical, argc == 3 && std::string_view(argv[1]) == "--frame" ? argv[2] : "");
            if (!physical && argc == 1) { rope_gestures(); plaques(); }
        }
        status = 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
    }
    if (private_desk) {
        SetThreadDesktop(old);
        CloseDesktop(private_desk);
    }
    if (restore)
        SetCursorPos(cursor.x, cursor.y);
    return status;
}
