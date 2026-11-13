#pragma once
#include "platform_win/history_clipboard.hpp"
#include <utility>

namespace cliprope::host {
struct CardPress {
    std::shared_ptr<win::EditGesture> target;
    domain::Entry entry;
    bool on_submitted_text{};
    bool paste = true;
    std::optional<DWORD> confirmed_sequence;
};

// Owned by the UI STA. No provider or window callback retains a Desktop pointer.
class RecallSession {
public:
    static constexpr UINT_PTR timer = 4;
    struct Delivery {
        std::shared_ptr<win::EditGesture> target;
        application::PublicationId receipt;
        application::InvocationResult result;
        domain::Id id;
        application::Route route;
    };

    void warm() {
        if (opening_ || pending() || active_ || warmed_window_ == GetForegroundWindow()) return;
        warmed_window_ = GetForegroundWindow();
        warm_.begin_bind(true);
    }
    void remember_editor(HWND owner) {
        if (GetForegroundWindow() == owner) return;
        focus_owner_ = owner;
        auto target = std::make_unique<win::EditGesture>();
        target->begin_bind();
        opening_ = std::move(target);
    }
    bool opening_ready() { return !opening_ || opening_->binding_ready(); }
    void cancel_opening() { if (opening_) opening_->cancel(); }
    void forget_editor() { opening_.reset(); }
    void lost_focus(HWND surface) { if (focus_owner_ == surface) forget_editor(); }

    std::shared_ptr<win::EditGesture> press(HWND surface, std::shared_ptr<win::CardPressInput> input) {
        std::shared_ptr<win::EditGesture> target;
        if (GetForegroundWindow() == focus_owner_ && opening_) {
            target = std::move(opening_);
            target->allow_restore_from(focus_owner_);
        } else {
            forget_editor();
            target = std::make_shared<win::EditGesture>();
            target->begin_bind(false, std::move(input));
            target->allow_restore_from(surface);
        }
        latest_ = target;
        return target;
    }
    bool wait_for_binding(HWND listener, CardPress& press) {
        if (!press.target || press.target->binding_ready()) return false;
        if (!SetTimer(listener, timer, 10, nullptr)) { press.target->cancel(); return false; }
        waiting_ = std::move(press);
        return true;
    }
    std::optional<CardPress> take_bound() {
        if (waiting_ && waiting_->target->binding_ready()) return std::exchange(waiting_, {});
        return {};
    }
    bool active() const { return active_ != nullptr; }
    bool pending() const { return waiting_.has_value() || delivery_.has_value(); }
    class Active {
    public:
        Active(RecallSession& session, win::EditGesture* target) : session_(session), previous_(session.active_) {
            if (target) session_.active_ = target;
        }
        Active(const Active&) = delete;
        ~Active() { session_.active_ = previous_; }
    private:
        RecallSession& session_;
        win::EditGesture* previous_;
    };
    bool wait_for_delivery(HWND listener, Delivery next) {
        if (!SetTimer(listener, timer, 10, nullptr)) { next.target->cancel(); return false; }
        delivery_ = std::move(next);
        return true;
    }
    template<class Report>
    void poll(win::HistoryClipboard& clipboard, Report report) {
        if (!delivery_ || active_) return;
        auto item = std::exchange(delivery_, {});
        Active scope(*this, item->target.get());
        if (auto result = clipboard.poll_delivery(*item->target, item->receipt)) {
            item->result.delivery = *result;
            report(*item);
        } else delivery_ = std::move(item);
    }
    void cancel() {
        if (auto latest = latest_.lock()) latest->cancel();
        if (active_) active_->cancel();
        if (waiting_ && waiting_->target) waiting_->target->cancel();
        if (delivery_) delivery_->target->cancel();
    }
    void stop_opening() { forget_editor(); warm_.cancel(); }
    void discard() { cancel(); waiting_.reset(); delivery_.reset(); }
private:
    HWND focus_owner_{}, warmed_window_{};
    std::unique_ptr<win::EditGesture> opening_;
    win::EditGesture warm_;
    std::weak_ptr<win::EditGesture> latest_;
    win::EditGesture* active_{};
    std::optional<CardPress> waiting_;
    std::optional<Delivery> delivery_;
};
}
