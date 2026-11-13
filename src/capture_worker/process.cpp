#include "capture_worker/process.hpp"
#include <filesystem>
#include <stdexcept>

namespace cliprope::worker {
namespace {
void require(bool ok) {
    if (!ok) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Capture worker startup");
}
std::wstring object_name(HANDLE object) {
    wchar_t name[256]{};
    DWORD bytes{};
    require(GetUserObjectInformationW(object, UOI_NAME, name, sizeof(name), &bytes) != FALSE);
    return name;
}
std::wstring executable() {
    std::wstring name(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
    require(length && length < name.size());
    name.resize(length);
    return (std::filesystem::path(name).parent_path() / L"ClipRopeWorker.exe").wstring();
}
struct Attributes {
    std::vector<unsigned char> storage;
    LPPROC_THREAD_ATTRIBUTE_LIST list;
    Attributes() {
        SIZE_T bytes{};
        InitializeProcThreadAttributeList(nullptr, 2, 0, &bytes);
        require(bytes != 0);
        storage.resize(bytes);
        list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        require(InitializeProcThreadAttributeList(list, 2, 0, &bytes) != FALSE);
    }
    ~Attributes() { DeleteProcThreadAttributeList(list); }
};
}

Process::Process(std::uint64_t serial, DWORD sequence, ULONGLONG deadline_tick, Wake wake,
                 decltype(&CreateThreadpoolWait) create_wait) : serial_(serial), wake_(std::move(wake)) {
    try {
        job_ = CreateJobObjectW(nullptr, nullptr);
        if (!job_) throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"Cannot create capture job");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
                                                 JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        limits.BasicLimitInformation.ActiveProcessLimit = 1;
        limits.ProcessMemoryLimit = 256ULL * 1024 * 1024;
        require(SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) != FALSE);
        SECURITY_ATTRIBUTES inherited{sizeof(inherited), nullptr, TRUE};
        section_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &inherited, PAGE_READWRITE | SEC_RESERVE,
                                      0, static_cast<DWORD>(section_size()), nullptr);
        require(section_ != nullptr);
        const auto path = executable();
        std::wstring command = L"\"" + path + L"\" " + std::to_wstring(reinterpret_cast<UINT_PTR>(section_)) +
                               L" " + std::to_wstring(serial) + L" " + std::to_wstring(sequence) +
                               L" " + std::to_wstring(deadline_tick);
        auto desktop = object_name(GetProcessWindowStation()) + L"\\" + object_name(GetThreadDesktop(GetCurrentThreadId()));
        Attributes attributes;
        require(UpdateProcThreadAttribute(attributes.list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                          &section_, sizeof(section_), nullptr, nullptr) != FALSE);
        // Attach before the child can execute, including if the parent exits during startup.
        require(UpdateProcThreadAttribute(attributes.list, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                          &job_, sizeof(job_), nullptr, nullptr) != FALSE);
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.lpDesktop = desktop.data();
        startup.lpAttributeList = attributes.list;
        PROCESS_INFORMATION process{};
        require(CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, TRUE,
                               EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr,
                               &startup.StartupInfo, &process) != FALSE);
        process_ = process.hProcess;
        CloseHandle(process.hThread);
        require(SetHandleInformation(section_, HANDLE_FLAG_INHERIT, 0) != FALSE);
        wait_ = create_wait(completed, this, nullptr);
        require(wait_ != nullptr);
        const auto now = GetTickCount64();
        LARGE_INTEGER relative{};
        relative.QuadPart = -static_cast<LONGLONG>(deadline_tick > now ? deadline_tick - now : 0) * 10000;
        FILETIME timeout{relative.LowPart, static_cast<DWORD>(relative.HighPart)};
        SetThreadpoolWait(wait_, process_, &timeout);
    } catch (...) { close(); throw; }
}
Process::~Process() { close(); }
void CALLBACK Process::completed(PTP_CALLBACK_INSTANCE, void* context, PTP_WAIT, TP_WAIT_RESULT result) {
    auto& self = *static_cast<Process*>(context);
    if (result == WAIT_TIMEOUT) {
        self.timed_out_ = true;
        std::lock_guard lock(self.job_mutex_);
        if (WaitForSingleObject(self.process_, 0) == WAIT_TIMEOUT && !TerminateJobObject(self.job_, ERROR_TIMEOUT)) {
            self.termination_error_ = GetLastError();
            // The kill-on-close job has no other owner. Reclaim even if the STA is unavailable.
            CloseHandle(self.job_);
            self.job_ = nullptr;
        }
    }
    if (self.wake_) self.wake_(self.serial_);
}
void Process::close() {
    if (wait_) {
        SetThreadpoolWait(wait_, nullptr, nullptr);
        WaitForThreadpoolWaitCallbacks(wait_, TRUE);
        CloseThreadpoolWait(wait_);
    }
    if (job_) CloseHandle(job_);
    if (process_) CloseHandle(process_);
    if (section_) CloseHandle(section_);
}
bool Process::ready() const { return WaitForSingleObject(process_, 0) != WAIT_TIMEOUT; }
DWORD Process::peak_commit_kib() const {
    std::lock_guard lock(job_mutex_);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    if (!QueryInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits), nullptr)) return 0;
    return static_cast<DWORD>(limits.PeakProcessMemoryUsed / 1024);
}
DWORD Process::termination_error() const {
    std::lock_guard lock(job_mutex_);
    return termination_error_;
}
CaptureResult Process::take(std::uint64_t serial, DWORD sequence, DWORD& exit_code) const {
    if (!GetExitCodeProcess(process_, &exit_code) || exit_code != 0) return {{}, {}, "worker-failed"};
    return read_response(section_, serial, sequence);
}
} // namespace cliprope::worker
