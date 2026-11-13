#include "process_metrics.hpp"
#include <algorithm>
#include <map>
#include <pdhmsg.h>
#include <psapi.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tlhelp32.h>

namespace performance {
namespace {
struct Handle {
    HANDLE h{};
    ~Handle() {
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
};
std::uint64_t ticks(FILETIME t) { return (std::uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; }
std::vector<PROCESSENTRY32W> processes() {
    Handle snap{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    std::vector<PROCESSENTRY32W> out;
    PROCESSENTRY32W item{sizeof(item)};
    if (Process32FirstW(snap.h, &item))
        do {
            out.push_back(item);
        } while (Process32NextW(snap.h, &item));
    return out;
}
std::map<std::wstring, std::int64_t> values(PDH_HCOUNTER counter) {
    DWORD length{}, count{};
    if (!counter ||
        PdhGetFormattedCounterArrayW(counter, PDH_FMT_LARGE, &length, &count, nullptr) != PDH_MORE_DATA)
        return {};
    std::vector<std::uint64_t> data((length + 7) / 8);
    auto *items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(data.data());
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_LARGE, &length, &count, items) != ERROR_SUCCESS)
        return {};
    std::map<std::wstring, std::int64_t> result;
    for (DWORD i = 0; i < count; ++i)
        if (items[i].szName && (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA ||
                                items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA))
            result[items[i].szName] += items[i].FmtValue.largeValue;
    return result;
}
} // namespace
Sample process_tree(DWORD root) {
    auto table = processes();
    Sample totals;
    totals.ids.push_back(root);
    for (std::size_t at = 0; at < totals.ids.size(); ++at)
        for (const auto &p : table)
            if ((!_wcsicmp(p.szExeFile, L"ClipRopeWorker.exe") || !_wcsicmp(p.szExeFile, L"ClipRopePreviewWorker.exe") ||
                 !_wcsicmp(p.szExeFile, L"ClipRopeTargetWorker.exe")) &&
                p.th32ParentProcessID == totals.ids[at] &&
                std::find(totals.ids.begin(), totals.ids.end(), p.th32ProcessID) == totals.ids.end())
                totals.ids.push_back(p.th32ProcessID);
    for (auto id : totals.ids) {
        Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, id)};
        if (!process.h)
            continue;
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        if (!GetProcessMemoryInfo(process.h, reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory),
                                  sizeof(memory)))
            continue;
        ++totals.processes;
        totals.working_set += memory.WorkingSetSize;
        if (id == root) {
            totals.host_private = memory.PrivateUsage;
            totals.host_working_set = memory.WorkingSetSize;
        } else {
            totals.worker_private += memory.PrivateUsage;
            totals.worker_working_set += memory.WorkingSetSize;
            totals.worker_peak_commit = std::max<std::uint64_t>(totals.worker_peak_commit, memory.PeakPagefileUsage);
        }
        DWORD handles{};
        GetProcessHandleCount(process.h, &handles);
        totals.handles += handles;
        totals.gdi += GetGuiResources(process.h, GR_GDIOBJECTS);
        totals.user += GetGuiResources(process.h, GR_USEROBJECTS);
        auto entry =
            std::find_if(table.begin(), table.end(), [=](const auto &p) { return p.th32ProcessID == id; });
        if (entry != table.end())
            totals.threads += entry->cntThreads;
        FILETIME creation{}, exit{}, kernel{}, user{};
        if (GetProcessTimes(process.h, &creation, &exit, &kernel, &user))
            totals.cpu_ticks += ticks(kernel) + ticks(user);
    }
    if (!totals.host_private)
        throw std::runtime_error("Cannot sample test host");
    return totals;
}
GpuCounters::GpuCounters() {
    DWORD session{};
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    for (auto &p : processes())
        if (!_wcsicmp(p.szExeFile, L"dwm.exe")) {
            DWORD other{};
            if (ProcessIdToSessionId(p.th32ProcessID, &other) && other == session)
                dwm_ = p.th32ProcessID;
        }
    if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS)
        return;
    PdhAddEnglishCounterW(query_, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &dedicated_);
    PdhAddEnglishCounterW(query_, L"\\GPU Process Memory(*)\\Shared Usage", 0, &shared_);
    PdhAddEnglishCounterW(query_, L"\\Process(dwm*)\\ID Process", 0, &dwm_process_);
    PdhAddEnglishCounterW(query_, L"\\Process(dwm*)\\Private Bytes", 0, &dwm_bytes_);
}
GpuCounters::~GpuCounters() {
    if (query_)
        PdhCloseQuery(query_);
}
GpuUsage GpuCounters::read(const std::vector<DWORD> &ids) {
    GpuUsage result;
    if (!query_ || PdhCollectQueryData(query_) != ERROR_SUCCESS)
        return result;
    const auto pids = values(dwm_process_), bytes = values(dwm_bytes_);
    for (const auto &[instance, id] : pids) {
        if (id != dwm_ && (dwm_ || pids.size() != 1))
            continue;
        dwm_ = static_cast<DWORD>(id);
        if (auto it = bytes.find(instance); it != bytes.end())
            result.dwm_private = it->second;
    }
    auto sum = [&](PDH_HCOUNTER counter, std::int64_t &group, std::int64_t &dwm) {
        const auto instances = values(counter);
        if (instances.empty())
            return;
        group = 0;
        for (const auto &[instance, usage] : instances) {
            if (!instance.starts_with(L"pid_"))
                continue;
            const DWORD id = wcstoul(instance.c_str() + 4, nullptr, 10);
            if (id == dwm_)
                dwm = std::max<std::int64_t>(0, dwm) + usage;
            if (std::find(ids.begin(), ids.end(), id) != ids.end())
                group += usage;
        }
    };
    sum(dedicated_, result.dedicated, result.dwm_dedicated);
    sum(shared_, result.shared, result.dwm_shared);
    return result;
}
} // namespace performance
