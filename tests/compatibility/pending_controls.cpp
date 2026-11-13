#include "../integration/clipboard_process.hpp"
#include "../integration/history_fixture.hpp"
#include "platform_win/card_editor.hpp"
#include "platform_win/color_picker.hpp"
#include "management_frame.hpp"
#include "platform_win/rope_manager.hpp"
#include "platform_win/settings.hpp"
#include <ole2.h>
#include <iostream>
#include <sstream>

namespace {
using namespace cliprope;
using history_fixture::require;
namespace fs = std::filesystem;

using management_fixture::frame;

struct Queue {
    std::function<void()> waiting;
    win::DeferHistoryCommand defer() {
        return [this](auto command) { require(!waiting, "Duplicate submission"); waiting = std::move(command); return true; };
    }
    void run() {
        require(bool(waiting), "No submitted management command");
        std::exchange(waiting, {})();
    }
};

void rope_cases(HWND owner, const fs::path& output) {
    Queue queue;
    int revision{}, writes{};
    domain::Rope rope;
    rope.id = "fixture";
    rope.name = "Fixture rope";
    win::RopeActions actions;
    actions.defer_history = queue.defer();
    actions.preview = [&](const auto& change) {
        application::RopePreview result{change};
        result.stamp = std::to_string(revision);
        result.hanging = 2 + revision;
        return result;
    };
    actions.apply = [&](const auto& approved) -> std::optional<domain::Id> {
        if (approved.change.rope != rope.id || approved.stamp != std::to_string(revision))
            throw std::runtime_error("Changed tail requires new review");
        ++writes;
        return {};
    };
    win::RopeManager manager(std::move(actions));
    manager.open(owner, rope, application::RopeOp::clear);
    const auto dialog = FindWindowW(L"ClipRope.RopeManager", nullptr);
    require(dialog, "Missing rope confirmation");
    SendMessageW(dialog, WM_COMMAND, IDOK, 0); // Review.
    queue.run();
    SendMessageW(dialog, WM_COMMAND, IDOK, 0); // Confirm.
    require(queue.waiting && !IsWindowEnabled(GetDlgItem(dialog, IDOK)) && !writes, "Rope closed before commit");
    frame(dialog, output / "rope-pending.bmp");
    ++revision;
    queue.run();
    require(IsWindow(dialog) && IsWindowEnabled(GetDlgItem(dialog, IDOK)) && !writes, "Stale consent applied");
    frame(dialog, output / "rope-rejected.bmp");
    SendMessageW(dialog, WM_COMMAND, IDOK, 0);
    queue.run();
    SendMessageW(dialog, WM_COMMAND, IDOK, 0);
    queue.run();
    require(!IsWindow(dialog) && writes == 1, "Fresh consent did not commit exactly once");
    manager.open(owner, rope, application::RopeOp::clear);
    const auto cancelled = FindWindowW(L"ClipRope.RopeManager", nullptr);
    SendMessageW(cancelled, WM_COMMAND, IDOK, 0);
    queue.run();
    SendMessageW(cancelled, WM_COMMAND, IDOK, 0);
    SendMessageW(cancelled, WM_COMMAND, IDCANCEL, 0);
    queue.run();
    require(!IsWindow(cancelled) && writes == 1, "Cancelled dialog applied its queued draft");
}

struct PickerDriver {
    fs::path output;
    Queue pending;
    int commits{}, cancels{};
    bool reject{}, dismiss{};
    std::exception_ptr error;
    static void CALLBACK step(HWND owner, UINT, UINT_PTR timer, DWORD) {
        auto& self = *reinterpret_cast<PickerDriver*>(GetWindowLongPtrW(owner, GWLP_USERDATA));
        const auto picker = FindWindowW(L"ClipRope.ColorPicker", nullptr);
        if (!picker) return;
        KillTimer(owner, timer);
        try {
            HWND body{};
            while ((body = FindWindowExW(picker, body, L"STATIC", nullptr)) && !GetDlgItem(body, 4102)) {}
            require(body, "Missing color panel");
            SendDlgItemMessageW(body, 4102, BM_CLICK, 0, 0);
            frame(picker, self.output / "picker-draft.bmp");
            SendMessageW(picker, WM_COMMAND, IDOK, 0);
            require(self.pending.waiting && !self.commits && !IsWindowEnabled(GetDlgItem(picker, IDOK)),
                    "Color picker reported a queued save as committed");
            frame(picker, self.output / "picker-pending.bmp");
            if (self.dismiss) { SendMessageW(picker, WM_CLOSE, 0, 0); return; }
            self.reject = true;
            self.pending.run();
            require(!self.commits && IsWindowEnabled(GetDlgItem(picker, IDOK)), "Failed color save discarded its draft");
            frame(picker, self.output / "picker-rejected.bmp");
            self.reject = false;
            SendMessageW(picker, WM_COMMAND, IDOK, 0);
            self.pending.run();
        } catch (...) {
            self.error = std::current_exception();
            SendMessageW(picker, WM_CLOSE, 0, 0);
        }
    }
    void run(HWND owner) {
        SetWindowLongPtrW(owner, GWLP_USERDATA, LONG_PTR(this));
        const auto anchor = CreateWindowW(L"BUTTON", L"Color", WS_CHILD | WS_VISIBLE, 15, 15, 96, 30,
                                          owner, nullptr, nullptr, nullptr);
        for (bool cancel : {false, true}) {
            commits = cancels = 0;
            dismiss = cancel;
            win::ColorPickerOptions options;
            options.defer_history = pending.defer();
            const domain::ColorSelection selected = options.presets.at(1);
            options.callbacks.commit = [&](auto value) {
                require(value == selected, "Color save lost bound RGBA value");
                if (reject) throw std::runtime_error("Injected color failure");
                ++commits;
                return presentation::ColorCommit::saved;
            };
            options.callbacks.cancel = [&] { ++cancels; };
            require(SetTimer(owner, 1, 60, step), "Could not drive picker");
            const auto applied = win::ColorPickerPopup::track(owner, anchor, std::move(options));
            KillTimer(owner, 1);
            if (error) std::rethrow_exception(error);
            if (cancel) pending.run(); // The popup and its stack no longer exist.
            require(applied == !cancel && commits == int(!cancel) && cancels == int(cancel), "Incorrect picker outcome");
        }
        DestroyWindow(anchor);
    }
};

struct ExclusionDriver {
    Queue& queue;
    fs::path output;
    std::exception_ptr error;
    bool reject = true;
    static void CALLBACK step(HWND owner, UINT, UINT_PTR timer, DWORD) {
        auto& self = *reinterpret_cast<ExclusionDriver*>(GetWindowLongPtrW(owner, GWLP_USERDATA));
        const auto dialog = FindWindowW(L"#32770", nullptr);
        if (!dialog || !GetDlgItem(dialog, 1977)) return;
        KillTimer(owner, timer);
        try {
            SendDlgItemMessageW(dialog, 1972, WM_SETTEXT, 0, LPARAM(L"synthetic.exe"));
            SendDlgItemMessageW(dialog, IDOK, BM_CLICK, 0, 0);
            SendDlgItemMessageW(dialog, 1977, BM_CLICK, 0, 0);
            require(self.queue.waiting && !IsWindowEnabled(GetDlgItem(dialog, 1977)), "Exclusion save acknowledged early");
            frame(dialog, self.output / "exclusions-pending.bmp");
            self.queue.run();
            require(IsWindowEnabled(GetDlgItem(dialog, 1977)) && SendDlgItemMessageW(dialog, 1975, LB_GETCOUNT, 0, 0) == 1,
                    "Failed exclusion save lost the draft");
            frame(dialog, self.output / "exclusions-rejected.bmp");
            self.reject = false;
            SendDlgItemMessageW(dialog, 1977, BM_CLICK, 0, 0);
            self.queue.run();
        } catch (...) { self.error = std::current_exception(); EndDialog(dialog, IDCANCEL); }
    }
};

struct PropertyMenus {
    fs::path output;
    Queue queue;
    bool rope{}, reject = true;
    int writes{};
    std::exception_ptr error;

