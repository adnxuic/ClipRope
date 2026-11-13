#include "platform_win/tab_navigation.hpp"
#include <iostream>
#include <stdexcept>

using namespace cliprope::win;
namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void expect(TabNavigation::Route route, WorkspaceShortcut command, bool consume) {
    check(route.command == command && route.consume == consume, "Unexpected Tab route");
}
void run() {
    using S = WorkspaceShortcut;
    TabNavigation keys;
    expect(keys.key(VK_TAB, true, true), S::next_rope, true);
    expect(keys.key(VK_TAB, true, true), S::none, true);
    expect(keys.key(VK_TAB, true, false), S::none, true);
    expect(keys.key(VK_TAB, false, false), S::none, true);
    expect(keys.key(VK_TAB, true, false), S::none, false);
    expect(keys.key(VK_TAB, true, true), S::none, false);
    expect(keys.key(VK_TAB, false, true), S::none, false);
    keys.key(VK_LSHIFT, true, false);
    expect(keys.key(VK_TAB, true, true), S::none, false);
    keys.key(VK_TAB, false, true);
    keys.key(VK_RCONTROL, true, false);
    expect(keys.key(VK_TAB, true, true), S::none, false);
    keys.key(VK_TAB, false, true);
    keys.key(VK_LSHIFT, false, false);
    expect(keys.key(VK_TAB, true, true), S::toggle_overview, true);
    keys.key(VK_TAB, false, true);
    keys.key(VK_RCONTROL, false, false);
    for (const auto modifier : {VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN}) {
        keys.key(modifier, true, false);
        expect(keys.key(VK_TAB, true, true), S::none, false);
        keys.key(modifier, false, false);
        expect(keys.key(VK_TAB, true, true), S::none, false);
        keys.key(VK_TAB, false, true);
    }
    keys.key(VK_TAB, true, true);
    keys.pass_press(); // Failed message delivery must pass the entire press.
    expect(keys.key(VK_TAB, true, true), S::none, false);
    expect(keys.key(VK_TAB, false, true), S::none, false);
    keys.seed(VK_TAB, true); // Started while Tab was already held.
    expect(keys.key(VK_TAB, true, true), S::none, false);
    expect(keys.key(VK_TAB, false, true), S::none, false);
    keys.seed(VK_LCONTROL, true);
    keys.key(VK_RCONTROL, true, false);
    keys.key(VK_LCONTROL, false, false);
    expect(keys.key(VK_TAB, true, true), S::toggle_overview, true);
    keys.key(VK_TAB, false, true);
    keys.key(VK_RCONTROL, false, false);
    expect(keys.key(VK_TAB, true, true), S::next_rope, true);
    keys.key(VK_TAB, false, true);
    keys.seed(VK_LCONTROL,true);
    expect(keys.key('F', true, false), S::none, false);
    expect(keys.key('F', true, true), S::none, false);
    expect(keys.key('F', false, true), S::none, false);
    expect(keys.key('F', true, true), S::find, true);
    expect(keys.key('F', true, false), S::none, true);
    expect(keys.key('F', false, false), S::none, true);
    keys.seed(VK_LCONTROL,false);
    auto custom = default_scoped_keys;
    custom[0] = {0, VK_F6};
    expect(keys.key(VK_F6, true, true, custom), S::next_rope, true);
    custom[0] = {};
    expect(keys.key(VK_F6, true, true, custom), S::none, true);
    expect(keys.key(VK_F6, false, false, custom), S::none, true);
    expect(keys.key(VK_F6, true, true, custom), S::none, false);
    expect(keys.key('V', true, true), S::none, false);
    expect(keys.key('V', false, true), S::none, false);
}
}
int main() {
    try {
        run();
        std::cout << "Tab ownership, repeats, modifiers, startup and delivery failure passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
