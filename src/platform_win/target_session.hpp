#pragma once
#include "platform_win/target_protocol.hpp"
#include <memory>
#include <optional>

namespace cliprope::win {
class TargetSession {
public:
    TargetSession(HWND foreground, HWND field, DWORD pid, DWORD tid, bool warm = false,
                  std::shared_ptr<CardPressInput> input = {});
    ~TargetSession();
    TargetSession(const TargetSession&) = delete;
    TargetSession& operator=(const TargetSession&) = delete;
    std::optional<target::Kind> binding() const;
    std::string description() const;
    bool verify(PastePayload payload, DWORD sequence, ULONGLONG deadline);
    std::optional<target::Verdict> result() const;
    bool expired() const;
    void cancel();
private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace cliprope::win
