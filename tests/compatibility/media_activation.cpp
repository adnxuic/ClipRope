#include "desktop_app.hpp"
#include "../clipboard_sources/image_file_sample.hpp"
#include "../clipboard_sources/media_sample.hpp"
#include "platform_win/history_clipboard.hpp"
#include "persistence/hotkey_settings.hpp"
#include "platform_win/card_editor.hpp"

namespace {
using namespace workspace_fixture;
domain::Snapshot snapshot(std::vector<Representation> reps) {
    CaptureResult capture;
    capture.outcome = "captured";
    for (const auto& f : reps)
        capture.formats.push_back({f.format, f.tymed, f.aspect, f.index, "saved"});
    capture.saved = std::move(reps);
    return win::capture_snapshot(std::move(capture));
}
void press(HWND target, POINT at, bool right = false) {
    aim(target, at);
    mouse(right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN);
    mouse(right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP);
}
void details(Desktop& app, const std::string& id) {
    DWORD pid{};
    GetWindowThreadProcessId(app.workspace, &pid);
    press(app.workspace, center(app.card(id)), true);
    HWND popup{};
    require(until([&] {
                popup = owned_window(pid, L"ClipRope.CardMenu");
                return popup && IsWindowVisible(popup);
            }),
            "Card menu missing");
    RECT r{};
    auto item=GetDlgItem(popup,int(win::CardCommand::information));
    require(item && GetWindowRect(item,&r),"Missing card information");
    press(item,center(r));
    HWND dialog{};
    require(until([&] {
                dialog = owned_window(pid, L"ClipRope.CardEditor");
                return dialog && GetDlgItem(dialog, 1701);
            }),
            "Readable details dialog unavailable");
    wchar_t text[2048]{};
    SendDlgItemMessageW(dialog, 1701, WM_GETTEXT, std::size(text), reinterpret_cast<LPARAM>(text));
    require(std::wstring_view(text).find(L"Saved\t") != std::wstring_view::npos &&
                std::wstring_view(text).find(L"PNG\tsaved") != std::wstring_view::npos,
            "UI label disagrees with image-only restore plan");
    SendMessageW(dialog, WM_CLOSE, 0, 0);
}
void file_delivery(const fs::path& exe) {
    Fields fields;
    fields.start();
    for (bool attachment : {false, true}) {
        history_fixture::Directory dir;
        auto path = dir.path / "fixture-picture.png";
        auto png = media_sample::png().bytes;
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<char*>(png.data()), png.size());
        domain::Id id;
        {
            HistoryStore store(dir.path);
            id = store.admit("main", snapshot({image_file_sample::drop({path})}));
        }
        Desktop app(exe, dir.path, true);
        reset_fields(fields);
        if (attachment) fields.act(Action::focus_attachment);
        input_click(app.workspace, app.card(id), fields.window);
        const auto outcome = attachment ? "shortcut_sent" : "no-field";
        require(until([&] {return app.contains("invoke double-click " + id + " published " + outcome + " committed");}),
                "File paste target gate disagreed with control capability");
        if (attachment) require(until([&]{return fields.get(Query::attachments) == 1;}), "File bytes did not reach attachment editor");
        else require(fields.get(Query::edit_attempts) == 0 && fields.get(Query::a_unchanged),
                     "Incompatible edit control received file paste");
    }
}
void exercise(const fs::path& exe) {
    history_fixture::Directory dir;
    domain::Id rich, image;
    {
        HistoryStore store(dir.path);
        persistence::save_shortcut(store.database(), application::workspace_toggle, {true, "Ctrl+Alt+F11"});
        rich = store.admit("main", snapshot(sample::formats(false)));
        image = store.admit("main", snapshot({media_sample::png()}));
    }
    Fields fields;
    fields.start();
    Desktop app(exe, dir.path, true);
    details(app, image);
    for (const int step : {0, 1, 2, 3}) {
        reset_fields(fields);
        if (step == 1)
            fields.act(Action::focus_button);
        if (step == 3)
            fields.act(Action::focus_ansi);
        show(app, fields);
        const auto& id = step >= 2 ? image : rich;
        brief(std::chrono::milliseconds(GetDoubleClickTime() + 20));
        input_click(app.workspace, app.card(id), fields.window);
        const std::string delivery = step == 1 ? "no-field" : "shortcut_sent";
        require(until([&] {
                    return !IsWindowVisible(app.workspace) &&
                           app.contains("invoke double-click " + id + " published " + delivery +
                                        " committed");
                }),
                "Publication, delivery or history outcome mismatch");
        if ((step != 2 && !until([&] { return fields.get(Query::changes_a) == static_cast<unsigned>(step == 0); })) ||
            fields.get(Query::changes_b)) {
            std::cerr << "step=" << step << " changes_a=" << fields.get(Query::changes_a)
                      << " changes_b=" << fields.get(Query::changes_b) << " paste_attempts=" << fields.get(Query::edit_attempts)
                      << " target_foreground=" << (GetForegroundWindow() == fields.window)
                      << " has_unicode=" << IsClipboardFormatAvailable(CF_UNICODETEXT) << '\n';
            throw std::runtime_error("Wrong number of edits");
        }
        require(order(dir.path).front() == id, "Wrong history promotion");
        if (step == 0)
            require(fields.get(Query::a_replaced), "Selected field did not receive the exact text");
        if (step == 1) {
            fields.act(Action::focus_b);
            input_paste(fields.window);
            require(until([&] { return fields.get(Query::b_pasted) != 0; }),
                    "Manual paste did not receive the publication");
        }
        if (step == 2) {
            require(until([&] { return fields.get(Query::edit_attempts) == 1; }), "Plain Edit missed the image shortcut");
            const bool kept = fields.get(Query::a_unchanged) != 0;
            const bool removed = fields.get(Query::a_without_selection) != 0;
            const auto edits = fields.get(Query::changes_a);
            require(kept || removed, "Unexpected text after image paste");
            reset_fields(fields);
            input_paste(fields.window);
            require(until([&] { return fields.get(Query::edit_attempts) == 1; }) &&
                    (fields.get(Query::a_unchanged) != 0) == kept &&
                    (fields.get(Query::a_without_selection) != 0) == removed && fields.get(Query::changes_a) == edits,
                    "Image gesture differed from manual Ctrl+V in plain Edit");
            std::cout << "Plain Edit image: kept-selection-text=" << kept << " removed-selection-text=" << removed
                      << " change-events=" << edits << "; manual result matched\n";
        }
        if (step == 3)
            require(until([&] { return fields.get(Query::ansi_png_pastes) == 1; }),
                    "ANSI Edit attachment handler did not receive the published image");
    }
    std::cout << "Restore-details dialog showed readable PNG. Text selected=1 edit; no field=0 then native "
                 "paste; plain Edit image matched manual; ANSI attachment handler=1 paste; 4 history commits.\n";
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc == 3 && std::wstring_view(argv[1]) == L"--fields")
        return fixture::run_fields(wcstoul(argv[2], nullptr, 10));
    if (argc != 3 || std::wstring_view(argv[2]) != L"--desktop-input" || FAILED(OleInitialize(nullptr)))
        return 2;
    POINT pos{};
    bool restore = GetCursorPos(&pos) != FALSE;
    int rc = 0;
    try {
        exercise(argv[1]);
        file_delivery(argv[1]);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        rc = 1;
    }
    if (restore)
        SetCursorPos(pos.x, pos.y);
    OleUninitialize();
    return rc;
}
