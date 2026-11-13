#include "presentation/card_labels.hpp"
#include "../clipboard_sources/external_copy.hpp"
#include "../integration/history_fixture.hpp"
#include "persistence/history_store.hpp"
#include "rendering/card_browser.hpp"
#include "platform_win/file_group.hpp"
#include "platform_win/history_clipboard.hpp"

#include <shlobj.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>

namespace {
using namespace cliprope;
using namespace cliprope::persistence;
using history_fixture::require;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
DWORD last_verified_group{};

void check(HRESULT result, const char* action) {
    if (FAILED(result)) throw std::runtime_error(std::string(action) + " HRESULT=" + std::to_string(static_cast<unsigned>(result)));
}

// Explorer's own context menu handles cut/paste, including move completion.
// Every PIDL belongs to a new directory containing only this fixture.
void verb(IContextMenu* menu, HWND window, const char* narrow, const wchar_t* wide) {
    HMENU popup = CreatePopupMenu();
    require(popup != nullptr, "Menu unavailable");
    struct MenuGuard { HMENU value; ~MenuGuard() { DestroyMenu(value); } } guard{popup};
    check(menu->QueryContextMenu(popup, 0, 1, 0x7fff, CMF_NORMAL), "Shell verb enumeration");
    CMINVOKECOMMANDINFOEX action{};
    action.cbSize = sizeof action;
    action.fMask = CMIC_MASK_UNICODE | CMIC_MASK_FLAG_NO_UI | CMIC_MASK_NOASYNC;
    action.hwnd = window;
    action.lpVerb = narrow;
    action.lpVerbW = wide;
    action.nShow = SW_HIDE;
    check(menu->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&action)), narrow);
}

ComPtr<IShellItem> shell_item(const fs::path& path) {
    ComPtr<IShellItem> item;
    check(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)), "Shell fixture item");
    return item;
}

void cut(const std::array<fs::path,2>& paths, HWND owner) {
    ComPtr<IShellFolder> folder;
    check(shell_item(paths[0].parent_path())->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&folder)),
          "Source fixture folder");
    struct PidlFree {
        using pointer = PIDLIST_RELATIVE;
        void operator()(pointer value) const { ILFree(value); }
    };
    std::array<std::unique_ptr<ITEMIDLIST, PidlFree>, 2> items;
    std::array<PCUITEMID_CHILD, 2> children{};
    for (size_t i = 0; i < items.size(); ++i) {
        PIDLIST_RELATIVE child{};
        auto name = paths[i].filename().wstring();
        check(folder->ParseDisplayName(owner, nullptr, name.data(), nullptr, &child, nullptr), "Source fixture child");
        items[i].reset(child);
        children[i] = reinterpret_cast<PCUITEMID_CHILD>(child);
    }
    ComPtr<IContextMenu> context;
    check(folder->GetUIObjectOf(owner, static_cast<UINT>(children.size()), children.data(),
          IID_IContextMenu, nullptr, reinterpret_cast<void**>(context.GetAddressOf())), "Shell cut menu");
    verb(context.Get(), owner, "cut", L"cut");
}

void paste(const fs::path& destination, HWND owner) {
    ComPtr<IShellFolder> folder;
    check(shell_item(destination)->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&folder)), "Destination folder");
    ComPtr<IContextMenu> context;
    check(folder->CreateViewObject(owner, IID_PPV_ARGS(&context)), "Shell background menu");
    verb(context.Get(), owner, "paste", L"paste");
}

