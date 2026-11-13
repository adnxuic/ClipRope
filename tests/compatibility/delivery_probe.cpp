#include "desktop_fixture.hpp"
#include "../clipboard_sources/sample.hpp"
#include "../integration/clipboard_process.hpp"
#include "platform_win/clipboard.hpp"
#include "platform_win/delivery.hpp"
#include "platform_win/overlay.hpp"

#include <ole2.h>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using cliprope::win::DeliveryResult;
using fixture::Action;
using fixture::Query;
using namespace std::chrono_literals;

using namespace desktop_fixture;

struct Case {
    const char* name;
    Action fault;
    DeliveryResult result;
    bool edit;
};

UINT accepted{}, calls{}, key_ups{};
UINT WINAPI simulate_input(UINT count, INPUT* keys, int) {
    if (++calls == 1) {
        require(count == 4 && keys[0].ki.wVk == VK_LCONTROL && keys[1].ki.wVk == 'V' &&
                keys[2].ki.dwFlags == KEYEVENTF_KEYUP && keys[3].ki.dwFlags == KEYEVENTF_KEYUP,
                "Invalid paste chord");
        return accepted;
    }
    for (UINT i = 0; i < count; ++i) {
        require(keys[i].ki.dwFlags == KEYEVENTF_KEYUP, "Partial paste replayed a key down");
        ++key_ups;
    }
    return count;
}

int run_unselected() {
    for (accepted = 0; accepted <= 4; ++accepted) {
        calls = key_ups = 0;
        const auto result = cliprope::win::send_paste_shortcut(simulate_input);
        const auto expected = accepted == 0 ? DeliveryResult::send_failed :
                              accepted == 4 ? DeliveryResult::shortcut_sent : DeliveryResult::unconfirmed;
        const UINT expected_ups[]{0, 1, 2, 1, 0};
        require(result == expected && key_ups == expected_ups[accepted] &&
                calls == (key_ups ? 2u : 1u), "Incorrect partial SendInput handling");
    }
    require(probe::private_station(), "isolated mode requires the test window station");
    Fields fields;
    fields.start();
    fields.act(Action::reset);
    fields.act(Action::focus_button);
    cliprope::win::EditGesture gesture;
    gesture.bind();
    cliprope::ClipboardPublisher publisher;
    struct ClearOwnPublication {
        cliprope::ClipboardPublisher& publisher;
        ~ClearOwnPublication() {
            if (publisher.is_current()) OleSetClipboard(nullptr);
        }
    } clear{publisher};
    require(SUCCEEDED(publisher.publish(sample::formats(false))) && publisher.is_current(), "private publication failed");
    require(gesture.deliver([] { return true; }) == DeliveryResult::no_field, "unexpected bound edit");
    require(gesture.deliver([] { return true; }) == DeliveryResult::consumed, "binding not consumed");
    require(!fields.get(Query::changes_a) && !fields.get(Query::changes_b) && !fields.get(Query::edit_attempts),
            "unexpected edit without a target");
    fields.act(Action::focus_b);
    require(PostMessageW(fields.window, fixture::command, static_cast<WPARAM>(Action::paste_b), 0) != 0,
            "could not dispatch fixture paste");
    require(until([&] { return fields.get(Query::b_pasted) != 0; }), "fixture paste did not receive published text");
    require(fields.get(Query::changes_b) == 1, "fixture paste edit count mismatch");
    require(publisher.is_current() && SUCCEEDED(OleSetClipboard(nullptr)), "could not clear private sample");
    std::cout << "no editable selection; published Unicode/HTML; zero automatic edits; one subsequent fixture paste\n";
    return 0;
}

