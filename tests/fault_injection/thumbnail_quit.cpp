#include "thumbnail_gate.hpp"
#include "../integration/resident.hpp"
#include "../integration/history_fixture.hpp"
#include "../clipboard_sources/media_sample.hpp"
#include "../performance/process_metrics.hpp"
#include "persistence/history_store.hpp"
#include "platform_win/file_group.hpp"
#include "platform_win/performance_counters.hpp"
#include "platform_win/tray.hpp"
#include "preview_worker/thumbnail.hpp"
#include <dbt.h>
#include <shellapi.h>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <thread>

namespace {
using namespace cliprope;
using thumbnail_fixture::Handle;
using probe::require;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

double ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void seed(const fs::path& profile) {
    const auto picture = profile / "input.png";
    auto png = media_sample::png();
    std::ofstream file(picture, std::ios::binary);
    file.write(reinterpret_cast<const char*>(png.bytes.data()), png.bytes.size());
    file.close();
    require(file.good(), "Synthetic image unavailable");
    persistence::HistoryStore store(profile);
    domain::Snapshot group{{{CF_HDROP, {}, encode_file_group({picture.wstring()})}}};
    store.admit("main", group);
}

DWORD_PTR send(HWND hwnd, UINT msg, WPARAM w = 0, LPARAM l = 0) {
    DWORD_PTR result{};
    require(SendMessageTimeoutW(hwnd, msg, w, l, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 1500, &result) != 0,
            "Desktop heartbeat unavailable");
    return result;
}
void toggle(probe::Resident& host) {
    send(host.listener(), win::Tray::callback, 0, MAKELPARAM(NIN_SELECT, win::Tray::icon_id));
}
void check_protocol() {
    preview_worker::ThumbnailRequest request;
    request.hash.fill('f');
    request.width = 320;
    request.bytes = 32;
    request.host_pid = 1;
    request.generation = 1;
    request.deadline = 3000;
    require(preview_worker::valid_thumbnail_request(request), "Legal ordinary request refused");
    const auto reject = [&](auto change) {
        auto invalid = request;
        change(invalid);
        require(!preview_worker::valid_thumbnail_request(invalid), "Malformed ordinary request accepted");
    };
    reject([](auto& r) { r.hash.front() = '/'; });
    reject([](auto& r) { r.width = 32768; });
    reject([](auto& r) { r.bytes = 65536; });
    reject([](auto& r) { r.generation = 0; });
    reject([](auto& r) { r.op = preview_worker::ThumbnailOp::png; r.explicit_read = 1; });
}
DWORD_PTR metric(probe::Resident& host, win::PerformanceCounter counter) {
    return send(host.listener(), win::performance_query, static_cast<WPARAM>(counter));
}
Handle watchdog() {
    Handle job(CreateJobObjectW(nullptr, nullptr), CloseHandle);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    require(job && SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof limits),
            "Fixture Job unavailable");
    return job;
}
void stopped(probe::Resident& host, HANDLE job) {
    require(WaitForSingleObject(host.handle(), 10000) == WAIT_OBJECT_0, "Desktop failed to exit");
    DWORD status{};
    require(GetExitCodeProcess(host.handle(), &status) && status == 0, "Desktop returned a failure");
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION counts{};
    probe::pump_until([&] {
        require(QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &counts, sizeof counts, nullptr),
                "Child accounting unavailable");
        return counts.ActiveProcesses == 0;
    }, "Child survived desktop exit");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    require(QueryInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits, nullptr),
            "Peak Job memory unavailable");
    std::cout << " child_processes=" << counts.TotalProcesses - 1 << " active_at_exit=" << counts.ActiveProcesses
              << " peak_job_bytes=" << limits.PeakJobMemoryUsed;
}

