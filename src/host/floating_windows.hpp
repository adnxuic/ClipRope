#pragma once
#include "platform_win/floating.hpp"
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>

namespace cliprope::host {
// Retired HWNDs stay owned until the outermost dispatch has unwound.
class FloatingWindows {
public:
    using Map = std::map<domain::Id, std::unique_ptr<win::Floating>, std::less<>>;
    FloatingWindows(std::function<void(HWND)> unbind, std::function<void(const domain::Id&)> retired)
        : unbind_(std::move(unbind)), on_retired_(std::move(retired)) {}
    const Map& active() const { return active_; }
    void clear() { active_.clear(); retired_.clear(); }
    void finish_dispatch() { retired_.clear(); }
    void discard_uncommitted(const domain::Id& id) { active_.erase(id); }

    void open(application::CardSummary card, const domain::Layout& layout, bool show, win::FloatingActions actions) {
        auto [found, inserted] = active_.try_emplace(card.entry.id, std::make_unique<win::Floating>(std::move(actions)));
        if (!inserted) throw std::logic_error("Duplicate Floating window");
        try {
            if (!found->second->open(std::move(card), layout, show)) throw std::runtime_error("Floating window creation failed");
        } catch (...) { active_.erase(found); throw; }
    }
    template<class Commit>
    void detach(application::CardSummary card, const domain::Layout& layout, win::FloatingActions actions, Commit commit) {
        const auto id = card.entry.id;
        open(std::move(card), layout, false, std::move(actions));
        try { commit(); } catch (...) { discard_uncommitted(id); throw; }
        active_.at(id)->show();
    }
    template<class Commit>
    bool rehang(const domain::Id& id, Commit commit) {
        if (!commit()) return false;
        retire(id);
        return true;
    }
    void retire(const domain::Id& id) {
        auto node = active_.extract(id);
        if (node.empty()) return;
        const auto window = node.mapped()->window();
        unbind_(window);
        ShowWindow(window, SW_HIDE);
        retired_.insert(std::move(node));
        on_retired_(id);
    }
private:
    Map active_;
    std::multimap<domain::Id, std::unique_ptr<win::Floating>, std::less<>> retired_;
    std::function<void(HWND)> unbind_;
    std::function<void(const domain::Id&)> on_retired_;
};
}
