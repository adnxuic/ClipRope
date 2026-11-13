#include "persistence/hotkey_settings.hpp"
#include "platform_win/workspace_commands.hpp"
#include "platform_win/menu_timing.hpp"
#include "platform_win/tray.hpp"
#include <windowsx.h>
#include <shellapi.h>
#include "../clipboard_sources/sample.hpp"
#include "../integration/clipboard_process.hpp"
#include "../integration/history_fixture.hpp"
#include "capture_worker/capture.hpp"
#include "persistence/history_store.hpp"
#include "persistence/localization_settings.hpp"
#include "platform_win/clipboard_object.hpp"
#include "platform_win/performance_counters.hpp"
#include "presentation/image_card_layout.hpp"
#include "process_metrics.hpp"
#include "order_fixture.hpp"
#include "../compatibility/hidden_tray.hpp"
#include "../clipboard_sources/image_file_sample.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mmsystem.h>
#include <thread>

namespace {
using namespace cliprope;
using namespace performance;
using namespace persistence;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using history_fixture::require;
double ms(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }
void pump() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}
template <class F> bool wait(F f, int timeout = 4500) {
    const auto end = Clock::now() + std::chrono::milliseconds(timeout);
    do {
        pump();
        if (f())
            return true;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 1, QS_ALLINPUT);
    } while (Clock::now() < end);
    return false;
}
HWND owned(DWORD pid, const wchar_t *cls) {
    HWND h = nullptr;
    while ((h = FindWindowExW(nullptr, h, cls, nullptr))) {
        DWORD p{};
        GetWindowThreadProcessId(h, &p);
        if (p == pid)
            return h;
    }
    return nullptr;
}
LRESULT send(HWND w, UINT m, WPARAM p = 0, LPARAM l = 0) {
    DWORD_PTR result{};
    if (!SendMessageTimeoutW(w, m, p, l, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 3500, &result))
        throw std::runtime_error("Benchmark target failed to respond");
    return static_cast<LRESULT>(result);
}
struct Host {
    HANDLE process{};
    DWORD pid{};
    HWND listener{}, workspace{};
    double startup{};
    const bool isolated = probe::private_station();
    Host(const fs::path &exe, const fs::path &profile, const fs::path &report = {}) {
        const auto start = Clock::now();
        std::vector<std::wstring> args{exe.wstring(), L"--profile", profile.wstring()};
        if (!report.empty()) {
            args.push_back(L"--probe");
            args.push_back(report.wstring());
        }
        process = probe::launch(args, probe::desktop_path());
        require(process, "Could not launch desktop");
        pid = GetProcessId(process);
        if (!wait(
                [&] {
                    listener = owned(pid, L"ClipRope.Listener");
                    wchar_t title[32]{};
                    return listener && GetWindowTextW(listener, title, 32) &&
                           std::wstring_view(title) == L"ClipRope";
                },
                8000)) {
            stop();
            throw std::runtime_error("Listener readiness timed out");
        }
        startup = ms(start);
        workspace = owned(pid, L"ClipRope.Workspace");
        require(workspace, "Workspace is missing");
    }
    ~Host() { stop(); }
    void stop() {
        if (!process)
            return;
        if (listener)
            PostMessageW(listener, WM_CLOSE, 0, 0);
        if (!wait([&] { return WaitForSingleObject(process, 0) == WAIT_OBJECT_0; }, 5000)) {
            TerminateProcess(process, 2);
            WaitForSingleObject(process, 3000);
        }
        CloseHandle(process);
        process = nullptr;
    }
    void exit_cleanly() {
        require(process && PostMessageW(listener, WM_CLOSE, 0, 0), "Cannot request measured exit");
        require(wait([&] { return WaitForSingleObject(process, 0) == WAIT_OBJECT_0; }, 5000), "Measured exit timed out");
        DWORD status{};
        require(GetExitCodeProcess(process, &status) && !status, "Measured exit reported failure");
        CloseHandle(process);
        process = nullptr;
    }
    LRESULT metric(win::PerformanceCounter counter) {
        return send(listener, win::performance_query, static_cast<WPARAM>(counter));
    }
    bool visible() const { return (GetWindowLongPtrW(workspace, GWL_STYLE) & WS_VISIBLE) != 0; }
    void toggle() {
        // The isolated station has no interactive hotkey owner. Exercise the
        // notification icon's existing activation route on that station.
        if (isolated)
            send(listener, win::Tray::callback, 0, MAKELPARAM(NIN_SELECT, win::Tray::icon_id));
        else
            send(listener, WM_HOTKEY, 0x4260, MAKELPARAM(MOD_CONTROL | MOD_ALT, VK_F24));
    }
    POINT at(int x, int y) const {
        const auto d = GetDpiForWindow(workspace);
        return {MulDiv(x, d, 96), MulDiv(y, d, 96)};
    }
    void click(int x, int y) {
        auto p = at(x, y);
        send(workspace, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(p.x, p.y));
        send(workspace, WM_LBUTTONUP, 0, MAKELPARAM(p.x, p.y));
    }
    void wheel(int direction) {
        POINT p = at(60, 140);
        ClientToScreen(workspace, &p);
        send(workspace, WM_MOUSEWHEEL, MAKEWPARAM(0, direction * WHEEL_DELTA), MAKELPARAM(p.x, p.y));
    }
    void paint() { send(workspace, WM_PAINT); }
};
struct Sql {
    sqlite3 *db{};
    fs::path root;
    DWORD host_pid;
    unsigned receipts{}, partial_receipts{};
    Sql(const fs::path &profile, DWORD pid) : root(profile), host_pid(pid) {
        const auto u8 = (profile / "history.db").u8string();
        require(sqlite3_open_v2(reinterpret_cast<const char *>(u8.c_str()), &db, SQLITE_OPEN_READONLY,
                                nullptr) == SQLITE_OK,
                "Read-only history unavailable");
        sqlite3_busy_timeout(db, 1000);
    }
    ~Sql() {
        std::cout << "Verified synthetic copy receipts " << receipts << ", partial " << partial_receipts
                  << '\n';
        sqlite3_close(db);
    }
    std::string head() {
        Statement q(db, "SELECT e.id FROM entries e JOIN app_settings a ON a.active_rope_id=e.rope_id WHERE "
                        "e.state='Hanging' ORDER BY e.sort_key,e.id LIMIT 1");
        return q.step() ? q.text(0) : "";
    }
    std::string active() {
        Statement q(db, "SELECT active_rope_id FROM app_settings");
        require(q.step(), "Missing active collector");
        return q.text(0);
    }
    std::string head(std::string_view rope) {
        Statement q(
            db, "SELECT id FROM entries WHERE state='Hanging' AND rope_id=? ORDER BY sort_key,id LIMIT 1");
        q.bind(1, rope);
        return q.step() ? q.text(0) : "";
    }
    int count(const char *sql) {
        Statement s(db, sql);
        require(s.step(), "Observation query failed");
        return int(s.integer(0));
    }
    std::size_t blob_files() const {
        std::size_t n = 0;
        for (auto &f : fs::recursive_directory_iterator(root / "blobs"))
            if (f.is_regular_file() && f.path().extension() == ".blob")
                ++n;
        return n;
    }
};
struct Logs {
    std::ofstream timing, resources, events;
    explicit Logs(const fs::path &folder) {
        fs::create_directories(folder);
        timing.open(folder / "timing.csv");
        resources.open(folder / "resources.csv");
        events.open(folder / "events.csv");
        require(timing && resources && events, "Cannot open measurement outputs");
        timing << "series,trial,ms\n";
        resources << "series,step,host_private,worker_private,working_set,handles,threads,processes,gpu_"
                     "dedicated,gpu_shared,dwm_private,dwm_gpu_dedicated,dwm_gpu_shared,blobs,blob_files,"
                     "floating,visible_cards,thumbnail_bytes,bitmap_bytes,gdi,user,cpu_ticks,host_ws,worker_ws,"
                     "painted_frames,render_target_creations\n";
        events << "series,step,value\n";
        timing << std::fixed << std::setprecision(4);
    }
    void time(const char *series, int trial, double value) {
        timing << series << ',' << trial << ',' << value << '\n';
    }
    void event(const char *series, int step, std::int64_t v) {
        events << series << ',' << step << ',' << v << std::endl;
    }
    void phase(const char *name, int step) {
        LARGE_INTEGER ticks{};
        QueryPerformanceCounter(&ticks);
        events << name << ',' << step << ',' << ticks.QuadPart << '\n';
    }
    void resource(const char *series, int step, Host &host, Sql &sql, GpuCounters &gpu) {
        auto p = process_tree(host.pid);
        const auto g = gpu.read(p.ids);
        resources << series << ',' << step << ',' << p.host_private << ',' << p.worker_private << ','
                  << p.working_set << ',' << p.handles << ',' << p.threads << ',' << p.processes << ','
                  << g.dedicated << ',' << g.shared << ',' << g.dwm_private << ',' << g.dwm_dedicated << ','
                  << g.dwm_shared << ',' << sql.count("SELECT count(*) FROM blobs") << ',' << sql.blob_files()
                  << ',' << sql.count("SELECT count(*) FROM entries WHERE state='Floating'") << ','
                  << (host.visible() ? host.metric(win::PerformanceCounter::visible_cards) : 0) << ','
                  << host.metric(win::PerformanceCounter::thumbnail_bytes) << ','
                  << host.metric(win::PerformanceCounter::bitmap_bytes) << ',' << p.gdi << ',' << p.user << ','
                  << p.cpu_ticks << ',' << p.host_working_set << ',' << p.worker_working_set << ','
                  << host.metric(win::PerformanceCounter::painted_frames) << ','
                  << host.metric(win::PerformanceCounter::render_target_creations) << std::endl;
    }
};
Representation dib(int n, unsigned label) {
    BITMAPINFOHEADER h{};
    h.biSize = sizeof(h);
    h.biWidth = h.biHeight = n;
    h.biPlanes = 1;
    h.biBitCount = 24;
    const auto stride = (n * 3 + 3) & ~3;
    h.biSizeImage = stride * n;
    Representation image{CF_DIB, std::vector<unsigned char>(sizeof(h) + h.biSizeImage)};
    std::memcpy(image.bytes.data(), &h, sizeof h);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const auto i = sizeof(h) + y * stride + x * 3;
            image.bytes[i] = static_cast<BYTE>(x + label);
            image.bytes[i + 1] = static_cast<BYTE>(y + label * 3);
            image.bytes[i + 2] = static_cast<BYTE>(label * 9);
        }
    return image;
}
domain::Snapshot snapshot(std::vector<Representation> formats) {
    domain::Snapshot s;
    for (auto &native : formats) {
        domain::Format f;
        if (native.format < 0xc000)
            f.standard_id = native.format;
        else {
            const auto name = format_name(native.format);
            const auto length =
                WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()),
                                    nullptr, 0, nullptr, nullptr);
            require(length > 0, "Registered format name unavailable");
            f.name.resize(length);
            WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), f.name.data(), length,
                                nullptr, nullptr);
        }
        f.tymed = native.tymed;
        f.index = native.index;
        f.aspect = native.aspect;
        f.codec_version = native.codec_version;
        f.bytes = std::move(native.bytes);
        s.formats.push_back(std::move(f));
    }
    return s;
}
void seed_pictures(const fs::path& profile) {
    HistoryStore store(profile);
    store.set_capacity("main", 96);
    save_shortcut(store.database(), application::workspace_toggle, {true, "Ctrl+Alt+F24"});
    const auto dir = profile.parent_path() / "photos";
    fs::create_directory(dir);
    for (unsigned i = 0; i < 96; ++i) {
        auto image = dib(512, i);
        BITMAPFILEHEADER header{};
        header.bfType = 0x4d42;
        header.bfSize = static_cast<DWORD>(sizeof header + image.bytes.size());
        header.bfOffBits = sizeof header + sizeof(BITMAPINFOHEADER);
        auto path = dir / (std::to_string(i) + ".bmp");
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<char*>(&header), sizeof header);
        out.write(reinterpret_cast<char*>(image.bytes.data()), image.bytes.size());
        out.close();
        require(bool(out), "Could not write benchmark image");
        store.admit("main", snapshot({image_file_sample::drop({path})}));
    }
}
void seed(const fs::path &profile) {
    HistoryStore store(profile);
    store.set_capacity("main", 48);
    std::vector<domain::Id> ropes{"main", store.create_rope("Second", 48), store.create_rope("Third", 48)};
    save_shortcut(store.database(), application::workspace_toggle, {true, "Ctrl+Alt+F24"});
    unsigned image = 0;
    for (auto &rope : ropes)
        for (int i = 0; i < 48; ++i)
            store.admit(rope, i % 2 ? snapshot(sample::formats(false)) : snapshot({dib(192, ++image)}));
    store.gc();
}
class RawSource {
    HWND window_{};
    unsigned serial_{};