int run_desktop() {
    struct CursorRestore {
        POINT point{};
        bool restore{};
        ~CursorRestore() { if (restore) SetCursorPos(point.x, point.y); }
    } cursor;
    cursor.restore = GetCursorPos(&cursor.point) != 0;
    DWORD expected_sequence = GetClipboardSequenceNumber();
    Fields fields;
    fields.start();
    cliprope::ClipboardPublisher publisher;
    const auto formats = sample::formats(false);
    cliprope::win::EditGesture binding;
    Case active{};
    DeliveryResult result{};
    unsigned presses{}, gestures{};
    HRESULT publication{};
    bool retry_blocked{};
    std::exception_ptr callback_error;
    cliprope::win::Overlay overlay({
        [&] {
            ++presses;
            binding.bind();
        },
        [&] {
            ++gestures;
            try {
                require(GetForegroundWindow() == fields.window, "external foreground changed");
                require(GetClipboardSequenceNumber() == expected_sequence, "unexpected clipboard update before publication");
                publication = publisher.publish(formats);
                if (FAILED(publication)) return;
                if (active.fault != Action::reset && active.fault != Action::focus_button && active.fault != Action::caret)
                    fields.act(active.fault);
                result = binding.deliver([&] { return publisher.is_current(); });
                retry_blocked = binding.deliver([&] { return publisher.is_current(); }) == DeliveryResult::consumed;
            } catch (...) { callback_error = std::current_exception(); }
        }
    });
    require(overlay.open({345, 36}), "overlay creation failed");
    const Case cases[] = {
        {"selection", Action::reset, DeliveryResult::shortcut_sent, true},
        {"caret", Action::caret, DeliveryResult::shortcut_sent, true},
        {"manual-paste", Action::focus_button, DeliveryResult::no_field, false},
        {"changed-focus", Action::focus_b, DeliveryResult::lost_target, false},
        {"destroyed", Action::destroy_a, DeliveryResult::lost_target, false},
        {"read-only", Action::readonly_a, DeliveryResult::lost_target, false},
        {"access-denied", Action::deny_query, DeliveryResult::access_denied, false},
        {"overwritten", Action::overwrite, DeliveryResult::replaced_publication, false},
        {"edited-without-ack", Action::suppress_after, DeliveryResult::shortcut_sent, true},
        {"no-edit-without-ack", Action::suppress_without, DeliveryResult::shortcut_sent, false}
    };
    for (const auto& c : cases) {
        std::cout << "case=" << c.name << std::flush;
        active = c;
        reset_fields(fields);
        if (c.fault == Action::focus_button || c.fault == Action::caret) fields.act(c.fault);
        presses = gestures = 0;
        publication = E_PENDING;
        retry_blocked = false;
        // An observer can hold the clipboard open while asking our OLE STA
        // to render. Keep dispatching during the inter-gesture interval.
        const auto next_click = GetTickCount64() + GetDoubleClickTime() + 20;
        require(until([&] { return GetTickCount64() >= next_click; }), "inter-gesture wait timed out");
        input_click(overlay.window(), overlay.card_screen_rect(), fields.window);
        require(until([&] { return gestures != 0; }), "card double-click not received");
        if (callback_error) std::rethrow_exception(callback_error);
        if (presses != 1 || gestures != 1 || FAILED(publication) || !retry_blocked) {
            std::cerr << " presses=" << presses << " gestures=" << gestures << " publication_hr=0x"
                      << std::hex << publication << std::dec << " retry_blocked=" << retry_blocked;
            throw std::runtime_error("binding or publication count mismatch");
        }
        require(result == c.result, "unexpected delivery result");
        const bool attempted = c.edit || c.fault == Action::suppress_without;
        require(until([&] { return fields.get(Query::edit_attempts) == static_cast<unsigned>(attempted); }), "mutation attempt count mismatch");
        require(until([&] { return fields.get(Query::changes_a) == static_cast<unsigned>(c.edit); }) && !fields.get(Query::changes_b),
                "unexpected target edit count");
        require(fields.get(Query::b_unchanged) != 0, "other field changed");
        if (c.fault != Action::destroy_a) {
            const auto expected = c.fault == Action::caret ? Query::a_inserted :
                                  c.edit ? Query::a_replaced : Query::a_unchanged;
            require(fields.get(expected) != 0, "target text mismatch");
        }
        if (c.fault == Action::focus_button || c.fault == Action::overwrite) {
            fields.act(Action::focus_b);
            input_paste(fields.window);
            const auto expected = c.fault == Action::overwrite ? Query::b_pasted_new : Query::b_pasted;
            if (!until([&] { return fields.get(expected) != 0; })) {
                DWORD owner{};
                GetWindowThreadProcessId(GetClipboardOwner(), &owner);
                std::cerr << " manual_edits_B=" << fields.get(Query::changes_b)
                          << " fixture_owner=" << (owner == fields.pid)
                          << " fixture_foreground=" << (GetForegroundWindow() == fields.window);
                throw std::runtime_error("manual paste did not receive expected publication");
            }
            require(fields.get(Query::changes_b) == 1, "manual paste edit count mismatch");
        }
        if (c.fault == Action::overwrite) {
            DWORD owner_pid{};
            GetWindowThreadProcessId(GetClipboardOwner(), &owner_pid);
            require(owner_pid == fields.pid, "intervening external publication");
        } else require(publisher.is_current(), "intervening external publication");
        expected_sequence = GetClipboardSequenceNumber();
        std::cout << " publication=1 automatic_edits_A=" << c.edit << " automatic_edits_B=0 pass\n";
    }
    Fields alternate;
    alternate.start();
    reset_fields(fields);
    binding.bind();
    binding.allow_restore_from(overlay.window());
    require(SUCCEEDED(publisher.publish(formats)), "Could not publish foreground-change sample");
    reset_fields(alternate);
    require(binding.deliver([&] { return publisher.is_current(); }) == DeliveryResult::lost_target,
            "The gesture recaptured a different application");
    require(GetForegroundWindow() == alternate.window && !fields.get(Query::changes_a) &&
            !alternate.get(Query::changes_a), "Application switch redirected input or focus");
    std::cout << "changed-application: zero edits, new foreground retained\n";
    return 0;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc == 3 && std::wstring_view(argv[1]) == L"--fields")
        return fixture::run_fields(wcstoul(argv[2], nullptr, 10));
    const bool physical = argc == 2 && std::wstring_view(argv[1]) == L"--input-in-disposable-session";
    if (!physical && !(argc == 2 && std::wstring_view(argv[1]) == L"--private-station")) return 2;
    if (FAILED(OleInitialize(nullptr))) return 1;
    int status = 1;
    try { status = physical ? run_desktop() : run_unselected(); }
    catch (const std::exception& e) { std::cerr << " FAIL: " << e.what() << '\n'; }
    OleUninitialize();
    return status;
}
