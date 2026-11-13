#pragma once
#include "application/browse.hpp"
#include "persistence/history_store.hpp"
#include <windows.h>

namespace cliprope::win {
class ExternalDragSession {
public:
    ExternalDragSession();
    ~ExternalDragSession();
    // The host binds these at the gesture, before any admission delay.
    std::uint64_t replace();
    std::string_view run(HWND listener, HWND surface, persistence::HistoryStore&, const domain::Entry&,
                         const application::CardSummary&, std::uint64_t serial, DWORD sequence);
    std::string_view finish(persistence::HistoryStore&, std::uint64_t serial);
    bool cancel();
    bool staging() const { return bool(pending_); }
    static constexpr UINT ready_message = WM_APP + 256;
    static constexpr int escape_hotkey = 0x4262;
private:
    struct Pending;
    struct Encoder;
    std::unique_ptr<Pending> pending_;
    std::weak_ptr<Encoder> last_encoder_;
    std::uint64_t serial_{};
    HWND listener_{};
    bool modal_{}, canceled_{};
    void release_input();
    std::string_view deliver(persistence::HistoryStore&);
};
}