  public:
    RawSource() {
        window_ = CreateWindowW(L"STATIC", L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
        require(window_, "Source window unavailable");
    }
    ~RawSource() {
        if (GetClipboardOwner() == window_ && OpenClipboard(nullptr)) {
            if (GetClipboardOwner() == window_)
                EmptyClipboard();
            CloseClipboard();
        }
        DestroyWindow(window_);
    }
    void put(const Representation &item) {
        auto mem = GlobalAlloc(GMEM_MOVEABLE, item.bytes.size());
        require(mem, "Source allocation failed");
        auto p = GlobalLock(mem);
        require(p, "Source lock failed");
        std::memcpy(p, item.bytes.data(), item.bytes.size());
        GlobalUnlock(mem);
        if (!wait([&] { return OpenClipboard(window_) != 0; }, 800)) {
            GlobalFree(mem);
            throw std::runtime_error("Clipboard busy");
        }
        const bool ok = EmptyClipboard() && SetClipboardData(item.format, mem);
        CloseClipboard();
        if (!ok) {
            GlobalFree(mem);
            throw std::runtime_error("Source publication failed");
        }
    }
    std::wstring issued_text() const { return L"ClipRope synthetic " + std::to_wstring(serial_); }
    Representation text() {
        ++serial_;
        const auto text = issued_text();
        const auto *p = reinterpret_cast<const BYTE *>(text.c_str());
        return {CF_UNICODETEXT, {p, p + (text.size() + 1) * sizeof(wchar_t)}};
    }
    bool owns_clipboard() const { return GetClipboardOwner() == window_; }
    const char *verify_current(DWORD sequence, const Representation &expected) const {
        if (GetClipboardSequenceNumber() != sequence || !owns_clipboard())
            return "owner-or-sequence-changed";
        if (!OpenClipboard(window_))
            return "clipboard-busy";
        struct Close {
            ~Close() { CloseClipboard(); }
        } close;
        if (GetClipboardSequenceNumber() != sequence || !owns_clipboard())
            return "owner-or-sequence-changed";
        const auto memory = GetClipboardData(expected.format);
        if (!memory || GlobalSize(memory) < expected.bytes.size())
            return "missing-or-short";
        const auto bytes = GlobalLock(memory);
        if (!bytes)
            return "lock-failed";
        const bool equal = std::memcmp(bytes, expected.bytes.data(), expected.bytes.size()) == 0;
        GlobalUnlock(memory);
        return equal ? "match" : "mismatch";
    }
};
DWORD health(HWND listener, win::PerformanceCounter key) {
    return static_cast<DWORD>(send(listener, win::performance_query, static_cast<WPARAM>(key)));
}
void capture_receipt(std::ostream &out, HWND listener) {
    using C = win::PerformanceCounter;
    const auto counter = [&](C id) { return health(listener, id); };
    out << "Listener updates=" << counter(C::clipboard_events) << " seq=" << counter(C::clipboard_sequence)
        << ", observed seq=" << counter(C::capture_observed_sequence) << " "
        << win::capture_status_name(counter(C::capture_observation))
        << ", finished seq=" << counter(C::capture_completed_sequence) << " "
        << win::capture_status_name(counter(C::capture_outcome))
        << " worker_exit=" << counter(C::capture_worker_exit) << " hr=" << counter(C::capture_hresult)
        << " formats=" << counter(C::capture_format_count) << " saved=" << counter(C::capture_saved_count)
        << " retries=" << counter(C::capture_retries)
        << " unicode=" << win::unicode_status_name(counter(C::capture_unicode))
        << " job_peak_kib=" << counter(C::capture_job_peak_kib)
        << " reacquisitions=" << counter(C::capture_reacquisitions)
        << " recovery_guard=" << win::recovery_guard_name(counter(C::capture_recovery_guard))
        << " monitor_query=" << counter(C::capture_monitor_query)
        << " history_query=" << counter(C::capture_history_query)
        << " native_first=" << counter(C::capture_native_first)
        << " native_count=" << counter(C::capture_native_count)
        << " native_error=" << counter(C::capture_native_error)
        << " user_objects=" << counter(C::capture_user_objects)
        << " gdi_objects=" << counter(C::capture_gdi_objects)
        << " worker_handles=" << counter(C::capture_worker_handles) << std::endl;
}
double capture(RawSource &source, Sql &sql, const Representation &value) {
    const auto collector = sql.active();
    const auto previous = sql.head(collector);
    auto start = Clock::now();
    source.put(value);
    const auto published = GetClipboardSequenceNumber();
    unsigned held = 0, changed = 0;
    bool worker_seen = false;
    auto next_process_check = start + 350ms;
    const bool committed = wait([&] {
        if (GetOpenClipboardWindow())
            ++held;
        if (!source.owns_clipboard() || GetClipboardSequenceNumber() != published)
            ++changed;
        if (Clock::now() >= next_process_check) {
            worker_seen |= process_tree(sql.host_pid).processes > 1;
            next_process_check += 100ms;
        }
        return sql.head(collector) != previous;
    });
    if (!committed) {
        Statement saved(sql.db, "SELECT count(*) FROM entries e JOIN representations r ON "
                                "r.snapshot_id=e.snapshot_id WHERE r.blob_id=?");
        saved.bind(1, sha256(value.bytes));
        require(saved.step(), "Could not inspect failed capture");
        DWORD_PTR response{};
        HWND listener = owned(sql.host_pid, L"ClipRope.Listener");
        const bool responsive =
            SendMessageTimeoutW(listener, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 300, &response) != 0;
        std::cerr << "No committed head in the bound rope after " << ms(start) << " ms. Sequence "
                  << published << " -> " << GetClipboardSequenceNumber() << ", fixture owner "
                  << source.owns_clipboard() << ", observed open-lock polls " << held << ", changed polls "
                  << changed << ", same active rope " << (collector == sql.active())
                  << ", matching synthetic blobs on all entries " << saved.integer(0)
                  << ", worker seen past 350 ms " << worker_seen << ", listener responsive " << responsive
                  << ", owned source bytes " << source.verify_current(published, value) << '\n';
        if (responsive)
            capture_receipt(std::cerr, listener);
        throw std::runtime_error("Synthetic capture did not commit");
    }
    const auto latency = ms(start);
    Statement receipt(sql.db,
                      "SELECT s.capture_status FROM entries e JOIN snapshots s ON s.id=e.snapshot_id "
                      "JOIN representations r ON r.snapshot_id=e.snapshot_id "
                      "WHERE e.id=? AND r.standard_id=? AND r.blob_id=? AND r.status='saved' LIMIT 1");
    receipt.bind(1, sql.head(collector));
    receipt.bind(2, static_cast<int>(value.format));
    receipt.bind(3, sha256(value.bytes));
    require(receipt.step(), "Committed head does not contain the fixture's published bytes");
    ++sql.receipts;
    const auto listener = owned(sql.host_pid, L"ClipRope.Listener");
    const bool partial = receipt.text(0) == "partial";
    if (partial)
        ++sql.partial_receipts;
    if (partial || health(listener, win::PerformanceCounter::capture_reacquisitions) > 0) {
        std::cout << "Recovered copy seq=" << published << " partial=" << partial << " ";
        capture_receipt(std::cout, listener);
    }
    return latency;
}
void startup_trials(const fs::path &exe, const fs::path &root, Logs &log, int n) {
    for (int i = 0; i < n; ++i) {
        // Every trial exercises first launch of a fresh per-user store.
        Host host(exe, root / ("cold-" + std::to_string(i)));
        log.time("cold_process_ready", i, host.startup);
    }
}
void select_rope(Host &h, Sql &sql, int row, bool via_overview = true) {
    if (via_overview) {
        SendMessageW(h.workspace, WM_COMMAND, static_cast<WPARAM>(win::WorkspaceCommand::overview), 0);
        if (!h.isolated) send(h.workspace, WM_COMMAND, static_cast<WPARAM>(win::WorkspaceCommand::keyboard));
    }
    Statement expected(sql.db, "SELECT id FROM ropes WHERE id<>'system:rebin' ORDER BY sort_rank,id LIMIT 1 OFFSET ?");
    expected.bind(1, row);
    require(expected.step(), "Expected overview rope is missing");
    const auto target = expected.text(0);
    // Release the SQLite reader's snapshot before observing collector changes.
    expected.step();
    for (int i = 0; i < 3 && sql.active() != target; ++i) {
        if (h.isolated) send(h.workspace, WM_COMMAND, static_cast<WPARAM>(win::WorkspaceCommand::next));
        else send(h.workspace, WM_KEYDOWN, VK_TAB);
    }
    require(sql.active() == target, "Rope selection reached the wrong collector");
    if (via_overview) send(h.workspace, WM_COMMAND, static_cast<WPARAM>(win::WorkspaceCommand::overview));
}
HWND detach(Host &h, int offset = 0) {
    SendMessageW(h.workspace, WM_COMMAND, static_cast<WPARAM>(cliprope::win::WorkspaceCommand::latest), 0); // Bind a known visible body even if prior scrolling ended at a fractional offset.
    std::vector<HWND> before;
    HWND item = nullptr;
    while ((item = FindWindowExW(nullptr, item, L"ClipRope.Floating", nullptr))) {
        DWORD pid{};
        GetWindowThreadProcessId(item, &pid);
        if (pid == h.pid)
            before.push_back(item);
    }
    RECT bounds{};
    require(GetClientRect(h.workspace, &bounds), "Workspace bounds unavailable");
    auto a = h.at(45, 140), b = h.at(100 + (offset % 8) * 28, 1 + (offset % 4) * 24);
    b.y += bounds.bottom;
    send(h.workspace, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(a.x, a.y));
    send(h.workspace, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(b.x, b.y));
    send(h.workspace, WM_LBUTTONUP, 0, MAKELPARAM(b.x, b.y));
    item = nullptr;
    while ((item = FindWindowExW(nullptr, item, L"ClipRope.Floating", nullptr))) {
        DWORD pid{};
        GetWindowThreadProcessId(item, &pid);
        if (pid == h.pid && std::find(before.begin(), before.end(), item) == before.end())
            return item;
    }
    throw std::runtime_error("Detach failed");
}
void rehang(HWND h) {
    const auto d = GetDpiForWindow(h);
    const auto pt = MAKELPARAM(MulDiv(40, d, 96), MulDiv(16, d, 96));
    send(h, WM_LBUTTONDOWN, MK_LBUTTON, pt);
    send(h, WM_LBUTTONUP, 0, pt);
}
void spin_until(Clock::time_point due) {
    while (Clock::now() + 2ms < due) {
        pump();
        Sleep(1);
    }
    while (Clock::now() < due)
        std::this_thread::yield();
}
class Peak {
    std::atomic<bool> stop_{false};
    std::thread sampler_;

