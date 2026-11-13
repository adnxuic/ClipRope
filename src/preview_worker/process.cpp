#include "preview_worker/process.hpp"
#include "preview_worker/thumbnail.hpp"
#include "platform_win/media.hpp"
#include <userenv.h>
#include <aclapi.h>
#include <shlobj.h>
#include <algorithm>
#include <mutex>
#include <utility>

namespace cliprope::preview_worker {
#ifdef CLIPROPE_PREVIEW_FAULTS
namespace { thread_local StopProbe* stop_probe{}; }
void observe_stop(StopProbe* probe) { stop_probe = probe; }
#endif
namespace {
void require(bool ok, std::source_location location = std::source_location::current()) {
    if (!ok)
        throw SandboxFailure(GetLastError(), location.line());
}
struct Sid {
    PSID value{};
    std::wstring name = container_name;
    Sid() {
        const auto hr =
            CreateAppContainerProfile(name.c_str(), L"ClipRope screenshot decoder",
                                      L"Isolated screenshot pixel decoding", nullptr, 0, &value);
        if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
            Handle token;
            require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value) != FALSE);
            DWORD size{};
            GetTokenInformation(token.value, TokenAppContainerSid, nullptr, 0, &size);
            std::vector<unsigned char> info(size);
            require(size && GetTokenInformation(token.value, TokenAppContainerSid, info.data(),
                                                size, &size));
            const auto parent =
                reinterpret_cast<TOKEN_APPCONTAINER_INFORMATION *>(info.data())->TokenAppContainer;
            // The test host may itself be contained. Reopening must derive the same restricted
            // child SID.
            require(SUCCEEDED(
                parent ? DeriveRestrictedAppContainerSidFromAppContainerSidAndRestrictedName(
                             parent, name.c_str(), &value)
                       : DeriveAppContainerSidFromAppContainerName(name.c_str(), &value)));
        } else
            require(SUCCEEDED(hr));
    }
    ~Sid() {
        if (value)
            FreeSid(value);
    }
};
void allow_executable(const std::filesystem::path &path, PSID sid) {
    // Grant only this package SID read/execute on this binary; never grant profile access or a
    // broad package ACL.
    PACL old_acl{}, merged{};
    PSECURITY_DESCRIPTOR descriptor{};
    require(GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                  nullptr, &old_acl, nullptr, &descriptor) == ERROR_SUCCESS);
    std::unique_ptr<void, decltype(&LocalFree)> release(descriptor, LocalFree);
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
    access.grfAccessMode = GRANT_ACCESS;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.ptstrName = static_cast<LPWSTR>(sid);
    require(SetEntriesInAclW(1, &access, old_acl, &merged) == ERROR_SUCCESS);
    std::unique_ptr<void, decltype(&LocalFree)> release_acl(merged, LocalFree);
    require(SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                                  DACL_SECURITY_INFORMATION, nullptr, nullptr, merged,
                                  nullptr) == ERROR_SUCCESS);
}
struct Attributes {
    std::vector<unsigned char> bytes;
    LPPROC_THREAD_ATTRIBUTE_LIST list{};
    explicit Attributes(DWORD count = 4) {
        SIZE_T size{};
        InitializeProcThreadAttributeList(nullptr, count, 0, &size);
        bytes.resize(size);
        list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(bytes.data());
        require(InitializeProcThreadAttributeList(list, count, 0, &size) != FALSE);
    }
    ~Attributes() { DeleteProcThreadAttributeList(list); }
    void set(DWORD_PTR key, void *value, size_t size) {
        require(UpdateProcThreadAttribute(list, 0, key, value, size, nullptr, nullptr) != FALSE);
    }
};
std::filesystem::path worker_path() {
    std::wstring text(32768, L'\0');
    auto length = GetModuleFileNameW(nullptr, text.data(), DWORD(text.size()));
    require(length && length < text.size());
    text.resize(length);
    return std::filesystem::path(text).parent_path() / L"ClipRopePreviewWorker.exe";
}
// The rare unconfirmed exit retains only synchronization on the process. The
// child owns its restricted section handles. A callback never references a
// preview service, its resolver or an HWND.
class PendingExit {
    std::mutex mutex_;
    Handle child_;
    PTP_WAIT wait_{};
    void reap() {
        if (child_.value && WaitForSingleObject(child_.value, 0) == WAIT_OBJECT_0) {
            SetThreadpoolWait(wait_, nullptr, nullptr);
            CloseHandle(std::exchange(child_.value, nullptr));
        }
    }
    static void CALLBACK signaled(PTP_CALLBACK_INSTANCE, void* context, PTP_WAIT, TP_WAIT_RESULT) {
        auto& owner = *static_cast<PendingExit*>(context);
        std::lock_guard lock(owner.mutex_);
        owner.reap();
    }
  public:
    PendingExit() {
        wait_ = CreateThreadpoolWait(signaled, this, nullptr);
        require(wait_ != nullptr);
    }
    ~PendingExit() {
        SetThreadpoolWait(wait_, nullptr, nullptr);
        WaitForThreadpoolWaitCallbacks(wait_, TRUE);
        CloseThreadpoolWait(wait_);
    }
    void check() {
        std::lock_guard lock(mutex_);
        reap();
        if (child_.value) throw ShutdownFailure(0, ERROR_BUSY, false);
    }
    void retain(Handle& child) {
        std::lock_guard lock(mutex_);
        child_.value = std::exchange(child.value, nullptr);
        SetThreadpoolWait(wait_, child_.value, nullptr);
    }
#ifdef CLIPROPE_PREVIEW_FAULTS
    bool holding() {
        std::lock_guard lock(mutex_);
        return child_.value != nullptr;
    }
#endif
};
PendingExit& pending_exit() {
    static PendingExit pending;
    return pending;
}