struct Offered {
    std::vector<std::wstring> paths;
    DWORD effect{};
};
Offered offered(UINT expected = 2) {
    // Called only after cutting test-owned files or restoring their known entry.
    require(OpenClipboard(nullptr) != FALSE, "Could not inspect synthetic file selection");
    struct Unlock { ~Unlock() { CloseClipboard(); } } unlock;
    Offered out;
    HDROP drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
    require(drop != nullptr, "Native shell did not offer a file group");
    const auto count = DragQueryFileW(drop, 0xffffffffu, nullptr, 0);
    require(count == expected, "Unexpected native file count");
    for (UINT i = 0; i < count; ++i) {
        const auto n = DragQueryFileW(drop,i,nullptr,0);
        require(n > 0 && n <= 32767, "Invalid path length");
        std::wstring name(n+1,0);
        require(DragQueryFileW(drop,i,name.data(),n+1) == n, "Unreadable path");
        name.resize(n);
        out.paths.push_back(std::move(name));
    }
    auto effect = GetClipboardData(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
    require(effect && GlobalSize(effect) >= sizeof(DWORD), "Shell did not offer a drop effect");
    auto* bytes = GlobalLock(effect);
    require(bytes != nullptr, "Could not read drop effect");
    std::memcpy(&out.effect,bytes,sizeof(DWORD));
    GlobalUnlock(effect);
    return out;
}

int host(const fs::path& exe, const fs::path& profile, const wchar_t* command) {
    HANDLE child = probe::launch({exe.wstring(), profile.wstring(), command}, probe::desktop_path());
    require(child != nullptr, "Could not launch the headless host");
    probe::pump_until([&] { return WaitForSingleObject(child,0) == WAIT_OBJECT_0; }, "Headless host timed out");
    DWORD code{};
    require(GetExitCodeProcess(child,&code) != FALSE, "Missing host result");
    CloseHandle(child);
    return static_cast<int>(code);
}

std::array<fs::path,2> files(const fs::path& root) {
    fs::create_directories(root / "source" / "A-folder");
    fs::create_directories(root / "destination");
    std::array<fs::path,2> paths{root / "source" / L"Z-\u03a9.png", root / "source" / "A-folder"};
    // A valid one-pixel PNG, selected as a filesystem item.
    constexpr unsigned char png[]{
        0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0,0,0,0x0d,0x49,0x48,0x44,0x52,
        0,0,0,1,0,0,0,1,8,4,0,0,0,0xb5,0x1c,0x0c,2,0,0,0,0x0b,0x49,0x44,0x41,
        0x54,0x78,0xda,0x63,0x64,0xf8,0x0f,0,1,5,1,1,0x27,0x18,0xe3,0x66,0,
        0,0,0,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82};
    std::ofstream image(paths[0], std::ios::binary);
    image.write(reinterpret_cast<const char*>(png), sizeof png);
    std::ofstream(paths[1] / "child.txt") << "synthetic";
    return paths;
}
void check_paste(const fs::path& root, const std::array<fs::path,2>& paths, bool moved) {
    probe::pump_until([&] {return fs::exists(root/"destination"/paths[0].filename()) &&
        fs::exists(root/"destination"/paths[1].filename()/"child.txt");}, "Shell paste did not complete");
    for (const auto& item : paths) require(fs::exists(item) != moved, "Source ownership after paste is incorrect");
}

void validate_bounds() {
    constexpr char names[] = "C:\\cliprope-test\\one\0C:\\cliprope-test\\two\0";
    DROPFILES header{};
    header.pFiles = sizeof header;
    Representation value{CF_HDROP, std::vector<unsigned char>(sizeof header + sizeof names)};
    std::memcpy(value.bytes.data(), &header, sizeof header);
    std::memcpy(value.bytes.data() + sizeof header, names, sizeof names);
    const auto original = value;
    require(validate_file_group(value) && file_group_paths(value) ==
            std::vector<std::wstring>{L"C:\\cliprope-test\\one", L"C:\\cliprope-test\\two"},
            "ANSI file group lost path order during canonicalization");
    value = original;
    value.bytes.pop_back();
    require(!validate_file_group(value), "Missing final group terminator was accepted");
    value = original;
    header.pFiles = 0xfffffff0u;
    std::memcpy(value.bytes.data(), &header, sizeof header);
    require(!validate_file_group(value), "Out-of-bounds group offset was accepted");
    std::vector<Representation> orphan{copy_drop_effect()};
    require(!prepare_file_group(orphan) && orphan.empty(), "Orphan drop effect was restorable");
}

void run(const fs::path& exe, const fs::path& root, HWND window) {
    const auto direct = root / "direct";
    auto native = files(direct);
    {
        HistoryStore store(direct / "profile");
        win::HistoryClipboard clipboard;
        win::HistoryCapture listener(store, clipboard);
        cut(native,window);
        const auto source = offered();
        require((source.effect & DROPEFFECT_MOVE) != 0, "Native cut did not offer move");
        require(std::find(source.paths.begin(),source.paths.end(),native[0].wstring()) != source.paths.end() &&
                std::find(source.paths.begin(),source.paths.end(),native[1].wstring()) != source.paths.end(),
                "First native selection did not match the disposable fixture");
        last_verified_group = GetClipboardSequenceNumber();
        listener.observe();
        paste(direct / "destination",window);
        check_paste(direct,native,true);
        probe::pump_until([&] { listener.poll(); return !listener.busy(); }, "Background observer did not settle");
    }
    const auto restore = root / "restore";
    auto selected = files(restore);
    const auto profile = restore / "profile";
    cut(selected,window);
    const auto source = offered();
    require(std::find(source.paths.begin(),source.paths.end(),selected[0].wstring()) != source.paths.end() &&
            std::find(source.paths.begin(),source.paths.end(),selected[1].wstring()) != source.paths.end() &&
            (source.effect & DROPEFFECT_MOVE), "Cut fixture did not offer the two selected items");
    last_verified_group = GetClipboardSequenceNumber();
    require(host(exe,profile,L"capture") == 0, "File history capture failed");
    std::string id;
    {
        HistoryStore store(profile);
        auto entries = store.hanging("main");
        require(entries.size() == 1, "Grouped cut did not occupy exactly one entry");
        id = entries.front().id;
        auto saved = store.load(id);
        bool group = false;
        for (const auto& f : saved.snapshot.formats) {
            if (f.outcome != "saved") continue;
            if (f.standard_id == CF_HDROP) {
                group = file_group_paths({CF_HDROP,f.bytes}) == source.paths;
            }
            require(f.name != "PNG" && f.standard_id != CF_DIB && f.standard_id != CF_DIBV5,
                    "A selected image file was stored as pixels");
            require(f.name != "FileContents", "A filesystem reference was materialized as a virtual file");
        }
        require(group, "Saved group lost native path order");
        rendering::CardBrowser browser{persistence::Browser(store)};
        const auto label = cliprope::presentation::english_card(browser.summary(id)).capabilities;
        require(label.find(L"File references") != label.npos && label.find(L"Image") == label.npos,
                "File selection received an incorrect restore-capability label");
    }
    ClipboardPublisher sentinel;
    require(SUCCEEDED(sentinel.publish({{CF_UNICODETEXT,{'X',0,0,0}}})), "Failed to overwrite the native cut");
    require(host(exe,profile,L"head") == 0, "Restarted host could not publish the group");
    const auto published = offered();
    require(published.paths == source.paths && published.effect == DROPEFFECT_COPY, "Published group/effect mismatch");
    last_verified_group = GetClipboardSequenceNumber();
    paste(restore / "destination",window);
    check_paste(restore,selected,false);
    require(fs::remove(selected[0]), "Could not prepare missing-reference fixture");
    require(SUCCEEDED(sentinel.publish({{CF_UNICODETEXT,{'Y',0,0,0}}})), "Could not establish new owner");
    require(host(exe,profile,L"head") != 0 && sentinel.is_current(), "Missing path displaced the current clipboard");
    require(fs::copy_file(restore / "destination" / selected[0].filename(), selected[0]),
            "Could not restore the disposable missing member");
    require(host(exe,profile,L"head") == 0 && offered().paths == source.paths,
            "Returning the missing member did not restore file-group availability");
    last_verified_group = GetClipboardSequenceNumber();
    {
        HistoryStore store(profile);
        const auto group = inspect_expansion(store.prepare_expansion(store.entry(id)));
        const auto sequence = GetClipboardSequenceNumber();
        require(store.expand_file_group(group, {}) && GetClipboardSequenceNumber() == sequence,
                "Expansion changed the native clipboard");
        win::HistoryClipboard clipboard;
        for (const auto& member : group.members) {
            clipboard.publish(store.load(member.id));
            const auto single = offered(1);
            require(single.paths == std::vector<std::wstring>{member.path} && single.effect == DROPEFFECT_COPY,
                    "Expanded card did not publish a single copy reference");
        }
        clipboard.publish(store.load(id));
        require(offered().paths == source.paths, "Expanded original no longer publishes the entire selection");
        last_verified_group = GetClipboardSequenceNumber();
        store.erase(id);
        store.gc();
        require(fs::is_regular_file(selected[0]) && fs::is_regular_file(selected[1]/"child.txt"),
                "Deleting a card removed a referenced file");
    }
    if (sentinel.is_current()) OleSetClipboard(nullptr);
    std::cout << "Native cut moved two items; persisted group copied in order after restart; missing path and delete passed\n";
}
}

int wmain(int argc, wchar_t** argv) {
    const bool interactive = argc == 3 && std::wstring_view(argv[2]) == L"--replace-desktop-clipboard" &&
                             _wcsicmp(probe::object_name(GetProcessWindowStation()).c_str(), L"WinSta0") == 0;
    if (!interactive && (argc != 2 || !probe::private_station())) return 2;
    if (FAILED(OleInitialize(nullptr))) return 2;
    HWND window = CreateWindowW(L"STATIC",L"ClipRope file test",WS_POPUP,0,0,1,1,nullptr,nullptr,nullptr,nullptr);
    int status = 0;
    try {
        history_fixture::Directory directory;
        validate_bounds();
        run(argv[1],directory.path,window);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        status = 1;
    }
    if (last_verified_group && OpenClipboard(nullptr)) {
        if (GetClipboardSequenceNumber() == last_verified_group) EmptyClipboard();
        CloseClipboard();
    }
    if (window) DestroyWindow(window);
    OleUninitialize();
    return status;
}