    void visible_status(HWND menu, const char* state) {
        auto status = GetDlgItem(menu, 9000);
        RECT row{}, client{}, bounds{};
        GetWindowRect(status, &row);
        GetWindowRect(menu, &bounds);
        MapWindowPoints(nullptr, menu, reinterpret_cast<POINT*>(&row), 2);
        GetClientRect(menu, &client);
        MONITORINFO display{sizeof(display)};
        const auto have_monitor = GetMonitorInfoW(MonitorFromWindow(menu, MONITOR_DEFAULTTONEAREST), &display);
        const bool shown = (GetWindowLongPtrW(status, GWL_STYLE) & WS_VISIBLE) != 0;
        const bool effective = IsWindowVisible(status) != FALSE;
        const auto contained = row.left >= 0 && row.right <= client.right && row.right > row.left &&
                               row.top >= 0 && row.bottom <= client.bottom && row.bottom > row.top;
        std::ostringstream details;
        details << (rope ? "rope" : "card") << "-menu " << state << " station="
                  << (management_fixture::record_frames ? "composited-desktop" : "private-clipboard")
                  << " status-shown=" << shown << " effective=" << effective << " work="
                  << display.rcWork.left << ',' << display.rcWork.top << ',' << display.rcWork.right << ',' << display.rcWork.bottom
                  << " menu=" << bounds.left << ',' << bounds.top << ',' << bounds.right << ',' << bounds.bottom
                  << " client=" << client.right << 'x' << client.bottom << " footer="
                  << row.left << ',' << row.top << ',' << row.right << ',' << row.bottom;
        std::cout << details.str() << '\n';
        const auto failure = win::tr(win::TextKey::error_item_save);
        const auto expected = std::string_view(state) == "pending" ? win::tr(win::TextKey::common_saving) :
            rope ? win::message(win::TextKey::popup_saved_value, {{"error", failure}, {"value", L"20"}}) : failure;
        if (!status || !shown || win::fluent::caption(status) != expected.view())
            throw std::runtime_error("Property popup did not expose the expected result text: " + details.str());
        if (client.right > 0 && client.bottom > 0 && !contained)
            throw std::runtime_error("Property status extends outside client: " + details.str());
        if (management_fixture::record_frames && (!have_monitor || !effective || !contained))
            throw std::runtime_error("Property status not fully visible on composited desktop: " + details.str());
    }
    static void CALLBACK step(HWND owner, UINT, UINT_PTR timer, DWORD) {
        auto& self = *reinterpret_cast<PropertyMenus*>(GetWindowLongPtrW(owner, GWLP_USERDATA));
        const auto menu = FindWindowW(self.rope ? L"ClipRope.RopeMenu" : L"ClipRope.CardMenu", nullptr);
        if (!menu) return;
        KillTimer(owner, timer);
        try {
            const std::string prefix = self.rope ? "rope-menu-" : "card-menu-";
            const int capacity_increase = 30031; // Fourth property row, "+" stepper.
            const int id = self.rope ? capacity_increase : int(win::CardCommand::bold);
            auto click = [&] { SendDlgItemMessageW(menu, id, BM_CLICK, 0, 0); };
            frame(menu, self.output / (prefix + "before.bmp"));
            click();
            click();
            require(self.queue.waiting && !self.writes, "Menu acknowledged before commit or resubmitted");
            self.visible_status(menu, "pending");
            frame(menu, self.output / (prefix + "pending.bmp"));
            self.queue.run();
            require(!self.writes, "Rejected menu update changed its target");
            self.visible_status(menu, "rejected");
            frame(menu, self.output / (prefix + "rejected.bmp"));
            self.reject = false;
            click();
            self.queue.run();
            require(self.writes == 1 && !(GetWindowLongPtrW(GetDlgItem(menu, 9000), GWL_STYLE) & WS_VISIBLE),
                    "Retry did not clear status");
            require(self.rope || SendDlgItemMessageW(menu, id, BM_GETCHECK, 0, 0) == BST_CHECKED,
                    "Saved card toggle is not selected");
        } catch (...) { self.error = std::current_exception(); }
        SendMessageW(menu, WM_CLOSE, 0, 0);
    }
    void run(HWND owner) {
        SetWindowLongPtrW(owner, GWLP_USERDATA, LONG_PTR(this));
        for (const bool rope_menu : {false, true}) {
            rope = rope_menu;
            writes = 0;
            reject = true;
            require(SetTimer(owner, 1, 60, step), "Cannot drive property popup");
            if (rope) {
                domain::Rope fixture;
                fixture.id = "menu-rope";
                fixture.name = "Fixture rope";
                fixture.capacity = 20;
                win::RopeActions actions;
                actions.defer_history = queue.defer();
                actions.current = [&](const auto& id) {
                    require(id == fixture.id, "Rope menu changed target");
                    return fixture;
                };
                actions.preview = [](const auto& change) { return application::RopePreview{change}; };
                actions.apply = [&](const auto& proposal) -> std::optional<domain::Id> {
                    require(proposal.change.rope == fixture.id && proposal.change.capacity == 21,
                            "Rope menu changed its bound draft");
                    if (reject) throw std::runtime_error("Injected rope property rejection");
                    fixture.capacity = proposal.change.capacity;
                    ++writes;
                    return {};
                };
                win::RopeManager manager(std::move(actions));
                manager.menu(owner, {80, 80}, fixture, {});
            } else {
                application::CardSummary card;
                card.entry.id = "menu-card";
                card.menu = application::CardMenuKind::text;
                win::EditorActions actions;
                actions.defer_history = queue.defer();
                actions.current = [&](const auto& entry) {
                    require(entry.id == card.entry.id, "Card menu changed target");
                    return card;
                };
                actions.save_style = [&](std::string_view id, const auto& style, bool) {
                    require(id == card.entry.id && style.bold, "Card menu changed its bound draft");
                    if (reject) throw std::runtime_error("Injected card property rejection");
                    card.style = style;
                    ++writes;
                };
                win::CardEditor editor(std::move(actions));
                editor.menu(owner, {80, 80}, application::CardMenuSnapshot(card));
            }
            KillTimer(owner, 1);
            if (error) std::rethrow_exception(error);
            require(writes == 1, "Property menu closed before the retry");
        }
    }
};

void settings_frames(HWND owner, const fs::path& output) {
    Queue queue;
    bool reject = true;
    ExclusionDriver exclusions{queue, output};
    win::SettingsSnapshot::Exclusions saved;
    win::SettingsSnapshot model;
    model.defer_history = queue.defer();
    model.save_cards = [&](const auto& proposed) {
        if (reject) throw std::runtime_error("Injected preference rejection");
        require(!proposed.follow_card_head, "Retry lost the submitted preference");
    };
    model.read_exclusions = [&] { return saved; };
    model.save_exclusions = [&](const auto& original, auto names) {
        if (exclusions.reject || saved != original) throw std::runtime_error("Injected exclusion rejection");
        saved = std::move(names);
    };
    win::Settings page;
    page.open(std::move(model));
    const HWND settings = FindWindowW(L"ClipRope.Settings", nullptr);
    require(settings, "Settings window missing");
    SendDlgItemMessageW(settings, 1501, BM_CLICK, 0, 0);
    const auto viewport = FindWindowExW(settings, nullptr, L"ClipRope.SettingsViewport", nullptr);
    const HWND follow = GetDlgItem(viewport, 1927);
    require(follow, "Card preference missing");
    frame(settings, output / "settings-before.bmp");
    SendMessageW(follow, BM_CLICK, 0, 0);
    require(queue.waiting && !IsWindowEnabled(viewport) && SendMessageW(follow, BM_GETCHECK, 0, 0) == BST_CHECKED,
            "Pending settings frame lost its original value");
    frame(settings, output / "settings-pending.bmp");
    queue.run();
    require(IsWindowEnabled(viewport) && SendMessageW(follow, BM_GETCHECK, 0, 0) == BST_CHECKED,
            "Rejected preference did not restore its value");
    frame(settings, output / "settings-rejected.bmp");
    reject = false;
    SendMessageW(follow, BM_CLICK, 0, 0);
    queue.run();
    require(SendMessageW(follow, BM_GETCHECK, 0, 0) == BST_UNCHECKED, "Successful retry did not update preference");
    frame(settings, output / "settings-saved.bmp");
    SendDlgItemMessageW(settings, 1505, BM_CLICK, 0, 0);
    SetWindowLongPtrW(owner, GWLP_USERDATA, LONG_PTR(&exclusions));
    require(SetTimer(owner, 1, 60, ExclusionDriver::step), "Cannot drive exclusions dialog");
    SendDlgItemMessageW(viewport, 1944, BM_CLICK, 0, 0);
    KillTimer(owner, 1);
    if (exclusions.error) std::rethrow_exception(exclusions.error);
    require(saved == std::vector<std::string>{"synthetic.exe"}, "Exclusion retry did not commit the saved name");
    page.close();
}
}

