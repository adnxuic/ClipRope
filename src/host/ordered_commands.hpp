#pragma once
#include "platform_win/history_clipboard.hpp"
#include <deque>
#include <functional>
#include <optional>
#include <variant>
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace cliprope::host {
// STA-owned. A latest clipboard observation has its own slot alongside the 64 bound commands.
class OrderedCommands {
    using Observation = win::HistoryCapture::Observation;
    struct Command { DWORD sequence; std::function<void()> run; };
    std::deque<std::variant<Command, Observation>> actions_;
    size_t commands_{};
    std::optional<DWORD> sequence_;
    bool draining_{};
public:
    bool empty() const { return actions_.empty(); }
    bool full() const { return commands_ >= 64; }
    size_t size() const { return actions_.size(); }
    std::optional<DWORD> sequence() const { return sequence_; }
    void enqueue(DWORD sequence, std::function<void()> run) {
        if (full()) throw std::runtime_error("Pending history command limit reached");
        actions_.emplace_back(Command{sequence, std::move(run)});
        ++commands_;
    }
    void observe(Observation next) {
        discard_observation();
        actions_.emplace_back(std::move(next));
    }
    void discard_observation() {
        std::erase_if(actions_, [](const auto& action) { return std::holds_alternative<Observation>(action); });
    }
    void cancel() { actions_.clear(); commands_ = 0; }
    template<class Busy, class Observe, class Failure>
    void drain(Busy writing, Observe consume, Failure failed) {
        if (draining_) return;
        draining_ = true;
        struct Reset { bool& active; ~Reset() { active = false; } } reset{draining_};
        while (!empty() && !writing()) {
            auto action = std::move(actions_.front());
            actions_.pop_front();
            if (auto* command = std::get_if<Command>(&action)) {
                --commands_;
                struct Scope {
                    std::optional<DWORD>& slot;
                    std::optional<DWORD> saved;
                    ~Scope() { slot = saved; }
                } scope{sequence_, std::exchange(sequence_, command->sequence)};
                try { command->run(); } catch (const std::exception&) { failed(); }
            } else consume(std::move(std::get<Observation>(action)));
        }
    }
};
} // namespace cliprope::host