void normal(const wchar_t* exe, const fs::path& profile, bool warm) {
    Handle job = watchdog();
    probe::Resident host(exe, profile, false);
    require(AssignProcessToJobObject(job.get(), host.handle()), "Could not supervise desktop");
    const auto start = Clock::now();
    toggle(host);
    probe::pump_until([&] { return metric(host, win::PerformanceCounter::thumbnail_bytes) > 0; }, "No ordinary thumbnail");
    auto ready = ms(start);
    const auto decodes = metric(host, win::PerformanceCounter::preview_decodes);
    require(warm ? decodes == 0 : decodes > 0, "Unexpected file decode count");
    const auto resources = performance::process_tree(host.pid());
    const auto quit = Clock::now();
    host.exit();
    std::cout << "normal=" << (warm ? "disk-hit" : "cold") << " ready_ms=" << ready << " decodes=" << decodes
              << " quit_ms=" << ms(quit) << " handles=" << resources.handles;
    stopped(host, job.get());
    std::cout << '\n';
}

enum class End { quit, invalidate, expire };
void blocked(const wchar_t* exe, const wchar_t* stage, End end) {
    history_fixture::Directory dir;
    seed(dir.path);
    Handle job = watchdog();
    require(SetEnvironmentVariableW(thumbnail_fixture::stage_variable, stage), "Cannot select thumbnail gate");
    probe::Resident host(exe, dir.path, false);
    SetEnvironmentVariableW(thumbnail_fixture::stage_variable, nullptr);
    require(AssignProcessToJobObject(job.get(), host.handle()), "Could not supervise desktop");
    const auto create = [&](const wchar_t* suffix) {
        return Handle(CreateEventW(nullptr, TRUE, FALSE, thumbnail_fixture::event_name(host.pid(), suffix).c_str()), CloseHandle);
    };
    Handle entered = create(L".entered"), release = create(L".release");
    require(entered && release, "Cannot create thumbnail gate");
    Handle discarded = create(L".discarded");
    require(discarded != nullptr, "Cannot create cancellation receipt");
    toggle(host);
    require(WaitForSingleObject(entered.get(), 5000) == WAIT_OBJECT_0, "Thumbnail did not enter selected stage");
    const auto beat = Clock::now();
    metric(host, win::PerformanceCounter::painted_frames);
    const auto heartbeat = ms(beat);
    auto sample = performance::process_tree(host.pid());
    std::cout << "gate=";
    for (const auto c : std::wstring_view(stage)) std::cout.put(static_cast<char>(c)); // ASCII fixture names.
    std::cout << " heartbeat_ms=" << heartbeat << " handles=" << sample.handles << " group_private=" << sample.total_private();
    if (end == End::expire) {
        toggle(host);
        const auto timeout = Clock::now();
        probe::pump_until([&] {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION counts{};
            require(QueryInformationJobObject(job.get(), JobObjectBasicAccountingInformation, &counts, sizeof counts, nullptr),
                    "Could not inspect timed-out helper");
            return counts.ActiveProcesses == 1;
        }, "Expired helper survived its deadline");
        require(SetEvent(release.get()), "Cannot reopen the healthy provider");
        toggle(host);
        probe::pump_until([&] { return metric(host, win::PerformanceCounter::thumbnail_bytes) > 0; }, "Healthy request did not recover");
        std::cout << " timeout_and_retry_ms=" << ms(timeout);
        host.exit();
    } else if (end == End::invalidate) {
        toggle(host);
        send(host.listener(), WM_DEVICECHANGE, DBT_DEVNODES_CHANGED);
        require(SetEvent(release.get()), "Cannot resume invalidated job");
        require(WaitForSingleObject(discarded.get(), 5000) == WAIT_OBJECT_0, "Old generation was not discarded");
        require(metric(host, win::PerformanceCounter::thumbnail_bytes) == 0, "Old generation exposed pixels");
        toggle(host);
        probe::pump_until([&] { return metric(host, win::PerformanceCounter::thumbnail_bytes) > 0; }, "New generation did not recover");
        host.exit();
        std::cout << " invalidated_and_recovered=1";
    } else {
        const auto quit = Clock::now();
        require(PostMessageW(host.listener(), WM_CLOSE, 0, 0), "Could not close desktop");
        require(WaitForSingleObject(host.handle(), 2000) == WAIT_OBJECT_0, "Desktop outlived the thumbnail shutdown budget");
        const auto elapsed = ms(quit);
        require(!IsWindow(host.listener()), "Listener survived normal quit");
        std::cout << " last_heartbeat_to_quit_ms="
                  << std::chrono::duration<double, std::milli>(quit - beat).count() - heartbeat
                  << " gate_still_held=1 total_quit_ms=" << elapsed;
    }
    stopped(host, job.get());
    std::cout << '\n';
}