int wmain(int argc, wchar_t** argv) {
    const bool visual = argc == 3 && std::wstring_view(argv[1]) == L"--visual";
    if (!visual && (argc != 2 || !probe::private_station())) return 2;
    const auto original = GetThreadDesktop(GetCurrentThreadId());
    HDESK desktop{};
    if (visual) {
        const auto name = L"ClipRope.ManagementVisuals." + std::to_wstring(GetCurrentProcessId());
        desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (!desktop || !SetThreadDesktop(desktop)) return 2;
        management_fixture::record_frames = true;
    }
    if (FAILED(OleInitialize(nullptr))) return 2;
    int code{};
    try {
        const auto output = fs::absolute(argv[visual ? 2 : 1]);
        fs::create_directories(output);
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        localization::LocalizationManager language;
        language.set_language(localization::Language::automatic, win::preferred_ui_locale());
        win::UiLocalization binding(language);
        HWND owner = CreateWindowW(L"STATIC", L"Control fixture", WS_OVERLAPPEDWINDOW, 20, 20, 300, 170,
                                   nullptr, nullptr, nullptr, nullptr);
        require(owner != nullptr, "Fixture owner unavailable");
        ShowWindow(owner, SW_SHOWNOACTIVATE);
        rope_cases(owner, output);
        PickerDriver driver{output};
        driver.run(owner);
        PropertyMenus menus{output};
        menus.run(owner);
        settings_frames(owner, output);
        DestroyWindow(owner);
        std::cout << "Bound rope consent/rejection/retry/cancel and pending color draft/rollback/retry/cancel passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; code = 1; }
    OleUninitialize();
    if (desktop) { SetThreadDesktop(original); CloseDesktop(desktop); }
    return code;
}
