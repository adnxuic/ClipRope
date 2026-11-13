#pragma once
#include <windows.h>
#include "capture_worker/evidence.hpp"
#include <span>
#include <string_view>

namespace cliprope::win {
// Read-only diagnostics for the Release-frame and cache measurements. Values
// contain no entry identities, clipboard bytes or user paths.
inline constexpr UINT performance_query = WM_APP + 244;
inline constexpr UINT gc_probe_start = WM_APP + 254, gc_probe_read = WM_APP + 255;
enum class PerformanceCounter : WPARAM {
    visible_cards, thumbnail_bytes, bitmap_bytes, painted_frames, compositor_fence, render_target_creations,
    clipboard_events, clipboard_sequence, capture_observed_sequence, capture_observation, capture_completed_sequence,
    capture_outcome, capture_worker_exit, capture_hresult, capture_format_count, capture_saved_count,
    capture_retries, capture_unicode, capture_job_peak_kib, capture_reacquisitions, capture_recovery_guard,
    capture_monitor_query, capture_history_query, capture_native_first, capture_native_count, capture_native_error,
    capture_user_objects, capture_gdi_objects, capture_worker_handles, preview_bitmap_bytes, moving_face_bytes, first_card_point, first_rope_point, preview_decodes,
    screenshot_requests, screenshot_decodes, screenshot_cache_bytes, screenshot_failures,
    capture_writing, admission_peak_bytes,
    screenshot_shutdown_failures, screenshot_termination_error, screenshot_wait_error, screenshot_exit_confirmed,
    preview_notices, preview_batches, floating_summary_reads, floating_frames, visible_file_consumers
};

inline constexpr std::string_view capture_states[] = {
    "none", "pending", "duplicate", "version-unavailable", "self", "paused", "excluded-app",
    "excluded-marker", "captured", "partial", "no-usable-format", "superseded", "abandoned",
    "expired", "source-unavailable", "enumeration-failed", "clipboard-busy", "worker-unavailable",
    "worker-failed", "worker-invalid", "history-failed", "privacy-unconfirmed", "snip-pending", "other"
};
inline constexpr std::string_view unicode_states[] = {
    "unexamined", "absent", "saved", "unsupported-format", "unsupported-medium", "duplicate",
    "clipboard-busy", "get-data-failed", "invalid-medium", "size-rejected", "lock-failed",
    "invalid-content", "discarded-superseded", "other"
};
inline DWORD state_code(std::span<const std::string_view> states, std::string_view value) {
    for (DWORD n = 0; n + 1 < states.size(); ++n) if (states[n] == value) return n;
    return static_cast<DWORD>(states.size() - 1);
}
inline std::string_view capture_status_name(DWORD n) { return n < std::size(capture_states) ? capture_states[n] : "other"; }
inline std::string_view unicode_status_name(DWORD n) { return n < std::size(unicode_states) ? unicode_states[n] : "other"; }

struct CaptureHealth {
    DWORD observed_sequence{}, observation{};
    struct Completion {
        DWORD sequence{}, outcome{}, worker_exit{}, hresult{}, format_count{}, saved_count{}, retries{}, unicode{},
              job_peak_kib{};
        OomEvidence oom{};
    } completion;
};
}
