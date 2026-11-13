#include "desktop_app.hpp"
#include "../clipboard_sources/media_sample.hpp"
#include "platform_win/file_group.hpp"
#include "platform_win/virtual_files.hpp"
#include "platform_win/workspace_commands.hpp"
#include <wrl.h>

namespace {
using namespace workspace_fixture;

// This STA owns the drop target; the real desktop executable owns the drag source.
class Receiver final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDropTarget> {
public:
    std::vector<Representation> expected;
    DWORD accept = DROPEFFECT_COPY, offered{};
    int enters{}, drops{};
    bool matched{};
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject*, DWORD, POINTL, DWORD* effect) override {
        ++enters;
        offered = *effect;
        *effect &= accept;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        *effect &= accept;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        *effect &= accept;
        if (!*effect) return S_OK;
        ++drops;
        matched = std::all_of(expected.begin(), expected.end(), [data](const auto& item) {
            FORMATETC format{static_cast<CLIPFORMAT>(item.format), nullptr, DVASPECT_CONTENT, item.index, item.tymed};
            STGMEDIUM medium{};
            if (data->QueryGetData(&format) != S_OK || FAILED(data->GetData(&format, &medium))) return false;
            bool same = false;
            if (medium.tymed == TYMED_ISTREAM) {
                std::vector<unsigned char> bytes(item.bytes.size() + 1);
                ULONG count{};
                same = SUCCEEDED(medium.pstm->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &count)) &&
                       count == item.bytes.size() && std::equal(item.bytes.begin(), item.bytes.end(), bytes.begin());
            } else if (medium.tymed == TYMED_HGLOBAL) {
                const void* bytes = GlobalLock(medium.hGlobal);
                same = bytes && GlobalSize(medium.hGlobal) >= item.bytes.size() &&
                       std::memcmp(bytes, item.bytes.data(), item.bytes.size()) == 0;
                if (bytes) GlobalUnlock(medium.hGlobal);
            }
            ReleaseStgMedium(&medium);
            return same;
        });
        return S_OK;
    }
};

domain::Snapshot snapshot(const std::vector<Representation>& formats) {
    domain::Snapshot result;
    for (const auto& f : formats) {
        domain::Format item;
        if (f.format < 0xc000) item.standard_id = f.format;
        else {
            for (wchar_t ch : format_name(f.format)) {
                require(ch < 128, "Expected an ASCII fixture format name");
                item.name.push_back(static_cast<char>(ch));
            }
        }
        item.bytes = f.bytes;
        item.tymed = f.tymed;
        result.formats.push_back(std::move(item));
    }
    return result;
}

void drag(HWND source, POINT start, POINT end, bool escape = false, bool hold_ctrl = false, Receiver* receiver = nullptr) {
    const auto entered = receiver ? receiver->enters : 0;
    aim(source, start);
    key(VK_CONTROL, true);
    mouse(MOUSEEVENTF_LEFTDOWN);
    brief(100ms);
    SetCursorPos(start.x + 18, start.y);
    brief(220ms);
    SetCursorPos(end.x, end.y);
    brief(230ms);
    // Release Ctrl only after OLE actually entered the test target. A fixed
    // delay can expire while the source is still processing the lift.
    const bool reached_target = !receiver || until([&] { return receiver->enters > entered; });
    if (!hold_ctrl) key(VK_CONTROL, false);
    if (escape) { key(VK_ESCAPE, true); brief(45ms); key(VK_ESCAPE, false); }
    mouse(MOUSEEVENTF_LEFTUP);
    if (hold_ctrl) key(VK_CONTROL, false);
    brief(140ms);
    require(reached_target, "OLE never entered the synthetic drop target");
}

void run(const fs::path& exe, const fs::path& folder, HWND window, Receiver& receiver, bool floating) {
    const auto png = media_sample::png();
    const auto original = folder / "original.png";
    fs::create_directories(folder);
    std::ofstream(original, std::ios::binary).write(reinterpret_cast<const char*>(png.bytes.data()), png.bytes.size());
    const auto file = folder / "other.txt";
    std::ofstream(file) << "second";
    const Representation paths{CF_HDROP, encode_file_group({file.wstring(), original.wstring()})};
    const Representation native_path{CF_HDROP, encode_file_group({original.wstring()})};
    const Representation stream{virtual_files::contents_format(), png.bytes, 0, TYMED_ISTREAM};
    for (std::string kind : {"text", "files", "pixels", "original", "replaced"}) {
        const auto profile = folder / kind;
        auto saved = kind == "text" ? sample::formats(false) : std::vector<Representation>{kind == "files" ? paths : png};
        receiver.expected = saved;
        if (saved.front().format == png.format)
            receiver.expected.push_back({png.format, png.bytes, -1, TYMED_ISTREAM});
        auto content = snapshot(saved);
        if (kind == "original" || kind == "replaced") {
            const auto path = original.u8string();
            content.screenshot_path.assign(path.begin(), path.end());
            if (kind == "replaced") std::ofstream(original, std::ios::binary) << "changed";
        }
        if (kind == "original") receiver.expected.push_back(native_path);
        if (kind == "pixels" || kind == "replaced") receiver.expected.push_back(stream);
        std::string id;
        {
            HistoryStore store(profile);
            id = store.admit("main", content);
            if (floating) store.detach(id, {"", 24, 260, 220, 150, true});
        }
        Desktop host(exe, profile, !floating);
        const auto source = floating ? host.floating_window() : host.workspace;
        RECT before{}, target{};
        if (floating) GetWindowRect(source, &before);
        else before = host.card(id);
        GetWindowRect(window, &target);
        const auto sequence = GetClipboardSequenceNumber();
        const auto order_before = order(profile);
        int previous = receiver.drops;
        if (kind == "text") {
            for (bool cancel : {false, true}) {
                const int enters = receiver.enters;
                receiver.accept = cancel ? DROPEFFECT_COPY : DROPEFFECT_MOVE;
                drag(source, center(before), center(target), cancel, false, &receiver);
                require(receiver.enters > enters && receiver.drops == previous, "Rejection or Escape failed");
            }
            const int enters = receiver.enters;
            drag(source, center(before), center(target), true, true, &receiver);
            require(receiver.enters > enters && receiver.drops == previous, "Escape failed while holding Ctrl");
            // Windows can deliver the next Ctrl press as WM_LBUTTONDBLCLK.
            aim(source, center(before));
            mouse(MOUSEEVENTF_LEFTDOWN);
            mouse(MOUSEEVENTF_LEFTUP);
            brief(60ms);
        }
        receiver.accept = DROPEFFECT_COPY;
        drag(source, center(before), center(target), false, false, &receiver);
        require(receiver.drops == previous + 1 && receiver.offered == DROPEFFECT_COPY && receiver.matched,
                "Target did not receive exactly one copy of the saved bytes");
        require(GetClipboardSequenceNumber() == sequence && order(profile) == order_before,
                "External drag changed clipboard or rope order");
        RECT after{};
        if (floating) GetWindowRect(source, &after);
        else after = host.card(id);
        require(EqualRect(&before, &after), "External drag moved the card");
        host.stop();
        HistoryStore reopened(profile);
        require(host.clean_exit && reopened.floating().size() == (floating ? 1u : 0u), "Unexpected Floating lifetime");
        std::cout << (floating ? "Floating " : "Hanging ") << kind << " passed\n";
    }
}