void stop_child(Handle& job, Handle& child, PendingExit& pending, DWORD poll_error) {
    HANDLE terminate = job.value;
#ifdef CLIPROPE_PREVIEW_FAULTS
    Handle query_only;
    if (stop_probe && stop_probe->deny_termination) {
        require(DuplicateHandle(GetCurrentProcess(), job.value, GetCurrentProcess(), &query_only.value,
                                JOB_OBJECT_QUERY, FALSE, 0) != FALSE);
        terminate = query_only.value;
    }
#endif
    const DWORD termination_error = TerminateJobObject(terminate, ERROR_CANCELLED) ? 0 : GetLastError();
#ifdef CLIPROPE_PREVIEW_FAULTS
    if (query_only.value) CloseHandle(std::exchange(query_only.value, nullptr));
#endif
    // Closing our last Job handle applies KILL_ON_JOB_CLOSE even if explicit
    // termination was denied. Only a signaled process proves it actually exited.
    CloseHandle(std::exchange(job.value, nullptr));
    const auto result = WaitForSingleObject(child.value, 100);
    const auto wait_error = result == WAIT_OBJECT_0 ? 0u :
                            result == WAIT_TIMEOUT ? DWORD(ERROR_TIMEOUT) : GetLastError();
    if (result != WAIT_OBJECT_0) pending.retain(child);
    if (termination_error || poll_error || wait_error)
        throw ShutdownFailure(termination_error, poll_error ? poll_error : wait_error, result == WAIT_OBJECT_0);
}
} // namespace
#ifdef CLIPROPE_PREVIEW_FAULTS
bool holding_late_child() { return pending_exit().holding(); }
#endif
domain::ImagePreview decode(const Request &request, std::span<const unsigned char> bytes,
                            std::stop_token stop, application::PreviewDeadline deadline,
                            const std::filesystem::path &executable) {
    if (application::preview_expired(stop, deadline))
        throw std::runtime_error("Preview cancelled");
    const auto source = media::screenshot_header(format(request.encoding), bytes);
    if (!valid(request) || bytes.size() != request.length || !source)
        throw std::runtime_error("Invalid screenshot input");
    static std::mutex decoder;
    std::unique_lock active(decoder, std::try_to_lock);
    if (!active.owns_lock()) throw SandboxFailure(ERROR_BUSY, __LINE__);
    auto& pending = pending_exit();
    pending.check();
    deadline =
        std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(1500));
    static Sid sid;
    const auto path = executable.empty() ? worker_path() : executable;
    const auto attrs = GetFileAttributesW(path.c_str());
    require(attrs != INVALID_FILE_ATTRIBUTES &&
            !(attrs & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)));
    allow_executable(path, sid.value);
    Handle input(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                    static_cast<DWORD>(sizeof(Request) + bytes.size()), nullptr));
    Handle output(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                     static_cast<DWORD>(output_size), nullptr));
    require(input.value && output.value);
    Mapping in(input.value, FILE_MAP_WRITE, sizeof(Request) + bytes.size());
    require(in.data != nullptr);
    std::memcpy(in.data, &request, sizeof request);
    std::memcpy(static_cast<unsigned char *>(in.data) + sizeof request, bytes.data(), bytes.size());
    Handle child_input, child_output;
    require(DuplicateHandle(GetCurrentProcess(), input.value, GetCurrentProcess(),
                            &child_input.value, FILE_MAP_READ, TRUE, 0) != FALSE);
    require(DuplicateHandle(GetCurrentProcess(), output.value, GetCurrentProcess(),
                            &child_output.value, FILE_MAP_WRITE, TRUE, 0) != FALSE);
    HANDLE handles[]{child_input.value, child_output.value};
    Handle job(CreateJobObjectW(nullptr, nullptr));
    require(job.value != nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
                                              JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
                                              JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    limits.ProcessMemoryLimit = 256u * 1024 * 1024;
    require(SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits,
                                    sizeof limits) != FALSE);
    Attributes attributes;
    SECURITY_CAPABILITIES capabilities{};
    capabilities.AppContainerSid = sid.value;
    attributes.set(PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &capabilities, sizeof capabilities);
    attributes.set(PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof handles);
    attributes.set(PROC_THREAD_ATTRIBUTE_JOB_LIST, &job.value, sizeof job.value);
    DWORD policy = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED;
    attributes.set(PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY, &policy, sizeof policy);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    startup.lpAttributeList = attributes.list;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    std::wstring command = L"\"" + path.wstring() + L"\" " +
                           std::to_wstring(reinterpret_cast<UINT_PTR>(child_input.value)) + L" " +
                           std::to_wstring(reinterpret_cast<UINT_PTR>(child_output.value)) + L" " +
                           std::to_wstring(bytes.size());
    wchar_t windows[MAX_PATH]{};
    require(GetWindowsDirectoryW(windows, MAX_PATH) != 0);
    PWSTR local{};
    require(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)));
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> release_local(local, CoTaskMemFree);
    // Windows resolves the AppContainer profile from LOCALAPPDATA during process creation.
    std::wstring environment = L"LOCALAPPDATA=" + std::wstring(local);
    environment.push_back(0);
    environment += L"SystemRoot=" + std::wstring(windows);
    environment.push_back(0);
    environment.push_back(0);
    PROCESS_INFORMATION process{};
    if (stop.stop_requested())
        throw std::runtime_error("Preview cancelled");
    if (std::chrono::steady_clock::now() >= deadline)
        throw SandboxFailure(ERROR_TIMEOUT, __LINE__);
    const bool created =
        CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, TRUE,
                       EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW |
                           CREATE_UNICODE_ENVIRONMENT | BELOW_NORMAL_PRIORITY_CLASS,
                       environment.data(), windows, &startup.StartupInfo, &process) != FALSE;
    require(created);
    Handle child(process.hProcess), thread(process.hThread);
