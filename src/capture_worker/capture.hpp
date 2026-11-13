#pragma once

#include "platform_win/clipboard.hpp"
#include "capture_worker/evidence.hpp"
#include "domain/history.hpp"
#include <functional>
#include <future>
#include <memory>

namespace cliprope {

struct FormatObservation {
    UINT format;
    DWORD tymed;
    DWORD aspect;
    LONG index;
    std::string outcome;
};

struct CaptureResult {
    std::vector<Representation> saved;
    std::vector<FormatObservation> formats;
    std::string outcome;
    HRESULT error{S_OK};
    unsigned retries{};
    OomEvidence oom{};
    std::wstring screenshot_path;
    domain::PathKind path_kind = domain::PathKind::unknown;
};

// Synchronous boundary for callers that already own a pumping OLE STA.
enum class CaptureMode { format_probe, local_history, all_formats_probe };
using AcquireClipboard = std::function<HRESULT(IDataObject**)>;
CaptureResult capture_clipboard(DWORD sequence, CaptureMode mode = CaptureMode::format_probe,
                                ULONGLONG deadline_tick = 0, const AcquireClipboard& acquire = OleGetClipboard);
// Consume an acquired OLE offer on its STA. The clipboard identity remains
// authoritative across privacy checks, enumeration and media reads.
CaptureResult capture_offer(IDataObject& source, DWORD sequence, HWND owner, CaptureMode mode,
                            bool& confirmed_contention);

class CaptureSta {
public:
    CaptureSta();
    ~CaptureSta();
    CaptureSta(const CaptureSta&) = delete;
    CaptureSta& operator=(const CaptureSta&) = delete;
    std::future<CaptureResult> submit(DWORD sequence, CaptureMode mode = CaptureMode::format_probe);
private:
    struct Worker;
    std::unique_ptr<Worker> worker_;
};

} // namespace cliprope
