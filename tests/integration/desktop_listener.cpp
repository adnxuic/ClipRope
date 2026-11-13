#include "resident.hpp"
#include "history_fixture.hpp"
#include "../clipboard_sources/external_copy.hpp"
#include "../clipboard_sources/sample.hpp"
#include "persistence/history_store.hpp"
#include "capture_worker/capture.hpp"

namespace {
using namespace cliprope;
using probe::require;
using Store = persistence::HistoryStore;

using probe::Resident;
using probe::owned_window;

void floating_restart(const wchar_t* host) {
    history_fixture::Directory temporary;
    std::string shown, hidden, snapshot;
    const domain::Layout offscreen{"Removed synthetic monitor", -90000, 90000, 240, 160};
    {
        Store seed(temporary.path);
        auto rope = seed.active_rope();
        domain::Snapshot sample;
        domain::Format text;
        text.standard_id = CF_UNICODETEXT;
        text.bytes = {76, 0, 0, 0};
        sample.formats.push_back(text);
        shown = seed.admit(rope, sample);
        snapshot = seed.entry(shown).snapshot_id;
        hidden = seed.copy(shown, rope);
        history_fixture::rejects([&]{ seed.save_layout(shown, offscreen); }, "Hanging layout update accepted");
        seed.detach(shown, offscreen);
        auto layout = offscreen;
        layout.visible = false;
        seed.detach(hidden, layout);
    }
    {
        Resident resident(host, temporary.path, false);
        HWND window{};
        unsigned total{}, visible{};
        while ((window = FindWindowExW(nullptr, window, L"ClipRope.Floating", nullptr)) != nullptr) {
            DWORD owner{};
            GetWindowThreadProcessId(window, &owner);
            if (owner != resident.pid()) continue;
            ++total;
            visible += (GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) != 0;
            require((GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0, "Floating restored as taskbar window");
            RECT rect{};
            MONITORINFO monitor{sizeof(monitor)};
            require(GetWindowRect(window, &rect) && GetMonitorInfoW(MonitorFromRect(&rect, MONITOR_DEFAULTTONULL), &monitor) &&
                    rect.left >= monitor.rcWork.left && rect.top >= monitor.rcWork.top &&
                    rect.right <= monitor.rcWork.right && rect.bottom <= monitor.rcWork.bottom,
                    "Removed monitor left a Floating window out of reach");
        }
        require(total == 1 && visible == 1, "Startup materialized a hidden or omitted a visible Floating card");
        resident.exit();
    }
    {
        bool fail{};
        Store reopened(temporary.path, [&](auto point) {
            if (fail && point == "transaction") throw std::runtime_error("Synthetic move failure");
        });
        const auto layout = reopened.layout(shown);
        require(layout.visible && !reopened.layout(hidden).visible && layout.monitor != offscreen.monitor &&
                reopened.entry(shown).snapshot_id == snapshot && !reopened.entry(shown).rope_id, "Restored layout or identity lost");
        auto moved = layout;
        moved.x += 40;
        fail = true;
        history_fixture::rejects([&]{ reopened.save_layout(shown, moved); }, "Move fault did not roll back");
        fail = false;
        require(reopened.layout(shown).x == layout.x, "Failed move changed durable placement");
        reopened.save_layout(shown, moved);
        reopened.rehang(shown, reopened.active_rope());
        history_fixture::rejects([&]{ reopened.save_layout(shown, layout); }, "Late move changed a Hanging card");
        reopened.erase(hidden);
        history_fixture::rejects([&]{ reopened.save_layout(hidden, layout); }, "Late move resurrected a deleted card");
    }
    std::cout << "independent HWNDs=2 shown=1 hidden=1; removed monitor rescued and persisted; "
                 "move-fault=rolled-back; stale-layout-after-return/delete=rejected\n";
}

void test(const wchar_t* host, const wchar_t* source) {
    history_fixture::Directory directory;
    std::string main, collector;
    {
        Store seed(directory.path);
        main = seed.active_rope();
        collector = seed.create_rope("Fixture collector", 3);
        seed.set_active_rope(collector);
    }
    probe::ExternalCopy older(source, L"second");
    std::string entry_id, snapshot;
    {
        Resident hidden(host, directory.path, false);
        probe::ExternalCopy eligible(source, L"busy-once");
        sqlite3* raw{};
        const auto path = (directory.path / L"history.db").u8string();
        require(sqlite3_open_v2(reinterpret_cast<const char*>(path.c_str()), &raw, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK,
                "Could not observe the resident write through a read-only SQLite connection");
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> read(raw, sqlite3_close);
        sqlite3_busy_timeout(raw, 500);
        probe::pump_until([&] {
            persistence::Statement rows(raw, "SELECT id,snapshot_id FROM entries WHERE rope_id=?");
            rows.bind(1, collector);
            if (!rows.step()) return false;
            entry_id = rows.text(0);
            snapshot = rows.text(1);
            require(!rows.step(), "Hidden listener admitted the earlier copy");
            return true;
        }, "Hidden listener did not save copy");
        require(!(GetWindowLongPtrW(hidden.workspace, GWL_STYLE) & WS_VISIBLE), "Hidden listener showed the workspace on capture");
        hidden.exit();
    }
    {
        probe::ExternalCopy current(source, L"normal");
        CaptureSta reader;
        std::future<CaptureResult> job;
        HWND locker = CreateWindowExW(0, L"STATIC", L"Fixture clipboard reader", WS_POPUP,
                                      0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(locker && OpenClipboard(locker), "Could not simulate a concurrent clipboard reader");
        {
            struct Unlock { ~Unlock() { CloseClipboard(); } } lock;
            job = reader.submit(GetClipboardSequenceNumber(), CaptureMode::local_history);
            if (job.wait_for(std::chrono::milliseconds(48)) != std::future_status::timeout) {
                auto early = job.get();
                std::cerr << "Unexpected early read outcome=" << early.outcome << " retries=" << early.retries
                          << " hr=" << early.error << " descriptors=" << early.formats.size() << '\n';
                throw std::runtime_error("Busy clipboard was treated as an empty inventory");
            }
        }
        DestroyWindow(locker);
        probe::pump_until([&] { return job.wait_for(std::chrono::seconds(0)) == std::future_status::ready; },
                          "Read retry did not finish");
        auto result = job.get();
        require(result.retries > 0 && result.saved == sample::formats(false), "Stable-owner retry did not recover both formats");
        std::cout << "concurrent-reader retries=" << result.retries << " restored=Unicode+HTML\n";
    }
    {
        probe::ExternalCopy between_sessions(source, L"second");
        const auto sequence = GetClipboardSequenceNumber();
        Resident visible(host, directory.path, true);
        require(sequence == GetClipboardSequenceNumber(), "Startup overwrote an external copy with the saved head");
        visible.exit();
        Store reader(directory.path);
        auto entries = reader.hanging(collector);
        require(reader.active_rope() == collector && reader.hanging(main).empty() && entries.size() == 1 &&
                entries.front().id == entry_id && entries.front().snapshot_id == snapshot, "Restart changed saved identity");
        auto restored = reader.load(entry_id);
        auto expected = sample::formats(false);
        bool unicode{}, html{};
        for (const auto& format : restored.snapshot.formats) {
            if (format.outcome != "saved") continue;
            unicode |= format.standard_id == CF_UNICODETEXT && format.bytes == expected.front().bytes;
            html |= format.name == "HTML Format" && format.bytes == expected.back().bytes;
        }
        require(unicode && html, "Restart changed saved formats");
    }
    std::cout << "hidden=one capture; collector=" << collector << " entry=" << entry_id
              << " snapshot=" << snapshot << " restart=visible same-IDs Unicode+HTML\n";
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3 || !probe::private_station() || FAILED(OleInitialize(nullptr))) return 2;
    int result = 0;
    try {
        if (std::wstring_view(argv[2]) == L"--floating") floating_restart(argv[1]);
        else test(argv[1], argv[2]);
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    OleUninitialize();
    return result;
}
