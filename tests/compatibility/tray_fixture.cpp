#include "../integration/history_fixture.hpp"
#include "../clipboard_sources/sample.hpp"
#include "../integration/clipboard_process.hpp"
#include "persistence/hotkey_settings.hpp"
#include "persistence/history_store.hpp"
#include "platform_win/tray.hpp"
#include "platform_win/tray_popup.hpp"
#include "workspace_fixture_frame.hpp"
#include <iostream>

namespace {
using namespace cliprope;
using history_fixture::require;
void seed(const std::filesystem::path& profile, std::wstring_view mode) {
    require(!std::filesystem::exists(profile),"Fixture needs a new profile");
    persistence::HistoryStore store(profile);
    persistence::save_shortcut(store.database(), application::workspace_toggle, {true, "Ctrl+Alt+F11"});
    if (mode == L"--empty") return;
    const bool visual = mode == L"--seed-visual", tray = mode == L"--seed-tray-visual";
    const auto rope = tray ? store.create_rope("研究绳", 100) : visual ? store.create_rope("工作绳", 100) : "main";
    if (visual || tray) {
        persistence::save_appearance(store.database(), {application::Theme::system});
        store.set_active_rope(rope);
    }
    if (mode == L"--seed") store.set_capacity(rope, 6);
    const wchar_t* research[] = {L"草稿：UI 菜单优化", L"截图：SEM 图片", L"链接：QualPF 仓库", L"待办：整理 XRD 数据",
        L"实验记录：3244 样品 · XRD & SEM results for the diffraction series and instrument calibration"};
    for (int i = 0; i < (tray ? 5 : 6); ++i) {
        const std::wstring text = tray ? research[i] : L"Tray sample " + std::to_wstring(i + 1);
        const auto* bytes=reinterpret_cast<const unsigned char*>(text.c_str());
        domain::Format format; format.standard_id=CF_UNICODETEXT;
        format.bytes.assign(bytes,bytes+(text.size()+1)*sizeof(wchar_t));
        domain::Snapshot snap; snap.formats.push_back(std::move(format));
        if (visual) {
            snap.source_hint = "FixtureEditor.exe";
            domain::Format html;
            html.name = "HTML Format";
            html.bytes = sample::formats(false).back().bytes;
            snap.formats.push_back(std::move(html));
        }
        store.admit(rope,snap);
    }
}
void parser_registration() {
    require(probe::private_station(),"Use the isolated test station");
    for (auto value : {"Ctrl+Shift+V","Ctrl+Alt+F11","Shift+0"}) {
        auto key=win::parse_hotkey(value);
        require(key && win::hotkey_name(*key)==value,"Shortcut round trip failed");
    }
    for (auto value : {"Ctrl","Alt+F25","Ctrl+Ctrl+B","Ctrl+Unknown","Ctrl+p","Ctrl+"})
        require(!win::parse_hotkey(value),"Invalid shortcut accepted");
    for (auto value : {"Tab", "F8", "Ctrl+Shift+Space", "Shift+Tab"}) {
        auto key = win::parse_hotkey(value);
        require(key && win::hotkey_name(*key) == value && win::valid_hotkey(*key, false), "Scoped chord failed");
    }
    require(!win::valid_hotkey({0, 'A'}, false) && !win::valid_hotkey({0, VK_TAB}, true) &&
            !win::valid_hotkey({MOD_CONTROL, VK_F12}, true), "Reserved or unmodified chord accepted");
    HWND listener=CreateWindowW(L"STATIC",L"ClipRope private shortcut",WS_POPUP,0,0,0,0,nullptr,nullptr,nullptr,nullptr);
    require(listener!=nullptr,"Could not create shortcut listener");
    constexpr UINT modifiers=MOD_CONTROL|MOD_ALT|MOD_SHIFT;
    if (!RegisterHotKey(listener,7,modifiers,VK_F17)) {
        const auto error=GetLastError();
        win::TrayActions actions;
        actions.read_shortcuts=[] { auto keys = application::default_shortcuts(); keys[0].chord = "Ctrl+Alt+Shift+F17"; return keys; };
        win::Tray tray(std::move(actions));
        require(tray.start(listener) && !tray.hotkey_registered(),"Shortcut availability changed unexpectedly");
        tray.message(WM_DESTROY,0,0);
        DestroyWindow(listener);
        std::cout << "Noninteractive station declined registration, error=" << error
                  << "; listener stayed available; shortcut syntax passed\n";
        return;
    }
    {
        win::TrayActions actions;
        actions.read_shortcuts=[] { auto keys = application::default_shortcuts(); keys[0].chord = "Ctrl+Alt+Shift+F17"; return keys; };
        win::Tray tray(std::move(actions));
        require(tray.start(listener) && !tray.hotkey_registered(),"Expected detected shortcut conflict");
    }
    UnregisterHotKey(listener,7);
    {
        win::TrayActions actions;
        actions.read_shortcuts=[] { auto keys = application::default_shortcuts(); keys[0].chord = "Ctrl+Alt+Shift+F17"; return keys; };
        win::Tray tray(std::move(actions));
        require(tray.start(listener) && tray.hotkey_registered(),"Released shortcut unavailable");
    }
    require(RegisterHotKey(listener,7,modifiers,VK_F17)!=FALSE,"Tray teardown retained its shortcut");
    UnregisterHotKey(listener,7);
    DestroyWindow(listener);
    std::cout << "Default shortcut conflict, recovery, release and settings syntax passed\n";
}

void hidden_frames(const std::filesystem::path& output) {
    require(probe::private_station(),"Use an isolated station for the native tray controls");
    std::filesystem::create_directories(output);
    localization::LocalizationManager language;
    win::UiLocalization binding(language);
    auto listener=CreateWindowW(L"STATIC",L"",WS_POPUP,0,0,1,1,nullptr,nullptr,nullptr,nullptr);
    require(listener!=nullptr,"Missing tray fixture owner");
    bool hidden{},selected{};
    win::TrayActions actions;
    actions.read_shortcuts=[] { auto keys=application::default_shortcuts(); for(auto& key:keys) key.enabled=false; return keys; };
    actions.menu=[&] { win::TrayModel model{"main",L"main",{{"synthetic",L"Saved text"}}};
                       if(hidden) model.hidden_card="hidden"; return model; };
    actions.reveal_hidden=[&](const auto& id){selected=id=="hidden";};
    win::Tray tray(std::move(actions), [](DWORD,PNOTIFYICONDATAW)->BOOL {return TRUE;}, false);
    require(tray.start(listener),"Tray fixture could not start");
    for(bool chinese:{false,true}) for(bool has_hidden:{false,true}) {
        language.set_language(chinese ? localization::Language::simplified_chinese : localization::Language::english);
        hidden=has_hidden;
        selected=false;
        struct Frame { std::filesystem::path path; bool hidden; std::exception_ptr error; } frame{
            output/(std::string(hidden ? "hidden-" : "no-hidden-")+(chinese ? "zh.bmp":"en.bmp")),hidden};
        SetWindowLongPtrW(listener,GWLP_USERDATA,LONG_PTR(&frame));
        require(SetTimer(listener,1,20,[](HWND h,UINT,UINT_PTR,DWORD){
            auto menu=FindWindowW(L"ClipRope.TrayMenu",nullptr);
            if(!menu) return;
            auto& f=*reinterpret_cast<Frame*>(GetWindowLongPtrW(h,GWLP_USERDATA));
            KillTimer(h,1);
            try {
                auto command=GetDlgItem(menu,win::tray_command::reveal_hidden);
                require(bool(command)==f.hidden,"Incorrect conditional tray row");
                workspace_fixture::frame(menu,f.path,false,true);
                if(command) {PostMessageW(command,BM_CLICK,0,0); return;}
            } catch(...) { f.error=std::current_exception(); }
            PostMessageW(menu,WM_KEYDOWN,VK_ESCAPE,0);
        }),"Cannot capture tray menu");
        tray.message(win::Tray::callback,MAKELPARAM(64,64),MAKELPARAM(WM_CONTEXTMENU,win::Tray::icon_id));
        KillTimer(listener,1);
        if(frame.error) std::rethrow_exception(frame.error);
        require(selected==hidden && std::filesystem::is_regular_file(frame.path),"Tray capture or selection failed");
    }
    tray.message(WM_DESTROY,0,0);
    DestroyWindow(listener);
}
}
int wmain(int argc,wchar_t** argv) {
    try {
        if (argc==2 && std::wstring_view(argv[1])==L"--private") parser_registration();
        else if (argc==3 && std::wstring_view(argv[1])==L"--hidden-frames") hidden_frames(argv[2]);
        else if (argc==3 && (std::wstring_view(argv[1])==L"--seed" || std::wstring_view(argv[1])==L"--empty" ||
                std::wstring_view(argv[1])==L"--seed-visual" || std::wstring_view(argv[1])==L"--seed-tray-visual"))
            seed(argv[2],argv[1]);
        else if (argc==3 && std::wstring_view(argv[1])==L"--verify") {
            persistence::HistoryStore store(argv[2]);
            require(persistence::read_shortcuts(store.database())[application::workspace_toggle].chord=="Ctrl+Alt+F11","Rebound shortcut was not persisted");
            auto rows=store.hanging("main");
            require(rows.size()==6,"Unexpected tray capture or deletion");
            const auto text=std::wstring(L"Tray sample 5");
            const auto* bytes=reinterpret_cast<const unsigned char*>(text.c_str());
            require(store.load(rows[2].id).snapshot.formats.front().bytes==
                    std::vector<unsigned char>(bytes,bytes+(text.size()+1)*sizeof(wchar_t)),"Promotion did not survive the two later copies");
        } else return 2;
        return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