void overview(const fs::path& exe, const fs::path& profile, bool fail) {
    std::string a, b, c, f;
    {
        HistoryStore store(profile);
        store.create_rope("Other", 3);
        store.set_capacity("main", 3);
        const auto content = snapshot(sample::formats(false));
        f = store.admit("main", content);
        store.detach(f, {"", 26, 455, 210, 144, true});
        c = store.admit("main", content);
        b = store.admit("main", content);
        a = store.admit("main", content);
    }
    Desktop host(exe, profile, true, fail ? L"transfer" : nullptr);
    SendMessageW(host.workspace, WM_COMMAND, static_cast<WPARAM>(win::WorkspaceCommand::overview), 0);
    require(until([&] { return host.frame().rows.size() == 2; }), "Overview unavailable");
    const auto sequence = GetClipboardSequenceNumber();
    const auto first = host.card(a);
    const POINT head{first.left + 8, center(first).y};
    const POINT outside{first.right + 100, GetSystemMetrics(SM_CYSCREEN) - 60};
    drag(host.workspace, center(host.card(c)), outside);
    require(order(profile) == Order{a,b,c}, "Overview drop outside changed the order");
    drag(host.workspace, center(host.card(c)), head);
    const auto next = order(profile);
    if (fail) require(next == Order{a,b,c} && host.contains("transfer copy " + c + " main failed"), "Copy did not roll back");
    else {
        require(next.size() == 3 && next[1] == a && next[2] == b && next[0] != c, "Wrong same-rope eviction");
        const auto floater = host.floating_window();
        RECT before{}, after{};
        GetWindowRect(floater, &before);
        drag(floater, center(before), head);
        GetWindowRect(floater, &after);
        const auto last = order(profile);
        host.stop();
        require(host.clean_exit, "Could not reopen after Floating copy");
        HistoryStore store(profile);
        require(last.size() == 3 && last[1] == next[0] && last[2] == a && last[0] != f &&
                store.entry(last[0]).snapshot_id == store.entry(f).snapshot_id && !store.entry(f).rope_id &&
                EqualRect(&before, &after), "Floating copy lost its source or inserted into the wrong slot");
    }
    if (fail) require(GetClipboardSequenceNumber() == sequence, "Rejected overview copy published content");
    else require(clipboard_text_is(sample::text(false)), "Overview head copy lost its saved text");
}
}

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc != 3 || std::wstring_view(argv[2]) != L"--desktop-input" || FAILED(OleInitialize(nullptr))) return 2;
    POINT cursor{};
    GetCursorPos(&cursor);
    const auto hwnd = CreateWindowExW(WS_EX_TOPMOST, L"STATIC", L"ClipRope external drop fixture", WS_POPUP | WS_VISIBLE,
        GetSystemMetrics(SM_CXSCREEN) - 300, GetSystemMetrics(SM_CYSCREEN) - 240, 210, 140, nullptr, nullptr, nullptr, nullptr);
    int result{};
    {
        auto receiver = Microsoft::WRL::Make<Receiver>();
        try {
            require(hwnd && SUCCEEDED(RegisterDragDrop(hwnd, receiver.Get())), "Could not register the OLE receiver");
            history_fixture::Directory temp;
            for (bool floating : {false, true})
                run(argv[1], temp.path / (floating ? "floating" : "hanging"), hwnd, *receiver.Get(), floating);
            overview(argv[1], temp.path / "rollback", true);
            overview(argv[1], temp.path / "overview", false);
        } catch (const std::exception& e) { std::cerr << e.what() << '\n'; result = 1; }
        RevokeDragDrop(hwnd);
    }
    DestroyWindow(hwnd);
    SetCursorPos(cursor.x, cursor.y);
    OleUninitialize();
    return result;
}
