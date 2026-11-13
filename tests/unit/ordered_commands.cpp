#include "host/ordered_commands.hpp"
#include "../integration/history_fixture.hpp"
#include <iostream>

int main() {
    using history_fixture::require;
    using cliprope::host::OrderedCommands;
    try {
        OrderedCommands actions;
        std::vector<DWORD> seen;
        unsigned failures{};
        bool busy{};
        auto observe = [&](auto item) { require(!actions.sequence(), "Observation inherited command sequence"); seen.push_back(item.version); };
        auto drain = [&] { actions.drain([&] { return busy; }, observe, [&] { ++failures; }); };
        actions.observe({7});
        actions.enqueue(11, [&] {
            require(actions.sequence() == 11, "Command lost the bound sequence");
            seen.push_back(11); drain(); busy = true;
        });
        actions.observe({13});
        actions.enqueue(17, [&] { throw std::runtime_error("command fixture"); });
        actions.enqueue(19, [&] { seen.push_back(*actions.sequence()); });
        drain();
        require(seen == std::vector<DWORD>{11} && actions.size() == 3 && !actions.sequence(),
                "Reentrant drain executed past the admission barrier");
        busy = false;
        drain();
        require(seen == std::vector<DWORD>{11,13,19} && failures == 1 && actions.empty() && !actions.sequence(),
                "Deferred command order or failure recovery changed");
        actions.observe({23});
        for (unsigned i = 0; i < 64; ++i) actions.enqueue(i+1, [] {});
        history_fixture::rejects([&] { actions.enqueue(65,[] {}); }, "Accepted a 65th command");
        actions.observe({29});
        require(actions.full() && actions.size()==65, "Latest observation lost its reserved slot");
        actions.discard_observation();
        require(actions.size()==64, "Quit discarded a bound command");
        actions.cancel();
        require(actions.empty() && !actions.full(), "End session retained pending work");
        std::cout << "Coalescing, 64 commands plus observation, sequence, reentrancy and close passed\n";
        return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
