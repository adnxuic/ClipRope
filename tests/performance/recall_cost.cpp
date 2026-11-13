#include "../integration/history_fixture.hpp"
#include "../integration/clipboard_process.hpp"
#include "../integration/resident.hpp"
#include "../integration/history_observer.hpp"
#include "../fault_injection/target_gate.hpp"
#include "persistence/history_store.hpp"
#include "persistence/hotkey_settings.hpp"
#include "persistence/privacy.hpp"
#include "platform_win/history_clipboard.hpp"
#include "platform_win/performance_counters.hpp"
#include "rendering/image_preview.hpp"
#include "platform_win/clipboard_object.hpp"
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <fstream>

namespace {
using namespace cliprope;
using Clock=std::chrono::steady_clock;
using history_fixture::require;
double ms(Clock::time_point t) { return std::chrono::duration<double,std::milli>(Clock::now()-t).count(); }
UINT WINAPI accept_keys(UINT count, LPINPUT input,int size) {
    if (count!=4 || !input || size!=sizeof(INPUT) || input[1].ki.wVk!='V') return 0;
    return count;
}
domain::Snapshot sample(unsigned width,unsigned height) {
    if (!width) return {{{CF_UNICODETEXT,{}, {'T',0,0,0}}}};
    BITMAPINFOHEADER h{};
    h.biSize=sizeof h; h.biWidth=width; h.biHeight=height; h.biPlanes=1; h.biBitCount=32; h.biCompression=BI_RGB;
    domain::Format f; f.standard_id=CF_DIB;
    f.bytes.resize(sizeof h+4*width*height);
    std::memcpy(f.bytes.data(),&h,sizeof h);
    for (unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x) {
        auto* pixel=f.bytes.data()+sizeof h+(size_t(y)*width+x)*4;
        pixel[0]=static_cast<unsigned char>(x/4);pixel[1]=static_cast<unsigned char>(y/8);
        pixel[2]=static_cast<unsigned char>((x+y)/16);pixel[3]=255;
    }
    return {{std::move(f)}};
}
void measure_ole_boundaries() {
    const std::wstring text = L"ClipRope AR3 OLE timing " + std::to_wstring(GetCurrentProcessId());
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.c_str());
    struct Cleanup {
        const std::wstring& text;
        ~Cleanup() { probe::clear_private_sample(text); }
    } clear{text};
    Microsoft::WRL::ComPtr<IDataObject> source;
    source.Attach(new ClipboardObject({{CF_UNICODETEXT, {bytes, bytes + (text.size() + 1) * sizeof(wchar_t)}}}));
    auto start = Clock::now();
    require(SUCCEEDED(OleSetClipboard(source.Get())), "OleSetClipboard failed");
    auto set = ms(start);
    FORMATETC format{CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    start = Clock::now();
    require(SUCCEEDED(source->GetData(&format, &medium)), "Published GetData failed");
    auto data = ms(start);
    ReleaseStgMedium(&medium);
    start = Clock::now();
    require(SUCCEEDED(OleFlushClipboard()), "OleFlushClipboard failed");
    auto flush = ms(start);
    require(probe::clear_private_sample(text), "Flushed sample mismatch");
    std::cout << "ole_text set_ms=" << set << " get_data_ms=" << data << " flush_ms=" << flush << '\n';
}
void measure(unsigned width,unsigned height) {
    history_fixture::Directory dir;
    persistence::HistoryStore store(dir.path);
    auto value=sample(width,height);
    auto id=store.admit("main",value);
    auto entry=store.entry(id);
    win::HistoryClipboard clipboard;
    for(unsigned trial=1;trial<=5;++trial) {
        auto start=Clock::now();
        auto lease=store.load_snapshot(entry.snapshot_id);
        auto read=ms(start);
        const auto& bytes=lease.snapshot.formats.front().bytes;
        start=Clock::now();
        require(persistence::sha256(bytes)==persistence::sha256(value.formats.front().bytes),"Snapshot bytes changed");
        auto hash_pair=ms(start);
        start=Clock::now();
        if(width) require(!rendering::encode_dib_png(CF_DIB,bytes).empty(),"Encoding failed");
        auto encode=ms(start);
        start=Clock::now();
        auto publication=clipboard.publish(lease);
        auto publish=ms(start);
        require(publication && clipboard.owns_publication(publication),"Could not publish synthetic offer");
        start=Clock::now();
        store.promote(id);
        auto commit=ms(start);
        start=Clock::now();
        win::EditGesture gesture; gesture.bind();
        auto check=ms(start);
        start=Clock::now();
        gesture.restore_interrupted_focus();
        auto restore=ms(start);
        start=Clock::now();
        require(win::send_paste_shortcut(accept_keys)==win::DeliveryResult::shortcut_sent,"One-shot send contract changed");
        std::cout<<"recall width="<<width<<" height="<<height<<" trial="<<trial<<" bytes="<<bytes.size()
                 <<" snapshot_read_hash_ms="<<read<<" independent_hash_pair_ms="<<hash_pair<<" dib_png_ms="<<encode
                 <<" publish_ms="<<publish<<" history_commit_ms="<<commit<<" private_focus_restore_ms="<<restore<<" private_focus_bind_ms="<<check<<" synthetic_send_ms="<<ms(start)
                 <<" target="<<gesture.target_status()<<'\n';
    }
    if (clipboard.owns_current()) OleSetClipboard(nullptr);
}
}
enum class DuringRead { none, delay, replace, revise, lost_target, quit };