#ifdef CLIPROPE_PREVIEW_FAULTS
    if (stop_probe) {
        require(DuplicateHandle(GetCurrentProcess(), job.value, GetCurrentProcess(), &stop_probe->keep_job.value,
                                JOB_OBJECT_QUERY, FALSE, 0) != FALSE);
        require(DuplicateHandle(GetCurrentProcess(), child.value, GetCurrentProcess(), &stop_probe->child.value,
                                SYNCHRONIZE, FALSE, 0) != FALSE);
        stop_probe->cancel.request_stop();
    }
#endif
    for (;;) {
        auto result = WaitForSingleObject(child.value, 10);
#ifdef CLIPROPE_PREVIEW_FAULTS
        if (stop_probe && stop_probe->fail_wait) { result = WAIT_FAILED; SetLastError(ERROR_INVALID_HANDLE); }
#endif
        if (result == WAIT_OBJECT_0) break;
        const auto error = result == WAIT_FAILED ? GetLastError() : 0u;
        if (result == WAIT_TIMEOUT && !application::preview_expired(stop, deadline)) continue;
        stop_child(job, child, pending, error);
        if (stop.stop_requested()) throw std::runtime_error("Preview cancelled");
        throw SandboxFailure(ERROR_TIMEOUT, __LINE__);
    }
    SecureZeroMemory(in.data, sizeof(Request) + bytes.size());
    if (stop.stop_requested())
        throw std::runtime_error("Preview cancelled");
    if (std::chrono::steady_clock::now() >= deadline)
        throw SandboxFailure(ERROR_TIMEOUT, __LINE__);
    DWORD code{};
    require(GetExitCodeProcess(child.value, &code) != FALSE);
    if (code == 7)
        throw std::runtime_error("Screenshot decode failed");
    if (code != 0)
        throw SandboxFailure(code, __LINE__);
    Mapping out(output.value, FILE_MAP_READ, output_size);
    require(out.data != nullptr);
    Response response{};
    std::memcpy(&response, out.data, sizeof response);
    if (!valid(response, request) || response.source_width != source->width ||
        response.source_height != source->height)
        throw SandboxFailure();
    domain::ImagePreview image{response.width,
                               response.height,
                               std::vector<unsigned char>(response.length),
                               {response.source_width, response.source_height}};
    std::memcpy(image.pbgra.data(), static_cast<const unsigned char *>(out.data) + sizeof response,
                response.length);
    return image;
}
static ThumbnailResult run_job(ThumbnailRequest request, std::span<const std::byte> bytes, std::stop_token stop,
                               const std::function<void(domain::ImageMetadata)>& measured) {
    if (bytes.size() > 32u * 1024 * 1024) throw std::runtime_error("Invalid preview job input");
    request.bytes = static_cast<DWORD>(bytes.size());
    request.host_pid = GetCurrentProcessId();
    request.deadline = GetTickCount64() + 3000;
    if (!valid_thumbnail_request(request)) throw std::runtime_error("Invalid thumbnail request");

    // Keep a late ordinary child separate from the byte-only screenshot lane.
    struct Lane { std::timed_mutex mutex; PendingExit pending; };
    const auto lane = [](bool drag) {
        if (drag) {
            static const auto encoder = std::make_shared<Lane>();
            return encoder;
        }
        static const auto thumbnails = std::make_shared<Lane>();
        return thumbnails;
    }(drag_encoding(request.op));
    std::unique_lock active(lane->mutex, std::defer_lock);
    while (!active.try_lock_for(std::chrono::milliseconds(10)))
        if (stop.stop_requested() || GetTickCount64() >= request.deadline) throw std::runtime_error("Thumbnail cancelled");
    auto& pending = lane->pending;
    pending.check();
    if (stop.stop_requested()) throw std::runtime_error("Thumbnail cancelled");
    const auto output_bytes = thumbnail_output_size(request);
    Handle input(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, DWORD(sizeof request + bytes.size()), nullptr));
    Handle output(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, DWORD(output_bytes), nullptr));
    Handle metadata(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    require(input.value && output.value && metadata.value);
    Mapping in(input.value, FILE_MAP_WRITE, sizeof request + bytes.size()), out(output.value, FILE_MAP_READ, output_bytes);
    require(in.data && out.data);
    std::memcpy(in.data, &request, sizeof request);
    std::memcpy(static_cast<BYTE*>(in.data) + sizeof request, bytes.data(), bytes.size());
    Handle child_input, child_output, child_event;
    const auto inherit = [](HANDLE source, Handle& destination, DWORD rights) {
        require(DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &destination.value, rights, TRUE, 0) != FALSE);
    };
    inherit(input.value, child_input, FILE_MAP_READ);
    inherit(output.value, child_output, FILE_MAP_WRITE);
    inherit(metadata.value, child_event, EVENT_MODIFY_STATE);
    HANDLE handles[]{child_input.value, child_output.value, child_event.value};
    Handle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
                                              JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    limits.ProcessMemoryLimit = 256u * 1024 * 1024;
    require(job.value && SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof limits));
    Attributes attributes(3);
    attributes.set(PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof handles);
    attributes.set(PROC_THREAD_ATTRIBUTE_JOB_LIST, &job.value, sizeof job.value);
    DWORD policy = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED;
    attributes.set(PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY, &policy, sizeof policy);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.lpAttributeList = attributes.list;
    const auto path = worker_path();
    auto command = L"\"" + path.wstring() + L"\" --thumbnail";
    for (const auto h : handles) command += L" " + std::to_wstring(reinterpret_cast<UINT_PTR>(h));
    command += L" " + std::to_wstring(bytes.size());
    PROCESS_INFORMATION process{};
    const auto attributes_of_image = GetFileAttributesW(path.c_str());
    require(attributes_of_image != INVALID_FILE_ATTRIBUTES &&
            !(attributes_of_image & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)));
    if (stop.stop_requested() || GetTickCount64() >= request.deadline) throw std::runtime_error("Thumbnail cancelled");
    require(CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT |
                           CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &startup.StartupInfo, &process) != FALSE);
    Handle child(process.hProcess), thread(process.hThread);
    HANDLE signals[]{child.value, metadata.value};
    const auto* response = static_cast<const ThumbnailResponse*>(out.data);
    for (;;) {
        const auto result = WaitForMultipleObjects(2, signals, FALSE, 10);
        if (result == WAIT_OBJECT_0) break;
        if (result == WAIT_FAILED || stop.stop_requested() || GetTickCount64() >= request.deadline) {
            stop_child(job, child, pending, result == WAIT_FAILED ? GetLastError() : 0);
            throw std::runtime_error("Thumbnail cancelled or expired");
        }
        if (result == WAIT_OBJECT_0 + 1 && measured) {
            const auto source = response->source;
            if (source.width && source.height && source.width <= 32768 && source.height <= 32768 &&
                std::uint64_t(source.width) * source.height <= 40u * 1024 * 1024 && source.orientation <= 31)
                try { measured(source); } catch (...) { stop_child(job, child, pending, 0); throw; }
        }
    }
    DWORD code{};
    require(GetExitCodeProcess(child.value, &code) && !code);
    if (response->signature == 0x32534854 && response->generation == request.generation &&
        response->status == ThumbnailStatus::unsupported_format)
        throw std::runtime_error("Unsupported history database version");
    if (stop.stop_requested() || GetTickCount64() >= request.deadline ||
        response->signature != 0x32534854 || response->generation != request.generation ||
        (response->status != ThumbnailStatus::complete && response->status != ThumbnailStatus::decode_failed) ||
        response->file.availability < domain::FileAvailability::unchecked ||
        response->file.availability > domain::FileAvailability::decode_failed || response->file.directory < -1 ||
        response->file.directory > 1 || response->decodes > 4 || response->live > 1 || response->reuse > 1)
        throw std::runtime_error("Invalid thumbnail response");
    ThumbnailResult result{response->file, {}, response->decodes, response->live != 0, response->reuse != 0};
    if (response->status == ThumbnailStatus::decode_failed) {
        result.file.availability = domain::FileAvailability::decode_failed;
        return result;
    }
    if (drag_encoding(request.op)) {
        if (response->encoded_bytes < 33 || response->encoded_bytes > output_bytes - sizeof *response)
            throw std::runtime_error("Invalid drag PNG length");
        const auto* first = reinterpret_cast<const BYTE*>(response + 1);
        result.encoded.assign(first, first + response->encoded_bytes);
        return result;
    }
    const size_t length = size_t(response->width) * response->height * 4;
    const auto bound = request.op == ThumbnailOp::file_picture &&
        response->file.availability != domain::FileAvailability::available ? 1024u : request.width;
    if (response->width > bound || response->height > bound ||
        (response->width == 0) != (response->height == 0) || length > output_bytes - sizeof *response ||
        response->source.width > 32768 || response->source.height > 32768 || response->source.orientation > 31)
        throw std::runtime_error("Invalid thumbnail dimensions");
    if (length) {
        const auto* first = reinterpret_cast<const BYTE*>(response + 1);
        result.image = {response->width, response->height, {first, first + length}, response->source};
    }
    return result;
}
ThumbnailResult run_thumbnail(ThumbnailRequest request, const std::filesystem::path& profile, std::stop_token stop,
                              const std::function<void(domain::ImageMetadata)>& measured) {
    if (drag_encoding(request.op)) throw std::runtime_error("Invalid file preview operation");
    return run_job(request, std::as_bytes(std::span(profile.native())), stop, measured);
}

std::vector<unsigned char> encode_drag_png(UINT format, std::span<const unsigned char> pixels,
                                           std::uint64_t serial, std::stop_token stop) {
    const auto source = media::screenshot_header(format, pixels);
    if ((format != CF_DIB && format != CF_DIBV5) || !source) throw std::runtime_error("Invalid drag DIB");
    ThumbnailRequest request;
    request.protocol = 2;
    request.op = format == CF_DIB ? ThumbnailOp::drag_dib : ThumbnailOp::drag_dibv5;
    request.generation = serial;
    auto result = run_job(request, std::as_bytes(pixels), stop, {});
    const auto png = media::screenshot_header(RegisterClipboardFormatW(L"PNG"),
                    std::span(result.encoded).first(std::min<size_t>(33, result.encoded.size())));
    if (!png || png->width != source->width || png->height != source->height)
        throw std::runtime_error("Invalid drag PNG dimensions");
    return std::move(result.encoded);
}
} // namespace cliprope::preview_worker