void scroll_workload(const wchar_t* exe, unsigned trial) {
    history_fixture::Directory dir;
    auto png = media_sample::png();
    {
        persistence::HistoryStore store(dir.path);
        for (unsigned i = 0; i < 48; ++i) {
            std::vector<std::wstring> paths;
            for (unsigned part = 0; part < (i % 2 ? 1u : 4u); ++part) {
                const auto file = dir.path / (std::to_string(i) + "-" + std::to_string(part) + ".png");
                std::ofstream stream(file, std::ios::binary);
                stream.write(reinterpret_cast<const char*>(png.bytes.data()), png.bytes.size());
                require(stream.good(), "Could not seed the scroll workload");
                paths.push_back(file.wstring());
            }
            domain::Snapshot content{{{CF_HDROP, {}, encode_file_group(paths)}}};
            store.admit("main", content);
        }
    }
    for (bool warm : {false, true}) {
        Handle job = watchdog();
        probe::Resident host(exe, dir.path, false);
        require(AssignProcessToJobObject(job.get(), host.handle()), "Could not supervise fast scroll");
        auto start = Clock::now();
        toggle(host);
        probe::pump_until([&] { return metric(host, win::PerformanceCounter::thumbnail_bytes) > 0; }, "First file preview missing");
        const auto first = ms(start);
        const auto point = metric(host, win::PerformanceCounter::first_card_point);
        require(point != 0, "No realized card hit rectangle");
        const auto hover = Clock::now();
        send(host.workspace, WM_MOUSEMOVE, 0, static_cast<LPARAM>(point));
        const auto hover_ms = ms(hover);
        std::uint64_t group_peak{};
        std::vector<double> times;
        for (unsigned step = 0; step < 120; ++step) {
            const auto frame = Clock::now();
            send(host.workspace, WM_MOUSEWHEEL, MAKEWPARAM(0, (step / 12) % 2 ? WHEEL_DELTA : -WHEEL_DELTA));
            times.push_back(ms(frame));
            group_peak = std::max(group_peak, performance::process_tree(host.pid()).total_private());
            std::this_thread::sleep_until(frame + std::chrono::microseconds(16667));
        }
        std::sort(times.begin(), times.end());
        std::cout << "file_scroll trial=" << trial << " warm=" << warm << " first_pixels_ms=" << first
                  << " first_hover_dispatch_ms=" << hover_ms
                  << " dispatch_p50_ms=" << times[59] << " dispatch_p95_ms=" << times[113]
                  << " dispatch_max_ms=" << times.back() << " peak_sampled_private=" << group_peak
                  << " decodes=" << metric(host, win::PerformanceCounter::preview_decodes)
                  << " resident_pixels=" << metric(host, win::PerformanceCounter::thumbnail_bytes);
        host.exit();
        stopped(host, job.get());
        std::cout << '\n';
    }
    persistence::HistoryStore reopened(dir.path);
    require(reopened.hanging("main").size() == 48, "Fast scroll changed rope membership");
}
}

int wmain(int argc, wchar_t** argv) {
    if ((argc != 2 && argc != 3) || !probe::private_station()) return 2;
    try {
        check_protocol();
        if (argc == 3 && std::wstring_view(argv[2]) == L"--scroll") {
            for (unsigned trial = 1; trial <= 3; ++trial) scroll_workload(argv[1], trial);
            return 0;
        }
        history_fixture::Directory dir;
        seed(dir.path);
        normal(argv[1], dir.path, false);
        normal(argv[1], dir.path, true);
        {
            persistence::HistoryStore reopened(dir.path);
            const auto entries = reopened.hanging("main");
            require(entries.size() == 1 && reopened.load(entries.front().id).snapshot.path_kind == domain::PathKind::unknown,
                    "Displaying the legacy file persisted its kind");
        }
        for (const auto stage : {L"inspect", L"codec", L"install"}) blocked(argv[1], stage, End::quit);
        blocked(argv[1], L"install", End::invalidate);
        blocked(argv[1], L"inspect", End::expire);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
