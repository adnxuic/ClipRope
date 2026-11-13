#include "persistence/history_store.hpp"
#include "rendering/card_browser.hpp"
#include "../compatibility/desktop_app.hpp"
#include "persistence/hotkey_settings.hpp"
#include "platform_win/card_editor.hpp"
#include "platform_win/menu_timing.hpp"
#include "platform_win/tray.hpp"
#include "platform_win/tray_popup.hpp"
#include "platform_win/workspace.hpp"
#include "../clipboard_sources/image_file_sample.hpp"
#include "../clipboard_sources/media_sample.hpp"
#include "../clipboard_sources/virtual_sample.hpp"

namespace {
using namespace workspace_fixture;
RECT bounds(HWND hwnd) {
    RECT r{};
    require(GetWindowRect(hwnd, &r) != FALSE, "Missing editor fixture geometry");
    return r;
}
void press(HWND target, POINT p, bool right = false) {
    aim(target, p);
    mouse(right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN);
    mouse(right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP);
}
void button(HWND editor, int id) {
    const auto control = GetDlgItem(editor, id);
    require(control != nullptr, "Missing editor control");
    press(control, center(bounds(control)));
}
std::wstring read(HWND h) {
    wchar_t value[512]{};
    SendMessageW(h, WM_GETTEXT, 512, reinterpret_cast<LPARAM>(value));
    return value;
}
void require_empty_caret(HWND field);
using Command = win::CardCommand;
HWND select_command(HWND surface, POINT p, Command command, const std::function<void(HWND)>& inspect = {}) {
    DWORD pid{};
    GetWindowThreadProcessId(surface, &pid);
    press(surface, p, true);
    HWND popup{};
    require(until([&] {
                popup = owned_window(pid, L"ClipRope.CardMenu");
                return popup && IsWindowVisible(popup);
            }),
            "Card context menu unavailable");
    auto item=GetDlgItem(popup,int(command));
    require(item,"Missing card command");
    if (inspect) inspect(popup);
    if (command == Command::foreground || command == Command::background) {
        press(item, center(bounds(item)));
        require(until([&] { auto picker=owned_window(pid, L"ClipRope.ColorPicker"); return picker&&IsWindowVisible(picker)&&GetForegroundWindow()==picker; }),
                "Card picker did not open");
        require(IsWindowVisible(popup), "Parent menu closed when picker took focus");
        return popup;
    }
    if (command == Command::font || command == Command::opacity) {
        press(item,center(bounds(item)));
        auto edit=GetDlgItem(popup,10000+int(command));
        press(edit,center(bounds(edit)));
        return popup;
    }
    press(item,center(bounds(item)));
    if (command == Command::expand) return {};
    if(command==Command::bold) {
        key(VK_ESCAPE,true);key(VK_ESCAPE,false);
        require(until([&]{return !IsWindow(popup);}),"Menu did not close");
        return {};
    }
    const bool inline_edit = command == Command::edit;
    HWND editor{};
    require(until([&] {
                editor = owned_window(pid, inline_edit ? L"ClipRope.InlineEditorHost" : L"ClipRope.CardEditor");
                return editor && IsWindowVisible(editor);
            }),
            "Modeless card editor unavailable");
    return editor;
}
void set(HWND editor, int id, const wchar_t *value) {
    require(SendDlgItemMessageW(editor, id, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(value)) != FALSE,
            "Could not enter synthetic field");
}
void saved(HWND editor) {
    button(editor, IDOK);
    require(until([&] { return !IsWindowVisible(editor) && !GetWindow(editor, GW_OWNER); }), "Valid save did not finish");
}
void check_menu(HWND surface, POINT at, const std::vector<std::wstring>& expected) {
    DWORD pid{};
    GetWindowThreadProcessId(surface, &pid);
    press(surface, at, true);
    HWND popup{};
    require(until([&] { popup = owned_window(pid, L"ClipRope.CardMenu"); return popup && IsWindowVisible(popup); }),
            "Card popup unavailable");
    // The single-path fixtures are local. Their first frame may precede classification.
    require(until([&] { return read(popup).find(L"File reference") == std::wstring::npos; }),
            "Local file menu did not finish classification");
    size_t count=0;
    for(auto ctrl=GetWindow(popup,GW_CHILD);ctrl;ctrl=GetWindow(ctrl,GW_HWNDNEXT)) {
        auto id=GetDlgCtrlID(ctrl);
        if(id<int(Command::font) || id>int(Command::expand)) continue;
        const auto label = read(ctrl);
        if (count >= expected.size() || label != expected[count]) {
            std::wcerr << L"Menu '" << read(popup) << L"' row " << count << L" has '" << label
                       << L"', expected '" << (count < expected.size() ? expected[count] : L"<end>") << L"'\n";
            throw std::runtime_error("Incorrect card menu label or position");
        }
        ++count;
    }
    require(count==expected.size(),"Unexpected card menu size");
    key(VK_ESCAPE, true);
    key(VK_ESCAPE, false);
    require(until([&] { return !IsWindow(popup) || !IsWindowVisible(popup); }), "Card menu stayed open");
}
void image_preferences(const fs::path& exe) {
    history_fixture::Directory dir;
    domain::Id image;
    {
        HistoryStore store(dir.path);
        BITMAPINFOHEADER info{sizeof(info), 512, 512, 1, 32, BI_RGB};
        domain::Format dib;
        dib.standard_id = CF_DIB;
        dib.bytes.resize(sizeof(info) + 512 * 512 * 4, 255);
        std::memcpy(dib.bytes.data(), &info, sizeof(info));
        image = store.admit("main", {{dib}});
        store.detach(store.copy(image, "main"), {"", 400, 240, 240, 160});
    }
    auto sizes = [&](Desktop& host, int side) {
        const auto dpi = GetDpiForWindow(host.workspace);
        return until([&] {
            const auto rect = host.card(image);
            const auto floating = host.floating_window();
            if (!floating) return false;
            const auto window = bounds(floating);
            return std::abs(rect.right - rect.left - MulDiv(side, dpi, 96)) <= 1 &&
                   std::abs(window.bottom - window.top - MulDiv(side + 38, GetDpiForWindow(floating), 96)) <= 1;
        });
    };
    {
        Desktop host(exe, dir.path, true);
        require(sizes(host, 144), "Initial Hanging or Floating image size is incorrect");
        DWORD pid{};
        GetWindowThreadProcessId(host.workspace, &pid);
        SendMessageW(owned_window(pid, L"ClipRope.Listener"), WM_COMMAND, 30, 0);
        const auto settings = owned_window(pid, L"ClipRope.Settings");
        SendDlgItemMessageW(settings, 1504, BM_CLICK, 0, 0);
        auto page = FindWindowExW(settings, nullptr, L"ClipRope.SettingsViewport", nullptr);
        require(SendDlgItemMessageW(page, 1900, BM_GETCHECK, 0, 0) == BST_CHECKED,
                "Unconfigured profile did not select Light");
        SendDlgItemMessageW(page,1901,BM_CLICK,0,0);
        require(SendDlgItemMessageW(page,1901,BM_GETCHECK,0,0)==BST_CHECKED && sizes(host,144),
                "Dark theme failed to apply independently of card dimensions");
    }
    Desktop reopened(exe, dir.path, true);
    require(sizes(reopened, 144), "Restart changed the card dimensions");
}
void matrix(const fs::path& exe) {
    image_preferences(exe);
    history_fixture::Directory root;
    const auto single = root.path / "actual.png", directory = root.path / "folder.zip";
    const auto png = media_sample::png();
    std::ofstream(single, std::ios::binary).write(reinterpret_cast<const char*>(png.bytes.data()), png.bytes.size());
    fs::create_directory(directory);
    struct Case { Representation value; std::vector<std::wstring> menu; };
    const std::vector<std::wstring> group{L"Show in File Explorer", L"Refresh file preview", L"Expand file group", L"Card information…"};
    const std::vector<Case> cases{
        {sample::formats(false).front(), {L"Font size",L"Bold",L"Text color",L"Background color",L"Opacity",L"Edit text…",L"Card information…"}},
        {png, {L"Open original file",L"Show in File Explorer",L"Card information…"}},
        {image_file_sample::drop({single}), {L"Open original file",L"Show in File Explorer",L"Refresh file preview",L"Card information…"}},
        {image_file_sample::drop({single,directory}), group},
        {image_file_sample::drop({directory}), {L"Show in File Explorer",L"Refresh file preview",L"Card information…"}},
        {sample::virtual_group().front(), {L"Card information…"}},
        {sample::formats(false)[1], {L"Card information…"}}
    };
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto profile = root.path / std::to_string(i);
        domain::Id id;
        {
            HistoryStore store(profile);
            store.create_rope("Overview", 10);
            domain::Format format;
            const auto& v = cases[i].value;
            if (v.format < 0xc000) format.standard_id = v.format;
            else if (v.format == cliprope::virtual_files::descriptor_format()) format.name = "FileGroupDescriptorW";
            else format.name = v.format == RegisterClipboardFormatW(L"PNG") ? "PNG" : "HTML Format";
            format.bytes = v.bytes;
            id = store.admit("main", {{format}});
        }
        auto seq = GetClipboardSequenceNumber();
        {
            Desktop app(exe, profile, true);
            auto card_point=[&] {
                // Transparent regions in image previews and the paper's decorative edge pass input through.
                POINT result{};
                require(until([&] {
                    const auto r=app.card(id);
                    for(int y:{50,30,70,18}) for(int x:{50,30,70}) {
                        POINT p{r.left+(r.right-r.left)*x/100,r.top+(r.bottom-r.top)*y/100};
                        if(WindowFromPoint(p)==app.workspace){result=p;return true;}
                    }
                    return false;
                }),"Card has no visible input surface");
                return result;
            };
            check_menu(app.workspace, card_point(), cases[i].menu);
            SendMessageW(app.workspace, WM_COMMAND, static_cast<WPARAM>(win::WorkspaceCommand::overview), 0);
            require(until([&] { return app.frame().rows.size() == 2; }), "Missing overview rows");
            check_menu(app.workspace, card_point(), cases[i].menu);
        }
        { HistoryStore store(profile); store.detach(id, {"", 420, 280, 300, 180}); }
        {
            Desktop app(exe, profile);
            HWND floating = app.floating_window();
            require(floating != nullptr, "Floating fixture missing");
            // File metadata can resize a newly restored Floating image. The
            // toolbar stays at the persisted origin across that resize.
            const auto rect = bounds(floating);
            const auto dpi = GetDpiForWindow(floating);
            check_menu(floating, {rect.left + MulDiv(96, dpi, 96), rect.top + MulDiv(17, dpi, 96)}, cases[i].menu);
        }
        require(seq == GetClipboardSequenceNumber(), "Inspecting a card changed the clipboard");
    }
}
void pencil_controls(const fs::path& exe) {
    history_fixture::Directory profile;
    domain::Id id, copy;
    {
        HistoryStore store(profile.path);
        domain::Format text;
        text.standard_id = CF_UNICODETEXT;
        text.bytes = sample::formats(false).front().bytes;
        id = store.admit("main", {{text}});
        copy = store.copy(id, "main");
        store.detach(copy, {"", 620, 380, 240, 160});
        exec(store.database().handle(), "CREATE TRIGGER reject_clear BEFORE UPDATE OF snapshot_id ON entries "
             "WHEN OLD.state='Floating' BEGIN SELECT RAISE(ABORT,'test empty draft'); END");
    }
    Desktop app(exe, profile.path, true);
    const auto paper = app.card(id);
    const auto floating = app.floating_window();
    const auto pin = bounds(floating);
    const auto dpi = GetDpiForWindow(app.workspace);
    auto px = [dpi](int dip) { return MulDiv(dip, dpi, 96); };
    DWORD pid{};
    GetWindowThreadProcessId(floating, &pid);
    HWND host{};
    auto pencil = [&](HWND owner, POINT pt) {
        press(owner, pt);
        require(until([&] {
            host = owned_window(pid, L"ClipRope.InlineEditorHost");
            return host && IsWindowVisible(host) && GetWindow(host, GW_OWNER) == owner;
        }), "Pencil did not edit its card");
        const auto area = bounds(host);
        require(bounds(GetDlgItem(host, IDOK)).top > area.top + (area.bottom - area.top) / 2 &&
                bounds(GetDlgItem(host, IDCANCEL)).top == bounds(GetDlgItem(host, IDOK)).top,
                "Save and Cancel did not occupy the footer");
    };
    const auto sequence = GetClipboardSequenceNumber();
    pencil(app.workspace, {paper.right - px(12), paper.top - px(9)});
    const auto field = GetDlgItem(host, 1760);
    auto clear_text = [&] {
        SendMessageW(field, EM_SETSEL, 0, -1);
        SendMessageW(field, WM_CLEAR, 0, 0);
        require_empty_caret(field);
    };
    clear_text();
    std::wstring lines;
    for (int i = 0; i < 28; ++i) lines += L"Select this line\r\n";
    set(host, 1760, lines.c_str());
    const auto area = bounds(field);
    aim(field, {area.left + px(3), area.top + px(9)});
    mouse(MOUSEEVENTF_LEFTDOWN);
    SetCursorPos(area.left + px(55), area.top + px(9));
    brief(60ms);
    mouse(MOUSEEVENTF_LEFTUP);
    const auto selection = SendMessageW(field, EM_GETSEL, 0, 0);
    require(HIWORD(selection) > LOWORD(selection), "Dragging on paper failed to select text");
    mouse_event(MOUSEEVENTF_WHEEL, 0, 0, DWORD(-WHEEL_DELTA), 0);
    require(until([&] { return SendMessageW(field, EM_GETFIRSTVISIBLELINE, 0, 0) > 0; }),
            "Wheel over text did not scroll inside the field");
    const auto still = app.card(id);
    require(EqualRect(&still, &paper), "Text selection or scrolling moved the card");
    set(host, 1760, L"First card saved on switch");
    pencil(floating, {pin.right - px(45), pin.top + px(18)});
    require(GetDlgItem(host, 1760) == field && !SendMessageW(field, EM_CANUNDO, 0, 0),
            "Switching cards did not reuse a clean text field");
    clear_text();
    button(host, IDOK);
    require(until([&] { return !read(GetDlgItem(host, 1761)).empty() && IsWindowVisible(host); }),
            "Failed empty save did not restore the Floating editor");
    require_empty_caret(field);
    set(host, 1760, L"Canceled");
    button(host, IDCANCEL);
    require(until([&] { return !IsWindowVisible(host); }), "Cancel left the editor on the paper");
    app.stop();
    HistoryStore store(profile.path);
    require(store.begin_text_edit(id).text == L"First card saved on switch" &&
            store.begin_text_edit(copy).text == sample::text(false) &&
            sequence != GetClipboardSequenceNumber() && clipboard_text_is(L"First card saved on switch"),
            "Cancel of a Floating edit replaced the saved head");
}
void hanging_edit(const fs::path& exe) {
    history_fixture::Directory profile;
    domain::Id original, sibling;
    const std::wstring edited = L"\u7b2c\u4e00\u884c\r\n\u7b2c\u4e8c\u884c";
    {
        HistoryStore store(profile.path);
        persistence::save_shortcut(store.database(), application::workspace_toggle, {true, "Ctrl+Alt+F11"});
        domain::Format text, html;
        const auto formats = sample::formats(false);
        text.standard_id = CF_UNICODETEXT;
        text.bytes = formats.front().bytes;
        html.name = "HTML Format";
        html.bytes = formats[1].bytes;
        for (int i = 0; i < 20; ++i) store.admit("main", {{text}});
        original = store.admit("main", {{text, html}});
        store.admit("main", {{text}});
        sibling = store.copy(original, store.create_rope("Second", 4));
        exec(store.database().handle(), ("CREATE TRIGGER fail_main_edit BEFORE UPDATE OF snapshot_id ON entries "
             "WHEN OLD.id='" + original + "' BEGIN SELECT RAISE(ABORT,'fixture failure'); END").c_str());
    }
    const auto before = order(profile.path);
    Desktop app(exe, profile.path, true);
    const auto sequence = GetClipboardSequenceNumber();
    auto open = [&](const domain::Id& id) {
        const auto rect = app.card(id);
        const auto editor = select_command(app.workspace, center(rect), Command::edit);
        require(until([&] {
            const auto actual = bounds(editor);
            return GetWindow(editor, GW_OWNER) == app.workspace && actual.left > rect.left && actual.right < rect.right &&
                   actual.top > rect.top && actual.bottom < rect.bottom;
        }), "Editor was outside the paper inset");
        require(until([&] {
            GUITHREADINFO state{sizeof state};
            return GetGUIThreadInfo(GetWindowThreadProcessId(editor, nullptr), &state) &&
                   state.hwndFocus == GetDlgItem(editor, 1760);
        }), "Card text did not receive keyboard focus");
        return editor;
    };
    auto command = [&](win::WorkspaceCommand action) {
        SendMessageW(app.workspace, WM_COMMAND, static_cast<WPARAM>(action), 0);
    };
    auto toggle = [] {
        key(VK_CONTROL, true); key(VK_MENU, true); key(VK_F11, true);
        key(VK_F11, false); key(VK_MENU, false); key(VK_CONTROL, false);
    };
    auto editor = open(original);
    require(read(GetDlgItem(editor, 1760)) == sample::text(false), "Editor loaded the wrong entry");
    set(editor, 1760, edited.c_str());
    SendDlgItemMessageW(editor, 1760, EM_SETSEL, 1, 3);
    const auto line = single_rope_point(app);
    SendMessageW(app.workspace, WM_MOUSEWHEEL, MAKEWPARAM(0, -20 * WHEEL_DELTA), MAKELPARAM(line.x, line.y));
    auto rejected = [&] { return IsWindowVisible(editor) && read(GetDlgItem(editor, 1761)).find(L"failed") != std::wstring::npos; };
    require(until(rejected) && read(GetDlgItem(editor, 1760)) == edited &&
            SendDlgItemMessageW(editor, 1760, EM_GETSEL, 0, 0) == MAKELONG(1, 3),
            "Rejected offscreen save moved the paper or lost its draft");
    const auto after = app.card(original);
    const auto inset = bounds(editor);
    require(inset.left > after.left && inset.right < after.right, "Rejected navigation detached the editor");
    command(win::WorkspaceCommand::next);
    require(until(rejected) && order(profile.path) == before && app.card(original).left == after.left,
            "Rejected selection hid the draft");
    toggle();
    brief(100ms);
    require(IsWindowVisible(app.workspace) && IsWindowVisible(editor), "Failed hide discarded the draft");
    button(editor, IDOK);
    require(until(rejected) &&
            read(GetDlgItem(editor, 1760)) == edited, "Failed write lost its draft");
    button(editor, 1760);
    key(VK_ESCAPE, true); key(VK_ESCAPE, false);
    require(until([&] { return !IsWindowVisible(editor); }), "Esc did not cancel the Hanging edit");
    editor = open(original);
    require(read(GetDlgItem(editor, 1761)) == L"Saves as plain text", "Rich-text notice missing");
    saved(editor); // Unchanged text retains its rich companion, even with the write fault.
    editor = open(before.front());
    set(editor, 1760, L"Saved on scroll");
    SendMessageW(app.workspace, WM_MOUSEWHEEL, MAKEWPARAM(0, -20 * WHEEL_DELTA), MAKELPARAM(line.x, line.y));
    require(until([&] { return !IsWindowVisible(editor) && !GetWindow(editor, GW_OWNER); }), "Offscreen navigation did not finish editing");
    command(win::WorkspaceCommand::latest);
    editor = open(before.front());
    require(read(GetDlgItem(editor, 1760)) == L"Saved on scroll", "Offscreen save lost the text");
    set(editor, 1760, L"Saved on collapse");
    toggle();
    require(until([&] { return !IsWindowVisible(app.workspace) && !IsWindowVisible(editor); }), "Workspace did not collapse");
    toggle();
    require(until([&] { return IsWindowVisible(app.workspace); }) && !IsWindowVisible(editor), "Hidden edit reopened");
    command(win::WorkspaceCommand::overview);
    editor = open(sibling);
    set(editor, 1760, edited.c_str());
    button(editor, 1760);
    key(VK_CONTROL, true); key('S', true); key('S', false); key(VK_CONTROL, false);
    require(until([&] { return !IsWindowVisible(editor); }), "Ctrl+S did not finish the overview edit");
    require(sequence != GetClipboardSequenceNumber() && clipboard_text_is(L"Saved on collapse") &&
            before == order(profile.path) && IsWindowVisible(app.workspace), "Inactive edit displaced the saved active head");
    app.stop();
    {
        HistoryStore reopened(profile.path);
        require(reopened.begin_text_edit(before.front()).text == L"Saved on collapse" &&
                reopened.begin_text_edit(original).rich && reopened.begin_text_edit(original).text == sample::text(false) &&
                !reopened.begin_text_edit(sibling).rich && reopened.begin_text_edit(sibling).text == edited,
                "Saved edit affected the shared original or failed to survive restart");
        reopened.set_capacity("main", 1);
    }
    Desktop full(exe, profile.path, true);
    editor = select_command(full.workspace, center(full.card(before.front())), Command::edit);
    set(editor, 1760, L"unsaved evicted draft");
    ClipboardPublisher producer;
    const auto replacement = external_copy(producer, profile.path, L'D');
    require(until([&] { return !IsWindowVisible(editor); }) && order(profile.path) == Order{replacement},
            "Eviction left an edit surface on the replacement entry");
    full.stop();
    require(full.clean_exit, "Host failed after editor eviction");
}
void return_edit(const fs::path& exe, bool reject_save) {
    history_fixture::Directory profile;
    domain::Id id, tail;
    {
        HistoryStore store(profile.path);
        store.set_capacity("main", 1);
        domain::Format text;
        text.standard_id = CF_UNICODETEXT;
        text.bytes = sample::formats(false).front().bytes;
        domain::Format html;
        html.name = "HTML Format";
        html.bytes = sample::formats(false)[1].bytes;
        id = store.admit("main", {{text, html}});
        store.detach(id, {"", 640, 330, 240, 160});
        tail = store.admit("main", {{text}});
        if (reject_save)
            exec(store.database().handle(), "CREATE TRIGGER reject_text BEFORE UPDATE OF snapshot_id ON entries "
                 "BEGIN SELECT RAISE(ABORT,'fixture failure'); END");
    }
    Desktop app(exe, profile.path, true, reject_save ? nullptr : L"return");
    const auto card = app.floating_window();
    const auto rect = bounds(card);
    const int dpi = GetDpiForWindow(card);
    auto editor = select_command(card, center(rect), Command::edit);
    set(editor, 1760, L"Saved before return");
    const auto sequence = GetClipboardSequenceNumber();
    auto back = [&] { press(card, {rect.left + MulDiv(20, dpi, 96), rect.top + MulDiv(18, dpi, 96)}); };
    back();
    require(until([&] { return reject_save ? read(GetDlgItem(editor, 1761)).find(L"failed") != std::wstring::npos :
                                            app.contains("return " + id + " failed"); }), "Expected failure was not observed");
    require(IsWindowVisible(card) && !!IsWindowVisible(editor) == reject_save && order(profile.path) == Order{tail},
            "Failed return changed Floating membership or target capacity");
    if (!reject_save) {
        editor = select_command(card, center(rect), Command::edit);
        require(read(GetDlgItem(editor, 1760)) == L"Saved before return", "Rejected rehang rolled back the saved text");
        back();
        require(until([&] { return !IsWindow(card); }), "Return retry did not finish");
    } else require(read(GetDlgItem(editor, 1760)) == L"Saved before return", "Rejected autosave lost the draft");
    require(reject_save ? sequence == GetClipboardSequenceNumber() : clipboard_text_is(L"Saved before return"),
            "Return publication did not match the committed result");
    if (!reject_save) {
        const auto at = center(app.card(id));
        SetForegroundWindow(app.workspace);
        require(until([&] { return GetForegroundWindow()==app.workspace && WindowFromPoint(at)==app.workspace; }),
                "Returned card did not receive foreground input");
        press(app.workspace, at);
        brief(std::chrono::milliseconds(std::min<UINT>(60, GetDoubleClickTime()/4)));
        press(app.workspace, at);
        require(until([&] { return !IsWindowVisible(app.workspace) && GetClipboardSequenceNumber() != sequence; }),
                "Next Hanging double-click did not publish");
        const auto published = GetClipboardSequenceNumber();
        require(until([] { return OpenClipboard(nullptr) != FALSE; }), "Published clipboard unavailable");
        const bool same_offer = GetClipboardSequenceNumber() == published;
        const auto data = GetClipboardData(CF_UNICODETEXT);
        const auto* text = static_cast<const wchar_t*>(data ? GlobalLock(data) : nullptr);
        const bool correct = same_offer && text && std::wstring_view(text) == L"Saved before return" &&
                             !IsClipboardFormatAvailable(RegisterClipboardFormatW(L"HTML Format"));
        if (text) GlobalUnlock(data);
        CloseClipboard();
        require(correct, "Next recall delivered old text or rich formats");
    }
    app.stop();
    HistoryStore reopened(profile.path);
    require(reopened.begin_text_edit(id).text == (reject_save ? sample::text(false) : L"Saved before return") &&
            order(profile.path) == Order{reject_save ? tail : id}, "Reopened edited return has incorrect history");
}
void run(const fs::path &exe, bool styles_only = false) {
    if (!styles_only) {
        hanging_edit(exe);
        return_edit(exe, true);
        return_edit(exe, false);
    }
    history_fixture::Directory profile;
    domain::Id original, copy;
    const auto formats = sample::formats(false);
    const application::CardStyle chosen{96, true, 0x172d55, 0xecb36a, .80};
    {
        HistoryStore store(profile.path);
        persistence::save_shortcut(store.database(), application::workspace_toggle, {true, "Ctrl+Alt+F11"});
        domain::Format text;
        text.standard_id = CF_UNICODETEXT;
        text.bytes = formats[0].bytes;
        domain::Format html;
        html.name = "HTML Format";
        html.bytes = formats[1].bytes;
        original = store.admit("main", {{text, html}});
        copy = store.copy(original, "main");
        store.detach(copy, {"", 720, 270, 300, 180});
    }
    struct Cursor {
        POINT p{};
        bool ok = GetCursorPos(&p) != FALSE;
        ~Cursor() {
            if (ok)
                SetCursorPos(p.x, p.y);
        }
    } cursor;
    {
        Desktop app(exe, profile.path, true);
        const auto card = app.card(original);
        const auto select = [&](Command cmd) { return select_command(app.workspace, center(card), cmd); };
        auto dismiss=[](HWND popup) {
            key(VK_ESCAPE,true);key(VK_ESCAPE,false);
            require(until([&]{return !IsWindow(popup);}),"Inline menu did not close");
        };
        auto style=select(Command::font);
        set(style,10000+int(Command::font),L"42");
        dismiss(style);
        auto commit=[&](Command command,const wchar_t* value) {
            auto popup=select(command);
            if (command == Command::foreground || command == Command::background) {
                DWORD pid{};
                GetWindowThreadProcessId(popup, &pid);
                const auto picker = owned_window(pid, L"ClipRope.ColorPicker");
                HWND hex{};
                EnumChildWindows(
                    picker,
                    [](HWND h, LPARAM p) -> BOOL {
                        if (GetDlgCtrlID(h) == 4150)
                            *reinterpret_cast<HWND *>(p) = h;
                        return TRUE;
                    },
                    LPARAM(&hex));
                require(hex != nullptr, "Color input missing");
                SendMessageW(hex, WM_SETTEXT, 0, LPARAM(value));
                button(picker, IDOK);
                require(until([&] { return !IsWindow(picker); }) && IsWindowVisible(popup),
                        "Apply lost the parent menu");
                dismiss(popup);
                return;
            }
            int field=10000+int(command);
            if(command==Command::font) require(read(GetDlgItem(popup,field))==L"14","Canceled size was committed");
            set(popup,field,value);
            key(VK_RETURN,true);key(VK_RETURN,false);
            dismiss(popup);
        };
        commit(Command::font,L"96");
        select(Command::bold);
        commit(Command::foreground,L"#172D55");
        commit(Command::background,L"#ECB36A");
        commit(Command::opacity,L"80");
        const auto floating = app.floating_window();
        require(floating && IsWindowVisible(floating), "Floating copy unavailable");
        auto editor = select_command(floating, center(bounds(floating)), Command::edit);
        require(read(GetDlgItem(editor, 1760)) == sample::text(false) &&
                    read(GetDlgItem(editor, 1761)) == L"Saves as plain text",
                "No visible downgrade before save");
        set(editor, 1760, L"Changed \U0001f9f5");
        button(editor, 1760);
        require(GetForegroundWindow() == editor, "Editor did not receive focus");
        key(VK_TAB, true);
        key(VK_TAB, false);
        require(until([&] {
                    GUITHREADINFO state{sizeof(state)};
                    return GetGUIThreadInfo(0, &state) && state.hwndFocus == GetDlgItem(editor, IDOK);
                }),
                "Tab did not move from content to Save");
        saved(editor);
        app.stop();
        require(app.clean_exit, "Failed to close edited host");
    }
    {
        HistoryStore store(profile.path);
        rendering::CardBrowser browse{persistence::Browser(store)};
        require(browse.summary(original).style == chosen &&
                    browse.summary(copy).style == application::CardStyle{},
                "Reopened card style differs from selected fields");
        require(store.begin_text_edit(copy).text == L"Changed \U0001f9f5" &&
                    !store.begin_text_edit(copy).rich && store.begin_text_edit(original).rich &&
                    store.begin_text_edit(original).text == sample::text(false),
                "Reopened rich original or plain copy is incorrect");
    }
    {
        Desktop app(exe, profile.path, true);
        HWND floating = app.floating_window();
        require(floating != nullptr, "Floating copy not restored");
        auto editor = select_command(floating, center(bounds(floating)), Command::edit);
        set(editor, 1760, L"Saved on recycle");
        const auto r = bounds(floating);
        const auto dpi = GetDpiForWindow(floating);
        press(floating, {r.right - MulDiv(19, dpi, 96), r.top + MulDiv(19, dpi, 96)});
        require(until([&] { return !IsWindow(floating) && !IsWindowVisible(editor); }), "Edited Floating recycle failed");
        app.stop();
        require(app.clean_exit, "Failed to close stale-edit host");
    }
    HistoryStore reopened(profile.path);
    require(reopened.floating().empty() && reopened.hanging("main").size() == 1 &&
            reopened.entry(copy).rope_id == domain::rebin_id && reopened.begin_text_edit(copy).text == L"Saved on recycle",
            "Recycle lost the edited entry");
    std::cout << "context_routes=Hanging,Floating style_fields=5 downgrade_warning=1 unicode_save=1 "
                 "tab_navigated=1 edited_rebin=1\n";
}
#include "inline_review.inc"
#include "quick_note.inc"
#include "expand_desktop.inc"
#include "edit_transition.inc"
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc < 2 || argc > 4)
        return 2;
    if (FAILED(OleInitialize(nullptr))) return 2;
    struct Ole { ~Ole() { OleUninitialize(); } } ole;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try {
        if (argc == 4 && std::wstring_view(argv[2]) == L"--paper-gallery") paper_gallery(fs::absolute(argv[1]), argv[3]);
        else if (argc >= 3 && std::wstring_view(argv[2]) == L"--ime") ime_review(fs::absolute(argv[1]), argc == 4 ? argv[3] : L"");
        else if (argc == 3 && std::wstring_view(argv[2]) == L"--menus") matrix(fs::absolute(argv[1]));
        else if (argc == 3 && std::wstring_view(argv[2]) == L"--styles") run(fs::absolute(argv[1]), true);
        else if (argc >= 3 && std::wstring_view(argv[2]) == L"--expand")
            expand_desktop(fs::absolute(argv[1]), argc == 4 ? fs::absolute(argv[3]) : fs::path{});
        else if (argc == 3 && std::wstring_view(argv[2]) == L"--controls") pencil_controls(fs::absolute(argv[1]));
        else if (argc == 3 && (std::wstring_view(argv[2]) == L"--quick-note" || std::wstring_view(argv[2]) == L"--quick-note-ime"))
            quick_notes(fs::absolute(argv[1]), std::wstring_view(argv[2]) == L"--quick-note-ime");
        else if (argc == 3 && (std::wstring_view(argv[2]) == L"--transition" || std::wstring_view(argv[2]) == L"--transition-baseline"))
            edit_transition(fs::absolute(argv[1]), std::wstring_view(argv[2]) == L"--transition-baseline");
        else run(fs::absolute(argv[1]));
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