unsigned receiver_inputs{};
LRESULT CALLBACK recall_receiver(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_PASTE || (msg == WM_KEYDOWN && w == 'V')) ++receiver_inputs;
    return DefWindowProcW(hwnd, msg, w, l);
}

void desktop_trial(const wchar_t* executable, DuringRead action) {
    history_fixture::Directory directory;
    auto multi = sample(1920, 1080);
    auto text = sample(0, 0);
    multi.formats.push_back(text.formats.front());
    domain::Id id;
    {
        persistence::HistoryStore seed(directory.path);
        application::CardPreferences prefs;
        prefs.paste_on_double_click = action == DuringRead::lost_target;
        persistence::save_card_preferences(seed.database(), prefs);
        persistence::save_privacy(seed.database(), {true, {}});
        id = seed.admit("main", multi);
    }
    const auto log = directory.path / "recall.log";
    const bool delayed = action != DuringRead::none;
    HWND receiver{};
    if (action == DuringRead::lost_target) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = recall_receiver;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ClipRope.TargetFixture";
        require(RegisterClassW(&wc), "Cannot register recall receiver");
        require(SetEnvironmentVariableW(L"CLIPROPE_TEST_TARGET_HOLD", L"uia-receiver"), "No bound target fixture");
    }
    probe::Resident host(executable, directory.path, true, log, delayed ? L"slow-snapshot" : nullptr);
    if (action == DuringRead::lost_target) {
        SetEnvironmentVariableW(L"CLIPROPE_TEST_TARGET_HOLD", nullptr);
        receiver = CreateWindowW(L"ClipRope.TargetFixture", L"", WS_POPUP | WS_VISIBLE, 0, 0, 120, 60,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(receiver && SetPropW(receiver, target_fixture::selection, reinterpret_cast<HANDLE>(1)),
                "Cannot bind the synthetic target");
    }
    const auto name = L"Local\\ClipRope.TestSnapshotRead." + std::to_wstring(host.pid());
    std::unique_ptr<void, decltype(&CloseHandle)> reading(CreateEventW(nullptr, TRUE, FALSE, name.c_str()), CloseHandle);
    require(bool(reading), "Read witness event unavailable");
    ClipboardPublisher sentinel;
    const Representation marker{CF_UNICODETEXT, {'S',0,0,0}};
    require(SUCCEEDED(sentinel.publish({marker})), "Could not set private sentinel");
    const auto initial_sequence = GetClipboardSequenceNumber();
    auto point = SendMessageW(host.listener(), win::performance_query, WPARAM(win::PerformanceCounter::first_card_point), 0);
    POINT p{short(LOWORD(point)), short(HIWORD(point))};
    require(point && ScreenToClient(host.workspace, &p), "No recall target in workspace");
    const auto start = Clock::now();
    for (auto msg : {WM_LBUTTONDOWN, WM_LBUTTONUP, WM_LBUTTONDBLCLK, WM_LBUTTONUP})
        require(PostMessageW(host.workspace, msg, msg == WM_LBUTTONUP ? 0 : MK_LBUTTON, MAKELPARAM(p.x,p.y)),
                "Could not post Hanging recall");
    if (delayed) probe::pump_until([&] { return WaitForSingleObject(reading.get(), 0) == WAIT_OBJECT_0; },
                                   "Desktop action did not reach the delayed read");
    if (action == DuringRead::replace) {
        require(SUCCEEDED(sentinel.publish({marker})), "Could not replace the private clipboard during preparation");
    } else if (action == DuringRead::revise) {
        auto writer = history_fixture::observe(directory.path, SQLITE_OPEN_READWRITE);
        persistence::Statement revise(writer.get(), "UPDATE entries SET revision=revision+1 WHERE id=?");
        revise.bind(1,id);
        revise.run();
    } else if (action == DuringRead::lost_target) {
        require(DestroyWindow(receiver), "Could not close the bound target during its read");
    } else if (action == DuringRead::quit) {
        host.exit();
        require(GetClipboardSequenceNumber() == initial_sequence && probe::clear_private_sample(L"S"),
                "Quit dispatched a late publication");
        return;
    }
    const bool stale = action == DuringRead::replace || action == DuringRead::revise;
    const auto sequence = GetClipboardSequenceNumber();
    double maximum{};
    unsigned beats{};
    probe::pump_until([&] {
        DWORD_PTR ignored{};
        const auto begin = Clock::now();
        require(SendMessageTimeoutW(host.listener(),WM_NULL,0,0,SMTO_BLOCK,1500,&ignored) != 0, "Recall heartbeat timeout");
        maximum = std::max(maximum,ms(begin));
        ++beats;
        std::ifstream receipt(log);
        const std::string output((std::istreambuf_iterator<char>(receipt)), {});
        const auto outcome = stale ? " failed skipped skipped" :
            action == DuringRead::lost_target ? " published lost-target committed" : " published skipped committed";
        return output.find("invoke double-click " + id + outcome) != output.npos;
    }, "No committed copy-only recall");
    const auto total = ms(start);
    auto quit = Clock::now();
    host.exit();
    const auto exit = ms(quit);
    require((!stale || GetClipboardSequenceNumber() == sequence) && probe::clear_private_sample(stale ? L"S" : L"T"),
            "Unexpected private clipboard after exit flush");
    if (action == DuringRead::delay) require(maximum < 100, "Slow snapshot read blocked the listener");
    require(receiver_inputs == 0, "Closed target received late input");
    std::cout << "desktop_recall bytes=" << multi.formats[0].bytes.size() << " injected_read_ms=" << (delayed ? 200 : 0)
              << " action_ms=" << total << " heartbeat_max_ms=" << maximum << " heartbeats=" << beats
              << " quit_including_flush_ms=" << exit << '\n';
}
void screenshot_trial(const wchar_t* executable, bool original, bool close_during_read) {
    history_fixture::Directory directory;
    const auto snapshot = sample(1280, 720);
    const auto& dib = snapshot.formats.front().bytes;
    auto stamp = Clock::now();
    const auto png = rendering::encode_dib_png(CF_DIB, dib);
    const double encode = ms(stamp);
    const auto images = directory.path / "originals";
    std::filesystem::create_directory(images);
    auto image = images / "capture.png";
    if (original) {
        std::ofstream out(image, std::ios::binary);
        out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        require(out.good(), "Synthetic original was not saved");
    }
    domain::Id id, snapshot_id;
    {
        persistence::HistoryStore seed(directory.path);
        auto pixels = snapshot;
        if (original) {
            auto path = images.u8string();
            pixels.snip_directory.assign(reinterpret_cast<const char*>(path.data()), path.size());
            pixels.snip_begin = 1;
            pixels.snip_end = std::numeric_limits<std::int64_t>::max();
        }
        id = seed.admit("main", pixels);
        snapshot_id = seed.entry(id).snapshot_id;
        seed.detach(id, {"", 60, 60, 300, 200});
    }
    if (original) {
        for (unsigned i = 0; i < 3; ++i) {
            stamp = Clock::now();
            require(rendering::matches_image_file(image.wstring(), CF_DIB, dib), "Original PNG pixels changed");
            std::cout << "original_match trial=" << i << " ms=" << ms(stamp) << " standalone_encode_ms=" << encode << '\n';
        }
    }
    const auto log = directory.path / "shot.log";
    const auto sequence = GetClipboardSequenceNumber();
    probe::Resident host(executable, directory.path, false, log, close_during_read ? L"shot-delay" : L"shot-observe");
    auto window = probe::owned_window(host.pid(), L"ClipRope.Floating");
    RECT rect{};
    require(window && GetClientRect(window, &rect), "Missing restored screenshot window");
    const auto name = L"Local\\ClipRope.TestSnapshotRead." + std::to_wstring(host.pid());
    std::unique_ptr<void, decltype(&CloseHandle)> reading(CreateEventW(nullptr, TRUE, FALSE, name.c_str()), CloseHandle);
    require(bool(reading), "No screenshot read event");
    auto receipts = [&] {
        std::ifstream in(log);
        std::string line;
        unsigned count{};
        while (std::getline(in, line)) count += line == "screenshot-ready " + id;
        return count;
    };
    for (unsigned i = 0; i < (close_during_read ? 1u : 3u); ++i) {
        const auto start = Clock::now();
        require(GetClientRect(window, &rect), "Screenshot body unavailable");
        const auto body_y = MulDiv(48, GetDpiForWindow(window), 96);
        for (auto msg : {WM_LBUTTONDOWN, WM_LBUTTONUP, WM_LBUTTONDBLCLK, WM_LBUTTONUP})
            require(PostMessageW(window, msg, msg == WM_LBUTTONUP ? 0 : MK_LBUTTON,
                                 MAKELPARAM(rect.right/2, body_y)), "Screenshot click unavailable");
        if (close_during_read) {
            probe::pump_until([&] { return WaitForSingleObject(reading.get(), 0) == WAIT_OBJECT_0; }, "No screenshot read");
            require(GetClientRect(window, &rect), "Screenshot toolbar was destroyed before close");
            const auto inset = MulDiv(19, GetDpiForWindow(window), 96);
            for (auto msg : {WM_LBUTTONDOWN, WM_LBUTTONUP})
                SendMessageW(window, msg, msg == WM_LBUTTONUP ? 0 : MK_LBUTTON, MAKELPARAM(rect.right-inset, inset));
            try { probe::pump_until([&] { return !IsWindow(window); }, "Closing a pending screenshot left its window open"); }
            catch (...) { std::ifstream trace(log); std::cerr << trace.rdbuf(); throw; }
            require(receipts() == 0, "Screenshot completion opened a retired destination");
        } else {
            double longest{};
            try { probe::pump_until([&] {
                auto beat = Clock::now();
                DWORD_PTR ignored{};
                require(SendMessageTimeoutW(host.listener(), WM_NULL, 0, 0, SMTO_BLOCK, 1000, &ignored), "Screenshot heartbeat lost");
                longest = std::max(longest, ms(beat));
                return receipts() == i+1;
            }, "Screenshot was not ready at the UI launch boundary"); }
            catch (...) { std::ifstream trace(log); std::cerr << trace.rdbuf(); throw; }
            std::cout << "desktop_screenshot original=" << original << " trial=" << i
                      << " action_ms=" << ms(start) << " heartbeat_max_ms=" << longest << '\n';
        }
    }
    host.exit();
    persistence::HistoryStore reopen(directory.path);
    require(GetClipboardSequenceNumber() == sequence, "Opening a screenshot changed the clipboard");
    if (close_during_read) {
        require(reopen.entry(id).rope_id == domain::rebin_id && receipts() == 0, "Screenshot close was not committed");
    } else {
        const auto stored = reopen.load(id).snapshot;
        const auto derived = directory.path / "screenshots" / (snapshot_id + ".png");
        require(!reopen.entry(id).rope_id && reopen.entry(id).snapshot_id == snapshot_id, "Open changed membership");
        if (original) {
            const auto expected = image.u8string();
            require(stored.screenshot_path == std::string_view(reinterpret_cast<const char*>(expected.data()), expected.size()) &&
                    !std::filesystem::exists(derived), "Original match was not used");
        }
        else {
            std::ifstream in(derived, std::ios::binary);
            const std::vector<unsigned char> saved((std::istreambuf_iterator<char>(in)), {});
            require(saved == png, "Prepared derived PNG differs from the saved pixels");
        }
    }
}
int wmain(int argc, wchar_t** argv) {
    if (!probe::private_station() || FAILED(OleInitialize(nullptr))) return 2;
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int status{};
    try {
        if (argc == 2 || argc == 3) {
            if (argc == 3 && std::wstring_view(argv[2]) == L"--screenshot") {
                screenshot_trial(argv[1], false, false);
                screenshot_trial(argv[1], true, false);
                screenshot_trial(argv[1], false, true);
            } else if (argc == 3) {
                desktop_trial(argv[1],DuringRead::replace);
                desktop_trial(argv[1],DuringRead::revise);
                desktop_trial(argv[1],DuringRead::lost_target);
                desktop_trial(argv[1],DuringRead::quit);
            } else {
                desktop_trial(argv[1],DuringRead::none);
                desktop_trial(argv[1],DuringRead::delay);
            }
        } else {measure_ole_boundaries();measure(0,0);measure(1280,720);measure(1920,1080);}
    }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';status=1;}
    OleUninitialize();
    return status;
}
