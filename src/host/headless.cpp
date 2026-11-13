#include "persistence/history_store.hpp"
#include "persistence/hotkey_settings.hpp"
#include "persistence/privacy.hpp"
#include "platform_win/history_clipboard.hpp"
#include <iostream>

namespace {
using namespace cliprope;
using application::Publication;
using application::Route;

int execute(int argc, wchar_t** argv) {
    if (argc < 3) return 2;
    const std::wstring_view action = argv[2];
    if(action==L"verify" && argc==3){
        persistence::Database::verify_store(argv[1]);
        std::cout<<"History database and blob checks passed\n";return 0;
    }
    if(action==L"restore" && argc==4){
        persistence::Database::restore_backup(argv[1],argv[3]);
        std::cout<<"Backup restored\n";return 0;
    }
    if(action==L"backup" && argc==4){
        persistence::Database::export_profile(argv[1],argv[3]);std::cout<<"Backup installed\n";return 0;
    }
    const bool takes_id = action == L"tray" || action == L"double-click" || action == L"return";
    if (argc != (takes_id ? 4 : 3) || (!takes_id && action != L"capture" && action != L"head")) return 2;
    persistence::HistoryStore store(argv[1]);
    win::HistoryClipboard clipboard;
    if (action == L"capture") {
        win::HistoryCapture capture(store, clipboard, CreateThreadpoolWait,
            [&store](const auto& rope, const auto& snapshot) { return store.admit_capture(rope, snapshot); });
        MSG message{};
        PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        capture.wake = [thread = GetCurrentThreadId()](std::uint64_t serial) {
            PostThreadMessageW(thread, WM_NULL, static_cast<WPARAM>(serial), 0);
        };
        auto status = capture.observe(persistence::read_privacy(store.database()));
        while (capture.busy()) {
            MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            for (auto& completed : capture.poll()) status = std::move(completed);
        }
        std::cout << "capture=" << status.status << " entry=" << status.entry << '\n';
        return status.entry.empty() ? 1 : 0;
    }
    std::string id;
    Route route;
    if (action == L"head") {
        auto entries = store.hanging(store.active_rope());
        if (entries.empty()) { std::cout << "history=empty\n"; return 1; }
        id = entries.front().id;
        route = Route::rope_head;
    } else {
        auto arg = std::wstring_view(argv[3]);
        if (arg.empty() || arg.size() > 128 || arg.find_first_not_of(L"0123456789abcdefABCDEF-") != arg.npos) return 2;
        for (const wchar_t digit : arg) id.push_back(static_cast<char>(digit));
        route = action == L"return" ? Route::return_button : (action == L"tray" ? Route::tray : Route::double_click);
    }
    if (route == Route::double_click && !store.entry(id).rope_id) {
        std::cerr << "Floating double-click actions require the desktop. Use the card in its Floating window.\n";
        return 1;
    }
    win::EditGesture gesture;
    if (route == Route::double_click) gesture.bind();
    const auto preferences = persistence::read_card_preferences(store.database());
    Publication followed = Publication::skipped;
    if (route == Route::return_button) {
        store.on_head_change = [&](const auto& change) {
            followed = publish_head_change(change, preferences, store,
                [&](const auto& content) { return clipboard.publish(content); });
        };
    }
    auto command = route == Route::double_click
        ? application::Invocation::hanging_double_click(store.entry(id),
              preferences.paste_on_double_click)
        : application::bind_invocation(store, route, id);
    auto result = command.run(store, [&](const auto& content) { return clipboard.publish(content, route == Route::double_click && gesture.bound_folder()); },
                              [&](const auto& content) { return clipboard.deliver(gesture, content); });
    if (route == Route::return_button) result.publication = followed;
    static constexpr const char* publication[] = {"skipped", "failed", "published"};
    static constexpr const char* history[] = {"skipped", "failed", "committed"};
    std::cout << "clipboard=" << publication[static_cast<int>(result.publication)]
              << " delivery=" << application::delivery_name(result.delivery)
              << " history=" << history[static_cast<int>(result.history)] << '\n';
    if (result.publication == application::Publication::published) {
        const auto flushed = clipboard.flush_current();
        if (FAILED(flushed.result)) { std::cerr << flushed.summary() << '\n'; return 1; }
        if (flushed.result == clipboard_partial_flush) std::cout << flushed.summary() << '\n';
    }
    return result.history == application::HistoryEffect::committed && result.publication != Publication::failed ? 0 : 1;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (FAILED(OleInitialize(nullptr))) return 1;
    int result = 1;
    try { result = execute(argc, argv); }
    catch (const persistence::StorageFailure& failure) { std::cerr << failure.what() << '\n'; }
    catch (const std::exception&) { std::cerr << "History command failed. Check the profile, destination and target ID. Keep the original folder for recovery.\n"; }
    OleUninitialize();
    if (result == 2) std::cerr << "cliprope_headless STORE capture|head|tray ID|double-click ID|return ID|verify|backup DEST|restore DEST\n";
    return result;
}