  public:
    std::uint64_t total{}, worker{}, host{}, host_ws{}, worker_ws{}, worker_commit{};
    unsigned processes{};
    explicit Peak(DWORD pid) {
        sampler_ = std::thread([this, pid] {
            while (!stop_) {
                try {
                    auto sample = process_tree(pid);
                    total = std::max(total, sample.total_private());
                    worker = std::max(worker, sample.worker_private);
                    host = std::max(host, sample.host_private);
                    host_ws = std::max(host_ws, sample.host_working_set);
                    worker_ws = std::max(worker_ws, sample.worker_working_set);
                    worker_commit = std::max(worker_commit, sample.worker_peak_commit);
                    processes = std::max(processes, sample.processes);
                } catch (...) {
                }
                Sleep(2);
            }
        });
    }
    void finish() {
        stop_ = true;
        if (sampler_.joinable())
            sampler_.join();
    }
    ~Peak() { finish(); }
};
void scroll_frames(Host &h, Sql &sql, Logs &logs, GpuCounters &gpu, bool quick, bool cold_files = false) {
    h.toggle();
    SendMessageW(h.workspace, WM_COMMAND, static_cast<WPARAM>(cliprope::win::WorkspaceCommand::density), 0);
    h.paint();
    if (!cold_files) Sleep(quick ? 150 : 1000);
    h.paint();
    // The rope spans the work area, so wider displays show more than the budgeted 12.
    const auto visible = h.metric(win::PerformanceCounter::visible_cards);
    if (cold_files) {
        RECT bounds{};
        GetClientRect(h.workspace, &bounds);
        const auto width = MulDiv(bounds.right, 96, GetDpiForWindow(h.workspace));
        // On a cold open the pending preview is wider than the measured square.
        application::CardSummary pending;
        pending.kind = application::CardKind::image;
        const auto stride = presentation::ImageCardLayout::measure(pending).advance();
        require(visible >= std::max(1, int((width - 28) / stride)), "Adaptive file rope is underfilled");
    } else {
        RECT bounds{};
        GetClientRect(h.workspace, &bounds);
        const auto width = MulDiv(bounds.right, 96, GetDpiForWindow(h.workspace));
        require(visible >= std::max(1, (width - 28) / 230), "Mixed rope is underfilled");
    }
    logs.resource("compact", 0, h, sql, gpu);
    const int frames = quick ? 35 : 240;
    auto due = Clock::now();
    int missed = 0;
    std::uint64_t peak_cpu = 0, peak_bitmaps = 0, peak_faces = 0;
    for (int i = 0; i < frames; ++i) {
        spin_until(due);
        auto begin = Clock::now();
        const auto before = h.metric(win::PerformanceCounter::painted_frames);
        const int sweep = cold_files ? 100 : 12;
        h.wheel((i / sweep) % 2 ? 1 : -1);
        const auto after_wheel = Clock::now();
        h.paint();
        require(h.metric(win::PerformanceCounter::painted_frames) > before, "Frame was not painted");
        const auto after_paint = Clock::now();
        logs.time("scroll_dispatch", i,
                  std::chrono::duration<double, std::milli>(after_wheel - begin).count());
        logs.time("scroll_paint", i,
                  std::chrono::duration<double, std::milli>(after_paint - after_wheel).count());
        logs.time("scroll_app", i, std::chrono::duration<double, std::milli>(after_paint - begin).count());
        if (h.metric(win::PerformanceCounter::compositor_fence)) logs.time("scroll_present", i, ms(begin));
        else {
            require(h.isolated, "DWM fence unavailable");
            logs.event("scroll_fence_unavailable", i, 1);
        }
        if (cold_files) {
            using enum win::PerformanceCounter;
            peak_cpu = std::max(peak_cpu, std::uint64_t(h.metric(thumbnail_bytes)));
            peak_bitmaps = std::max(peak_bitmaps, std::uint64_t(h.metric(preview_bitmap_bytes)));
            peak_faces = std::max(peak_faces, std::uint64_t(h.metric(moving_face_bytes)));
        }
        const auto next = due + std::chrono::microseconds(16667);
        if (Clock::now() > next)
            ++missed;
        due = std::max(Clock::now(), next); // No burst/catch-up frames after a missed deadline.
    }
    if (cold_files) {
        logs.event("peak_cpu_thumbnails", frames, peak_cpu);
        logs.event("peak_preview_bitmaps", frames, peak_bitmaps);
        logs.event("peak_moving_faces", frames, peak_faces);
        require(peak_cpu <= (16u << 20) && peak_bitmaps <= (32u << 20) && peak_faces <= (8u << 20),
                "Adaptive image caches exceeded their byte budgets");
    }
    logs.event("scroll_missed", frames, missed);
    logs.resource("compact", 1, h, sql, gpu);
}
void core(Host &h, Sql &sql, RawSource &source, Logs &logs, GpuCounters &gpu, bool quick) {
    capture(source, sql,
            source.text()); // Include normal capture initialization in the settled idle baseline.
    Sleep(quick ? 150 : 2000);
    logs.resource("tray", 0, h, sql, gpu);
    const auto t0 = Clock::now();
    auto a = process_tree(h.pid);
    Sleep(quick ? 1500 : 60000);
    const auto b = process_tree(h.pid);
    const double elapsed = ms(t0), cpu_ms = double(b.cpu_ticks - a.cpu_ticks) / 10000.;
    logs.time("idle_wall", 0, elapsed);
    logs.time("idle_cpu_ms", 0, cpu_ms);
    logs.resource("tray", 1, h, sql, gpu);
    logs.event("logical_processors", 0, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    for (int i = 0; i < (quick ? 4 : 25); ++i) {
        auto start = Clock::now();
        h.toggle();
        h.paint();
        require(h.visible(), "Resident expansion failed");
        logs.time("resident_expand", i, ms(start));
        h.toggle();
        require(!h.visible(), "Collapse failed");
    }
    Sleep(quick ? 150 : 1000);
    logs.resource("tray_after_hide", 0, h, sql, gpu);
    scroll_frames(h, sql, logs, gpu, quick);
    for (int i = 0; i < (quick ? 5 : 30); ++i)
        logs.time("text_commit", i, capture(source, sql, source.text()));
}
void finish_private_stress(Host& host, Sql& sql, const std::wstring& original) {
    require(probe::private_station(), "Stress cleanup requires its guarded station");
    const auto bytes = std::as_bytes(std::span(original.c_str(), original.size() + 1));
    const std::span<const unsigned char> expected{reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()};
    const auto digest = sha256(expected);
    // The final return can publish the original Floating card through the resident OLE host.
    // Validate its committed head and actual publisher before shutting that host down.
    require(wait([&] {
        Statement head(sql.db, "SELECT r.standard_id,r.blob_id FROM entries e JOIN representations r ON "
            "r.snapshot_id=e.snapshot_id WHERE e.id=? AND r.status='saved'");
        head.bind(1, sql.head());
        return head.step() && head.integer(0) == CF_UNICODETEXT && head.text(1) == digest && !head.step();
    }), "Final returned head does not match the original synthetic text");
    require(wait([&] {
        DWORD pid{};
        return GetWindowThreadProcessId(GetClipboardOwner(), &pid) && pid == host.pid;
    }), "Final synthetic publication does not belong to the benchmark host");
    host.exit_cleanly();

    require(wait([] { return OpenClipboard(nullptr) != FALSE; }, 1000), "Cannot inspect the flushed private sample");
    struct Close { ~Close() { CloseClipboard(); } } lock;
    require(!GetClipboardOwner(), "A live owner replaced the final private stress sample");
    bool has_formats{}, unicode{};
    UINT format{};
    for (;;) {
        SetLastError(ERROR_SUCCESS);
        format = EnumClipboardFormats(format);
        if (!format) {
            require(!GetLastError(), "Cannot enumerate the final private clipboard");
            break;
        }
        has_formats = true;
        if (format == CF_UNICODETEXT) { unicode = true; continue; }
        if (format == CF_TEXT || format == CF_OEMTEXT || format == CF_LOCALE) continue;
        const auto name = format_name(format);
        require(name == L"DataObject" || name == L"Ole Private Data", "Unexpected format in the final private clipboard");
    }
    if (!has_formats) {
        std::cout << "Verified private stress cleanup formats=0 owner=none (already empty)\n";
        return;
    }
    require(unicode, "The private host did not flush its synthetic text");
    const auto memory = GetClipboardData(CF_UNICODETEXT);
    const auto size = memory ? GlobalSize(memory) : 0;
    require(size >= expected.size() && size <= expected.size() + 16, "Flushed synthetic sample size differs");
    const auto actual = static_cast<const unsigned char*>(GlobalLock(memory));
    require(actual != nullptr, "Cannot read the final synthetic sample");
    const bool matches = std::equal(expected.begin(), expected.end(), actual) &&
        std::all_of(actual + expected.size(), actual + size, [](auto byte) { return byte == 0; });
    GlobalUnlock(memory);
    require(matches && EmptyClipboard(), "Could not retire the exact flushed stress sample");
    SetLastError(ERROR_SUCCESS);
    require(!EnumClipboardFormats(0) && !GetLastError() && !GetClipboardOwner(), "Private station retained the stress sample");
    std::cout << "Verified private stress cleanup sha256=" << digest << " formats=0 owner=none\n";
}

enum class SoakMode { paced, stress, hour };
enum class SoakActions { full, no_scroll, no_transfer, direct_switch };
struct MemoryTrial {
    int rounds{};
    SoakActions actions = SoakActions::full;
    bool observed{};
    fs::path cancel;
};

void pump_until(Clock::time_point due) {
    while (Clock::now() < due) {
        const auto delay = std::chrono::ceil<std::chrono::milliseconds>(due - Clock::now()).count();
        if (delay > 0)
            MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(delay), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        pump();
    }
}

bool observer_checkpoint(DWORD pid, Logs &log, const wchar_t *checkpoint, const char *series, const fs::path &cancel) {
    const auto name = L"Local\\ClipRope.Memory." + std::to_wstring(pid) + L"." + checkpoint;
    HANDLE signal = CreateEventW(nullptr, TRUE, FALSE, name.c_str());
    require(signal != nullptr, "Cannot create memory observer handshake");
    log.event(series, 0, pid);
    const bool acknowledged = wait([&] { return WaitForSingleObject(signal, 0) == WAIT_OBJECT_0 || fs::exists(cancel); }, 60000);
    CloseHandle(signal);
    return acknowledged && !fs::exists(cancel);
}

void soak(Host &h, Sql &sql, RawSource &source, Logs &log, GpuCounters &gpu, bool quick,
          SoakMode mode = SoakMode::paced, const MemoryTrial *memory = nullptr) {
    // Keep one independent card throughout. Every additional detach is paired
    // with a return, and every copy trims only the active rope.
    const auto original = source.issued_text();
    auto permanent = detach(h);
    capture(source, sql, source.text());
    const int n = memory ? memory->rounds : mode == SoakMode::hour ? 1800 :
                  mode == SoakMode::stress ? 900 : quick ? 8 : 180;
    const auto actions = memory ? memory->actions : SoakActions::full;
    const bool tracing = memory && memory->observed;
    auto start = Clock::now();
    auto last_action = start;
    log.resource("soak", 0, h, sql, gpu);
    for (int i = 1; i <= n; ++i) {
        if (tracing) log.phase("select_begin", i);
        select_rope(h, sql, i % 3, actions != SoakActions::direct_switch);
        if (tracing) log.phase("copy_begin", i);
        const double elapsed = capture(source, sql, source.text());
        log.time("soak_capture", i, elapsed);
        if (tracing) log.phase("wheel_begin", i);
        if (actions != SoakActions::no_scroll)
            for (int j = 0; j < 6; ++j)
                h.wheel((j < 3) ? -1 : 1);
        if (tracing) log.phase("transfer_begin", i);
        if (actions == SoakActions::no_transfer)
            send(h.workspace, WM_COMMAND, static_cast<WPARAM>(win::WorkspaceCommand::latest));
        else
            rehang(detach(h, i));
        if (tracing) log.phase("post_action_begin", i);
        if (i == n) last_action = Clock::now();
        require(sql.count("SELECT count(*) FROM entries WHERE state='Floating'") == 1,
                "Fixed population changed");
        if (memory)
            require(sql.count("SELECT count(*)=3 AND min(n)=48 AND max(n)=48 AND sum(rope_id='system:rebin')=0 "
                              "FROM (SELECT rope_id,count(*) n FROM entries WHERE state='Hanging' GROUP BY rope_id)"),
                    "Expected three full ropes with 48 Hanging entries each");
        else
            require(sql.count("SELECT max(n) FROM (SELECT count(*) n FROM entries WHERE state='Hanging' GROUP BY "
                              "rope_id)") <= 48, "Capacity exceeded");
        if (i % (quick ? 2 : 12) == 0) {
            log.resource("soak", i, h, sql, gpu);
            if (memory && memory->observed && fs::exists(memory->cancel)) {
                rehang(permanent);
                finish_private_stress(h, sql, original);
                throw std::runtime_error("Memory observer cancelled the trial");
            }
        }
        if (mode == SoakMode::hour || memory)
            pump_until(start + std::chrono::seconds(2*i));
        else if (!quick && mode == SoakMode::paced)
            spin_until(start + std::chrono::milliseconds(i * 500));
    }
    Sleep(quick ? 200 : 2500);
    log.resource("settled", n, h, sql, gpu);
    if (memory) {
        pump_until(last_action + 60s);
        log.resource("idle_60s", n, h, sql, gpu);
    }
    const bool sampled = !memory || !memory->observed ||
                         observer_checkpoint(h.pid, log, L"idle", "observer_idle_pid", memory->cancel);
    rehang(permanent);
    if ((memory || mode != SoakMode::paced) && h.isolated) finish_private_stress(h, sql, original);
    require(sampled, "Memory observer missed the final idle checkpoint");
    if (memory) require(sql.receipts == static_cast<unsigned>(n) + 32u && !sql.partial_receipts,
                        "Incomplete memory trial copy receipts");
}
void curves(Host &h, Sql &sql, RawSource &source, Logs &log, GpuCounters &gpu, bool quick) {
    std::vector<HWND> floats;
    const int n = quick ? 4 : 24;
    log.resource("floating", 0, h, sql, gpu);
    for (int i = 1; i <= n; ++i) {
        floats.push_back(detach(h, i));
        capture(source, sql, source.text());
        if (i == 1 || i == 4 || i == 12 || i == 24)
            log.resource("floating", i, h, sql, gpu);
    }
    for (auto window : floats)
        rehang(window);
    for (int size : {256, 1024, 2048, 3072}) {
        if (quick && size > 1024)
            break;
        auto image = dib(size, 29);
        auto old = sql.head();
        Peak peak(h.pid);
        auto t = Clock::now();
        source.put(image);
        const bool saved = wait([&] { return sql.head() != old; }, size == 3072 ? 3600 : 5200);
        log.time(saved ? "image_commit" : "image_rejection_window", size, ms(t));
        peak.finish();
        log.event("image_peak_group_private", size, peak.total);
        log.event("image_peak_worker_private", size, peak.worker);
        log.event("image_observed_processes", size, peak.processes);
        log.event("image_saved", size, saved);
        require(saved == (size != 3072), "Image budget outcome mismatch");
        if (saved) {
            Statement bytes(sql.db, "SELECT 1 FROM entries e JOIN representations r ON e.snapshot_id=r.snapshot_id "
                                    "WHERE e.id=? AND r.standard_id=? AND r.status='saved' AND r.blob_id=?");
            bytes.bind(1, sql.head());
            bytes.bind(2, int(CF_DIB));
            bytes.bind(3, sha256(image.bytes));
            require(bytes.step(), "Image curve committed unexpected bytes");
        }
        Sleep(quick ? 60 : 300);
        log.resource("large_image", size, h, sql, gpu);
    }
}
struct DelayObject final : ClipboardObject {
    DWORD delay;
    bool pending = true;
    DelayObject(std::vector<Representation> saved, DWORD d) : ClipboardObject(std::move(saved)), delay(d) {}
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC *f, STGMEDIUM *out) override {
        if (pending && SUCCEEDED(QueryGetData(f))) {
            pending = false;
            Sleep(delay);
        }
        return ClipboardObject::GetData(f, out);
    }
};
std::vector<Representation> office_formats(const fs::path &dir) {
    HistoryStore s(dir);
    const auto entries = s.hanging("main");
    require(!entries.empty(), "Missing synthetic Excel fixture");
    const auto snap = s.load(entries.front().id);
    std::vector<Representation> result;
    for (const auto &f : snap.snapshot.formats)
        if (f.outcome == "saved") {
            const auto id =
                f.standard_id ? f.standard_id
                              : RegisterClipboardFormatW(std::wstring(f.name.begin(), f.name.end()).c_str());
            result.push_back({id, f.bytes, f.index, f.tymed, f.aspect, f.codec_version});
        }
    return result;
}
void save_office(const fs::path &profile, DWORD expected) {
    DWORD actual{};
    GetWindowThreadProcessId(GetClipboardOwner(), &actual);
    require(actual == expected, "Synthetic Excel process does not own clipboard");
    auto capture = capture_clipboard(GetClipboardSequenceNumber(), CaptureMode::all_formats_probe);
    require(capture.saved.size() >= 2, "Excel did not supply independent usable formats");
    HistoryStore store(profile);
    store.admit("main", snapshot(std::move(capture.saved)));
    std::cout << "Synthetic Excel formats materialized for deterministic source-delay runs\n";
}
int delayed(const fs::path &profile, int delay, const std::wstring &key) {
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, (key + L"-ready").c_str());
    HANDLE stop = OpenEventW(SYNCHRONIZE, FALSE, (key + L"-stop").c_str());
    require(ready && stop, "Delayed source control unavailable");
    auto *object = new DelayObject(office_formats(profile), delay);
    require(SUCCEEDED(OleSetClipboard(object)), "Delayed source publication failed");
    SetEvent(ready);
    wait([&] { return WaitForSingleObject(stop, 0) == WAIT_OBJECT_0; }, 20000);
    if (OleIsCurrentClipboard(object) == S_OK)
        OleSetClipboard(nullptr);
    object->Release();
    CloseHandle(ready);
    CloseHandle(stop);
    return 0;
}
void office_curve(const fs::path &self, const fs::path &profile, Host &host, Sql &sql, Logs &log,
                  GpuCounters &gpu, RawSource &source, bool synthetic = false) {
    const auto expected = office_formats(profile);
    for (int delay : {0, 100, 500, 1500, 5000}) {
        auto key =
            L"Local\\ClipRope.Perf." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(delay);
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, (key + L"-ready").c_str());
        HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, (key + L"-stop").c_str());
        auto old = sql.head();
        auto proc =
            probe::launch({self.wstring(), L"--delayed", profile.wstring(), std::to_wstring(delay), key},
                          probe::desktop_path());
        require(proc && ready && stop, "Cannot start delayed Excel material");
        require(wait([&] { return WaitForSingleObject(ready, 0) == WAIT_OBJECT_0; }),
                "Delayed fixture failed");
        Peak peak(host.pid);
        auto t = Clock::now();
        const bool saved = wait([&] { return sql.head() != old; }, delay >= 5000 ? 4200 : 4000);
        log.time(synthetic ? (saved ? "synthetic_commit" : "synthetic_timeout_window") :
                 (saved ? "office_commit" : "office_timeout_window"), delay, ms(t));
        peak.finish();
        log.event(synthetic ? "synthetic_peak_group_private" : "office_peak_group_private", delay, peak.total);
        log.event(synthetic ? "synthetic_peak_worker_private" : "office_peak_worker_private", delay, peak.worker);
        log.event(synthetic ? "synthetic_observed_processes" : "office_observed_processes", delay, peak.processes);
        log.event(synthetic ? "synthetic_saved" : "office_saved", delay, saved);
        SetEvent(stop);
        require(wait([&] { return WaitForSingleObject(proc, 0) == WAIT_OBJECT_0; }, 7500),
                "Delayed producer did not exit");
        CloseHandle(proc);
        CloseHandle(ready);
        CloseHandle(stop);
        require(saved == (delay < 5000), "Delayed result not isolated");
        if (synthetic && saved) {
            for (const auto& format : expected) {
                Statement bytes(sql.db, "SELECT 1 FROM entries e JOIN representations r ON e.snapshot_id=r.snapshot_id "
                                        "WHERE e.id=? AND r.blob_id=? AND r.status='saved'");
                bytes.bind(1, sql.head());
                bytes.bind(2, sha256(format.bytes));
                require(bytes.step(), "Delayed synthetic receipt lost original bytes");
            }
        }
        log.time(synthetic ? "synthetic_followup_commit" : "office_followup_commit", delay, capture(source, sql, source.text()));
        log.resource(synthetic ? "delayed_synthetic" : "delayed_office", delay, host, sql, gpu);
    }
}
void quota(const fs::path &path, Logs &log) {
    std::string survivor;
    {
        HistoryStore store(path);
        store.set_capacity("main", 1);
        survivor = store.admit("main", snapshot(sample::formats(false)));
        auto *db = store.database().handle();
        Statement pages(db, "PRAGMA page_count");
        require(pages.step(), "Missing page count");
        const auto count = pages.integer(0);
        require(!pages.step(), "Unexpected page-count shape");
        exec(db, ("PRAGMA max_page_count=" + std::to_string(count)).c_str());
        domain::Snapshot large;
        for (int i = 0; i < 64; ++i) {
            domain::Format format;
            format.name = "Quota fixture " + std::to_string(i);
            format.bytes.assign(3000, static_cast<BYTE>(i));
            large.formats.push_back(std::move(format));
        }
        const auto start = Clock::now();
        bool full = false;
        try {
            store.admit("main", large);
        } catch (const std::exception &e) {
            full = std::string_view(e.what()).find(sqlite3_errstr(SQLITE_FULL)) != std::string_view::npos;
        }
        require(full, "Expected SQLite page quota to produce SQLITE_FULL");
        log.time("disk_full_failure", 0, ms(start));
    }
    HistoryStore reopened(path);
    const auto after = reopened.hanging("main");
    require(after.size() == 1 && after.front().id == survivor, "Disk-full failure evicted prior tail");
    log.event("disk_full_sqlite_code", 0, SQLITE_FULL);
}
void run(const fs::path &exe, const fs::path &output, const fs::path &office, const fs::path &self,
         bool quick, bool private_qualification = false) {
    Logs logs(output);
    history_fixture::Directory dir;
    GpuCounters gpu;
    quota(dir.path / "quota", logs);
    startup_trials(exe, dir.path, logs, quick ? 3 : 25);
    auto profile = dir.path / "mixed";
    seed(profile);
    Host host(exe, profile);
    Sql sql(profile, host.pid);
    RawSource source;
    core(host, sql, source, logs, gpu, quick);
    soak(host, sql, source, logs, gpu, quick);
    curves(host, sql, source, logs, gpu, quick);
    if (!office.empty())
        office_curve(self, office, host, sql, logs, gpu, source);
    else if (private_qualification) {
        const auto synthetic = dir.path / "delayed-fixture";
        { HistoryStore store(synthetic); store.admit("main", snapshot(sample::formats(false))); }
        office_curve(self, synthetic, host, sql, logs, gpu, source, true);
    }
    require(private_qualification || !office.empty() || quick, "Final qualification requires Excel fixture");
    std::cout << "Performance workload completed.\n";
}
#include "menu_benchmark.inc"
#include "private_menu_benchmark.inc"
#include "history_order.inc"
#include "adjacent_order.inc"
#include "middle_order.inc"
#include "arch_profiles.inc"
#include "floating_population.inc"
#include "footprint_images.inc"
} // namespace
int wmain(int argc, wchar_t **argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(OleInitialize(nullptr)))
        return 1;
    timeBeginPeriod(1);
    int rc = 1;
    try {
        if (argc == 4 && std::wstring_view(argv[1]) == L"--footprint-images") {
            require(probe::private_station(), "Image measurement requires the private station");
            footprint_images(fs::absolute(argv[2]), fs::absolute(argv[3]));
            rc = 0;
        } else if (argc == 4 && std::wstring_view(argv[1]) == L"--floating-population") {
            require(probe::private_station(), "Floating population benchmark requires a private station");
            measure_floating_population(fs::absolute(argv[2]), fs::absolute(argv[3]));
            rc = 0;
        } else if (argc == 3 && std::wstring_view(argv[1]) == L"--middle-order") {
            measure_middle_order(fs::absolute(argv[2]));
            rc = 0;
        } else if (argc == 3 && std::wstring_view(argv[1]) == L"--arch-order-adjacent") {
            measure_adjacent_order(fs::absolute(argv[2]), 25);
            rc = 0;
        } else if (argc == 3 && std::wstring_view(argv[1]) == L"--arch-order") {
            measure_order(fs::absolute(argv[2]), 25);
            rc = 0;
        } else if ((argc == 4 || (argc == 5 && std::wstring_view(argv[4]) == L"--quick")) &&
                   std::wstring_view(argv[1]) == L"--arch-profiles") {
            require(probe::private_station(), "Restore qualification requires the station wrapper");
            measure_profiles(fs::absolute(argv[2]), fs::absolute(argv[3]), argc == 5 ? 2 : 7);
            rc = 0;
        } else if (argc >= 4 && (std::wstring_view(argv[1]) == L"--menus" || std::wstring_view(argv[1]) == L"--private-menus")) {
            bool quick = false;
            std::string only;
            auto language = localization::Language::english;
            for (int i = 4; i < argc; ++i) {
                const std::wstring_view option(argv[i]);
                if (option == L"--quick") quick = true;
                else if (option == L"--entry" && i + 1 < argc) only = fs::path(argv[++i]).string();
                else if (option == L"--language" && i + 1 < argc) {
                    const std::wstring_view code(argv[++i]);
                    if (code == L"en-US") language = localization::Language::english;
                    else if (code == L"zh-CN") language = localization::Language::simplified_chinese;
                    else throw std::invalid_argument("Expected --language en-US or zh-CN");
                } else throw std::invalid_argument("Expected --quick, --entry <menu series> or --language <locale>");
            }
            if (std::wstring_view(argv[1]) == L"--private-menus") {
                require(only.empty(), "The private benchmark measures both Card and Rope property menus");
                private_menu_benchmark(fs::absolute(argv[2]), fs::absolute(argv[3]), quick, language);
            } else benchmark_menus(fs::absolute(argv[2]), fs::absolute(argv[3]), quick, only, language);
            rc = 0;
        } else if (argc >= 4 && std::wstring_view(argv[1]) == L"--save-office") {
            save_office(argv[2], wcstoul(argv[3], nullptr, 10));
            rc = 0;
        } else if (argc == 5 && std::wstring_view(argv[1]) == L"--delayed") {
            rc = delayed(argv[2], _wtoi(argv[3]), argv[4]);
        } else if (argc == 4 && (std::wstring_view(argv[1]) == L"--scroll" ||
                                  std::wstring_view(argv[1]) == L"--file-scroll")) {
            Logs logs(fs::absolute(argv[3]));
            history_fixture::Directory dir;
            const auto profile = dir.path / "mixed";
            if (std::wstring_view(argv[1]) == L"--file-scroll") seed_pictures(profile);
            else seed(profile);
            Host host(fs::absolute(argv[2]), profile);
            Sql sql(profile, host.pid);
            GpuCounters gpu;
            Peak peak(host.pid);
            scroll_frames(host, sql, logs, gpu, false, std::wstring_view(argv[1]) == L"--file-scroll");
            peak.finish();
            logs.event("scroll_peak_private_bytes", 0, peak.total);
            rc = 0;
        } else if ((argc >= 6 && argc <= 7 && std::wstring_view(argv[1]) == L"--memory-soak") || (argc == 4 &&
                   (std::wstring_view(argv[1]) == L"--soak" || std::wstring_view(argv[1]) == L"--stress" ||
                    std::wstring_view(argv[1]) == L"--hour-soak" || std::wstring_view(argv[1]) == L"--soak-probe" ||
                    std::wstring_view(argv[1]) == L"--stress-probe"))) {
            const std::wstring_view mode = argv[1];
            const bool memory_trial = mode == L"--memory-soak";
            SoakMode duration = mode == L"--hour-soak" ? SoakMode::hour :
                                mode.starts_with(L"--stress") ? SoakMode::stress : SoakMode::paced;
            MemoryTrial trial;
            if (memory_trial) {
                const std::wstring_view length(argv[4]), action(argv[5]);
                if (length == L"screen") trial.rounds = 900;
                else if (length == L"hour") trial.rounds = 1800;
                else if (length == L"smoke") trial.rounds = 12;
                else throw std::invalid_argument("Expected smoke, screen or hour");
                if (action == L"no-scroll") trial.actions = SoakActions::no_scroll;
                else if (action == L"no-transfer") trial.actions = SoakActions::no_transfer;
                else if (action == L"direct-switch") trial.actions = SoakActions::direct_switch;
                else require(action == L"full", "Expected full, no-scroll, no-transfer or direct-switch");
                require(argc == 6 || std::wstring_view(argv[6]) == L"--observe", "Expected --observe");
                trial.observed = argc == 7;
                trial.cancel = fs::absolute(argv[3]) / "observer.cancel";
                require(!fs::exists(fs::path(argv[3]) / "resources.csv"), "Use a fresh memory trial directory");
            }
            if (memory_trial || mode == L"--hour-soak")
                require(probe::private_station(), "Memory endurance measurements require the private station");
            Logs logs(fs::absolute(argv[3]));
            history_fixture::Directory dir;
            const auto profile = dir.path / "mixed";
            seed(profile);
            const auto probe_file =
                mode.ends_with(L"-probe") ? fs::absolute(argv[3]) / "probe.log" : fs::path{};
            Host host(fs::absolute(argv[2]), profile, probe_file);
            Sql sql(profile, host.pid);
            RawSource source;
            GpuCounters gpu;
            if (trial.observed)
                require(observer_checkpoint(host.pid, logs, L"ready", "observer_host_pid", trial.cancel),
                        "Memory observer did not start before warm-up");
            if (trial.observed) {
                LARGE_INTEGER frequency{};
                QueryPerformanceFrequency(&frequency);
                logs.event("qpc_frequency", 0, frequency.QuadPart);
                logs.phase("warmup_begin", 0);
            }
            if (mode != L"--soak") {
                capture(source, sql, source.text());
                for (int i = 0; i < 25; ++i) {
                    host.toggle();
                    host.paint();
                    host.toggle();
                }
                scroll_frames(host, sql, logs, gpu, false);
                for (int i = 0; i < 30; ++i)
                    capture(source, sql, source.text());
            } else {
                host.toggle();
                SendMessageW(host.workspace, WM_COMMAND, static_cast<WPARAM>(cliprope::win::WorkspaceCommand::density), 0);
            }
            soak(host, sql, source, logs, gpu, false, duration, memory_trial ? &trial : nullptr);
            rc = 0;
        } else if (argc == 4 && (std::wstring_view(argv[1]) == L"--private-qualify" ||
                                 std::wstring_view(argv[1]) == L"--private-quick")) {
            require(probe::private_station(), "Private qualification requires the station wrapper");
            run(fs::absolute(argv[2]), fs::absolute(argv[3]), {}, fs::absolute(argv[0]),
                std::wstring_view(argv[1]) == L"--private-quick", true);
            rc = 0;
        } else if ((argc == 4 || argc == 5) &&
                   (std::wstring_view(argv[1]) == L"--quick" || std::wstring_view(argv[1]) == L"--qualify")) {
            run(fs::absolute(argv[2]), fs::absolute(argv[3]), argc == 5 ? fs::absolute(argv[4]) : fs::path{},
                fs::absolute(argv[0]), std::wstring_view(argv[1]) == L"--quick");
            rc = 0;
        } else
            std::cerr << "--quick|--qualify <ClipRope.exe> <output> [synthetic Excel profile]\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
    }
    timeEndPeriod(1);
    OleUninitialize();
    return rc;
}
