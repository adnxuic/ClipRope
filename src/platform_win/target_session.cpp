#include "platform_win/target_session.hpp"
#include "platform_win/card_press_input.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace cliprope::win {
namespace {
void require(bool ok) { if (!ok) throw std::runtime_error("Target verification unavailable"); }
std::wstring desktop_name(HANDLE object) {
    wchar_t name[256]{};
    DWORD bytes{};
    require(GetUserObjectInformationW(object, UOI_NAME, name, sizeof name, &bytes) != FALSE);
    return name;
}
struct Attributes {
    std::vector<unsigned char> storage;
    LPPROC_THREAD_ATTRIBUTE_LIST list{};
    Attributes() {
        SIZE_T bytes{};
        InitializeProcThreadAttributeList(nullptr, 2, 0, &bytes);
        storage.resize(bytes);
        list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        require(InitializeProcThreadAttributeList(list, 2, 0, &bytes) != FALSE);
    }
    ~Attributes() { DeleteProcThreadAttributeList(list); }
};
DWORD remaining(ULONGLONG deadline) {
    const auto now = GetTickCount64();
    return deadline > now ? DWORD(std::min<ULONGLONG>(deadline - now, target::delivery_ms)) : 0;
}
}
struct TargetSession::State {
    target::Handle job, section, bound, request, reply, cancelled, finished;
    target::Channel* channel{};
    std::wstring desktop;
    target::Channel submitted{};
    std::shared_ptr<CardPressInput> card_input;
    std::optional<bool> press_accepted;
    ULONGLONG verify_start{}, verify_deadline{};
    DWORD payload{}, sequence{};
    bool matches_request() const {
        const auto& c = *channel;
        return c.magic == submitted.magic && c.size == submitted.size && c.serial == submitted.serial &&
               c.foreground == submitted.foreground && c.field == submitted.field && c.pid == submitted.pid &&
               c.tid == submitted.tid && c.input_tick == submitted.input_tick && c.warm == submitted.warm &&
               c.bind_deadline == submitted.bind_deadline && c.guarded_card_press == submitted.guarded_card_press;
    }
    State() {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        job.value = CreateJobObjectW(nullptr, nullptr);
        section.value = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(target::Channel), nullptr);
        bound.value = CreateEventW(&sa, TRUE, FALSE, nullptr);
        request.value = CreateEventW(&sa, TRUE, FALSE, nullptr);
        reply.value = CreateEventW(&sa, TRUE, FALSE, nullptr);
        cancelled.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        finished.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        require(job.value && section.value && bound.value && request.value && reply.value && cancelled.value && finished.value);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        limits.BasicLimitInformation.ActiveProcessLimit = 1;
        require(SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) != FALSE);
        desktop = desktop_name(GetProcessWindowStation()) + L"\\" + desktop_name(GetThreadDesktop(GetCurrentThreadId()));
        channel = static_cast<target::Channel*>(MapViewOfFile(section.value, FILE_MAP_WRITE, 0, 0, sizeof(target::Channel)));
        require(channel != nullptr);
    }
    ~State() { if (channel) UnmapViewOfFile(channel); }
    void supervise() {
        // Serialise start through confirmed exit even when a superseded launch returns late.
        static const auto lane = std::make_shared<std::timed_mutex>();
        auto keep = lane;
        std::unique_lock lock(*keep, std::defer_lock);
        if (!lock.try_lock_for(std::chrono::milliseconds(remaining(submitted.bind_deadline)))) return;
        wchar_t image[32768]{};
        const auto n = GetModuleFileNameW(nullptr, image, DWORD(std::size(image)));
        require(n && n < std::size(image));
        const auto path = std::filesystem::path(image).parent_path() / L"ClipRopeTargetWorker.exe";
        const DWORD attrs = GetFileAttributesW(path.c_str());
        require(attrs != INVALID_FILE_ATTRIBUTES && !(attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)));
        HANDLE handles[]{section.value, bound.value, request.value, reply.value};
        std::wstring command = L"\"" + path.wstring() + L"\"";
        for (auto h : handles) command += L" " + std::to_wstring(reinterpret_cast<UINT_PTR>(h));
        Attributes attributes;
        require(UpdateProcThreadAttribute(attributes.list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                          handles, sizeof(handles), nullptr, nullptr) != FALSE);
        require(UpdateProcThreadAttribute(attributes.list, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                          &job.value, sizeof(HANDLE), nullptr, nullptr) != FALSE);
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.lpDesktop = desktop.data();
        startup.lpAttributeList = attributes.list;
        PROCESS_INFORMATION child{};
        if (!remaining(submitted.bind_deadline) || WaitForSingleObject(cancelled.value, 0) == WAIT_OBJECT_0) return;
        require(CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, TRUE,
                               EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr,
                               &startup.StartupInfo, &child) != FALSE);
        target::Handle process(child.hProcess), thread(child.hThread);
        struct Reap {
            HANDLE job, process;
            ~Reap() {
                TerminateJobObject(job, ERROR_CANCELLED);
                WaitForSingleObject(process, INFINITE); // Pool callback never holds a desktop object.
            }
        } reap{job.value, process.value};
        HANDLE waits[]{bound.value, cancelled.value, process.value};
        if (WaitForMultipleObjects(3, waits, FALSE, remaining(submitted.bind_deadline)) != WAIT_OBJECT_0) return;
        waits[0] = request.value;
        if (WaitForMultipleObjects(3, waits, FALSE, INFINITE) != WAIT_OBJECT_0) return;
        waits[0] = reply.value;
        if (WaitForMultipleObjects(3, waits, FALSE, remaining(channel->verify_deadline)) == WAIT_OBJECT_0) {
            // A normal reply lets the owning apartment release its interfaces and exit.
            WaitForSingleObject(process.value, std::min<DWORD>(100, remaining(channel->verify_deadline)));
        }
    }
    static void CALLBACK launch(PTP_CALLBACK_INSTANCE, void* context) noexcept {
        std::unique_ptr<std::shared_ptr<State>> owner(static_cast<std::shared_ptr<State>*>(context));
        auto& state = **owner;
        try { state.supervise(); } catch (...) {}
        TerminateJobObject(state.job.value, ERROR_CANCELLED);
        SetEvent(state.finished.value);
    }
};
TargetSession::TargetSession(HWND foreground, HWND field, DWORD pid, DWORD tid, bool warm,
                             std::shared_ptr<CardPressInput> card_input) {
    const auto deadline = GetTickCount64() + target::bind_ms;
    LASTINPUTINFO input{sizeof(input)};
    require(GetLastInputInfo(&input) != FALSE);
    // Desktop and CLI each own gestures on their single UI STA.
    static thread_local std::weak_ptr<State> previous;
    if (auto old = previous.lock()) {
        if (warm && WaitForSingleObject(old->finished.value, 0) != WAIT_OBJECT_0)
            throw std::runtime_error("A target gesture is active");
        SetEvent(old->cancelled.value);
    }
    static std::atomic<std::uint64_t> next{};
    state_ = std::make_shared<State>();
    auto& state = *state_;
    *state.channel = {};
    auto& c = *state.channel;
    c.serial = ++next;
    c.foreground = reinterpret_cast<UINT_PTR>(foreground);
    c.field = reinterpret_cast<UINT_PTR>(field);
    c.pid = pid; c.tid = tid; c.warm = warm;
    c.input_tick = input.dwTime;
    state.card_input = std::move(card_input);
    c.guarded_card_press = state.card_input != nullptr;
    c.bind_deadline = deadline;
    state.submitted = c;
    auto owner = std::make_unique<std::shared_ptr<State>>(state_);
    require(TrySubmitThreadpoolCallback(State::launch, owner.get(), nullptr) != FALSE);
    owner.release();
    previous = state_;
}
TargetSession::~TargetSession() { cancel(); }
void TargetSession::cancel() { if (state_) SetEvent(state_->cancelled.value); }
bool TargetSession::expired() const {
    const auto& s = *state_;
    if (WaitForSingleObject(s.cancelled.value, 0) == WAIT_OBJECT_0) return false;
    const auto event = s.verify_deadline ? s.reply.value : s.bound.value;
    const auto deadline = s.verify_deadline ? s.verify_deadline : s.submitted.bind_deadline;
    return GetTickCount64() >= deadline && WaitForSingleObject(event, 0) != WAIT_OBJECT_0;
}
std::optional<target::Kind> TargetSession::binding() const {
    auto& s = *state_;
    const auto& c = *s.channel;
    if (WaitForSingleObject(s.cancelled.value, 0) == WAIT_OBJECT_0) return target::Kind::none;
    if (WaitForSingleObject(s.bound.value, 0) == WAIT_OBJECT_0) {
        if (s.matches_request() && c.bound_serial == s.submitted.serial &&
            c.bound_at >= s.submitted.bind_deadline - target::bind_ms && c.bound_at <= s.submitted.bind_deadline &&
            c.kind <= target::Kind::warm && (c.kind != target::Kind::warm || c.warm)) {
            if (target::guarded_editor(c.kind) && s.submitted.guarded_card_press) {
                if (!s.press_accepted.has_value()) {
                    if (!s.card_input) return target::Kind::none;
                    const bool timed_out = s.card_input->awaiting_hook() && GetTickCount64() >= s.submitted.bind_deadline;
                    const bool accepted = !timed_out && s.card_input->permits(s.submitted.input_tick);
                    // Windows can advance the session input clock before the hook handling
                    // the same mouse-up returns. Only an exact witnessed tick can finish binding.
                    if (!accepted && s.card_input->awaiting_hook() && GetTickCount64() < s.submitted.bind_deadline)
                        return {};
                    s.press_accepted = accepted;
                    s.card_input.reset();
                }
                if (!*s.press_accepted) return target::Kind::none;
            }
            return c.kind;
        }
        return target::Kind::none;
    }
    if (GetTickCount64() >= s.submitted.bind_deadline || WaitForSingleObject(s.finished.value, 0) == WAIT_OBJECT_0)
        return target::Kind::none;
    return {};
}
std::string TargetSession::description() const {
    const auto kind = binding();
    if (!kind || *kind == target::Kind::none) return "unbound";
    const auto& text = state_->channel->description;
    return {text, std::find(text, text + sizeof(text), '\0')};
}
bool TargetSession::verify(PastePayload payload, DWORD sequence, ULONGLONG deadline) {
    auto& s = *state_;
    const auto now = GetTickCount64();
    const auto kind = binding();
    if (!kind || *kind == target::Kind::none || *kind == target::Kind::warm || s.verify_deadline ||
        !sequence || payload > PastePayload::image_files || now >= deadline || deadline - now > target::delivery_ms) return false;
    s.verify_start = now;
    s.verify_deadline = deadline;
    s.channel->payload = s.payload = static_cast<DWORD>(payload);
    s.channel->sequence = s.sequence = sequence;
    s.channel->verify_deadline = deadline;
    return SetEvent(s.request.value) != FALSE;
}
std::optional<target::Verdict> TargetSession::result() const {
    const auto& s = *state_;
    const auto& c = *s.channel;
    if (!s.verify_deadline || GetTickCount64() >= s.verify_deadline ||
        WaitForSingleObject(s.cancelled.value, 0) == WAIT_OBJECT_0) return target::Verdict::lost;
    if (WaitForSingleObject(s.reply.value, 0) == WAIT_OBJECT_0) {
        if (s.matches_request() && c.reply_serial == s.submitted.serial && c.bound_serial == s.submitted.serial &&
            c.payload == s.payload && c.sequence == s.sequence && c.verify_deadline == s.verify_deadline &&
            c.replied_at >= s.verify_start && c.replied_at <= s.verify_deadline &&
            c.verdict <= target::Verdict::replaced) return c.verdict;
        return target::Verdict::lost;
    }
    if (WaitForSingleObject(s.finished.value, 0) == WAIT_OBJECT_0) return target::Verdict::lost;
    return {};
}
} // namespace cliprope::win
