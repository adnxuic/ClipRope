#pragma once
#include <exception>
#include <functional>
#include <memory>
#include <utility>

namespace cliprope::win {
// True queues an owned command for the STA behind earlier history work.
// False leaves it with the caller for immediate execution.
using DeferHistoryCommand = std::function<bool(std::function<void()>)>;

class PendingControlSave {
    std::shared_ptr<int> session_;
  public:
    PendingControlSave() = default;
    PendingControlSave(const PendingControlSave&) = delete;
    PendingControlSave& operator=(const PendingControlSave&) = delete;
    bool pending() const { return bool(session_); }
    void close() { session_.reset(); }

    // A settings write owns its input and survives dismissal. A transient
    // draft belongs to its editor and is withdrawn if that editor is cancelled.
    template<class Write, class Reply>
    bool submit(const DeferHistoryCommand& defer, Write write, Reply reply, bool editor_draft) {
        if (pending()) return true;
        session_ = std::make_shared<int>();
        std::weak_ptr<int> session = session_;
        auto finish = [this, session, reply = std::move(reply)](std::exception_ptr error) {
            if (!session.expired()) {
                session_.reset();
                reply(error);
            }
        };
        auto command = [write = std::move(write), finish, session, editor_draft]() mutable {
            if (editor_draft && session.expired()) return;
            std::exception_ptr error;
            try { write(); } catch (...) { error = std::current_exception(); }
            finish(error);
        };
        try { if (defer && defer(command)) return true; }
        catch (...) { finish(std::current_exception()); return false; }
        command();
        return false;
    }
};
}
