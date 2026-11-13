#include "host/ordered_commands.hpp"
#include "host/menu_observation.hpp"
#include "platform_win/unicode_name.hpp"
#include "host/card_preparation.hpp"
#include "host/recall_session.hpp"
#include "host/floating_windows.hpp"
#include "platform_win/popup_menu.hpp"
#include "platform_win/menu_timing.hpp"
#include "application/invoke.hpp"
#include "application/diagnostics.hpp"
#include "persistence/privacy.hpp"
#include "platform_win/diagnostic_export.hpp"
#include "rendering/card_browser.hpp"
#include "persistence/hotkey_settings.hpp"
#include "presentation/color_preview.hpp"
#include "persistence/localization_settings.hpp"
#include "platform_win/localization.hpp"
#include "persistence/profile_location.hpp"
#include "platform_win/card_editor.hpp"
#include "platform_win/expansion_prompt.hpp"
#include "platform_win/floating.hpp"
#include "platform_win/external_drag.hpp"
#include "platform_win/history_clipboard.hpp"
#include "platform_win/card_files.hpp"
#include "platform_win/file_group.hpp"
#include "presentation/card_content.hpp"
#include "presentation/card_labels.hpp"
#include "platform_win/rope_manager.hpp"
#include "platform_win/tray.hpp"
#include "platform_win/login_startup.hpp"
#ifdef CLIPROPE_DESKTOP_PROBES
#include "../../tests/compatibility/workspace_fixture_frame.hpp"
#endif
#include "platform_win/workspace.hpp"
#include "platform_win/find_popup.hpp"
#include "host/find_worker.hpp"
#include "host/runtime_policy.hpp"
#include "platform_win/performance_counters.hpp"
#include "rendering/workspace_paint.hpp"
#include <dwmapi.h>
#include <dbt.h>
#include <wtsapi32.h>
#include <shellapi.h>
#include <shlobj.h>
#include <fstream>
#include <future>
#include <deque>
#include <variant>
#include <set>
#include <tuple>
#include <cmath>
#include <map>
#include <sstream>

namespace {
using namespace cliprope;
namespace fs = std::filesystem;
using application::Route;
constexpr wchar_t listener_class[] = L"ClipRope.Listener";
constexpr UINT_PTR capture_timer = 1;
constexpr UINT_PTR paste_binding_timer = 2;
constexpr UINT_PTR find_timer = 3;
constexpr UINT_PTR target_timer = host::RecallSession::timer;
constexpr UINT find_ready = WM_APP + 247;
constexpr UINT snip_bound = WM_APP + 252;
constexpr UINT retry_note = WM_APP + 253;
constexpr UINT preview_ready = WM_APP + 211;
constexpr UINT head_change_ready = WM_APP + 250;
constexpr UINT capture_ready = WM_APP + 251;

win::UiText storage_message(persistence::StorageProblem problem) {
    using Key = localization::TextKey;
    switch (problem) {
    case persistence::StorageProblem::unsupported_format: return win::tr(Key::error_storage_unsupported_format);
    case persistence::StorageProblem::access: return win::tr(Key::error_storage_access);
    case persistence::StorageProblem::busy: return win::tr(Key::error_storage_busy);
    case persistence::StorageProblem::full: return win::tr(Key::error_storage_full);
    case persistence::StorageProblem::missing_blob: return win::tr(Key::error_storage_missing_blob);
    case persistence::StorageProblem::damaged_blob: return win::tr(Key::error_storage_damaged_blob);
    case persistence::StorageProblem::damaged_database: return win::tr(Key::error_storage_damaged_database);
    case persistence::StorageProblem::damaged_manifest: return win::tr(Key::error_storage_damaged_database);
    }
    return win::tr(Key::error_startup);
}

struct Options {
    fs::path profile;
    std::unique_ptr<persistence::ProfileLocation> location;
    win::UiText location_notice;
    bool show{}, manage_sign_in{}, export_diagnostics{};
#ifdef CLIPROPE_DESKTOP_PROBES
    fs::path report;
    std::wstring fault;
    bool defer_floating{};
    std::shared_ptr<std::remove_pointer_t<HKEY>> test_user;
#endif
};

win::UiText startup_failure(const std::exception& error) {
    if (const auto* conflict = dynamic_cast<const win::StartupConflict*>(&error))
        return win::message(localization::TextKey::error_startup_registration_conflict,
                            {{"path", conflict->other_executable()}});
    return win::tr(localization::TextKey::error_startup_registration);
}

std::wstring desktop_binary();

Options parse_options(int argc, wchar_t** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg = argv[i];
        if (arg == L"--show") options.show = true;
        else if (arg == L"--export-diagnostics-on-exit") options.export_diagnostics = true;
        else if (arg == L"--profile" && i + 1 < argc) options.profile = argv[++i];
#ifdef CLIPROPE_DESKTOP_PROBES
        else if (arg == L"--probe" && i + 1 < argc) options.report = argv[++i];
        else if (arg == L"--fault" && i + 1 < argc) options.fault = argv[++i];
        else if (arg == L"--defer-floating") options.defer_floating = true;
#endif
        else throw std::invalid_argument("Invalid desktop option");
    }
#ifdef CLIPROPE_DESKTOP_PROBES
    if ((options.defer_floating && options.report.empty()) ||
        (!options.report.empty() && options.profile.empty()) || (!options.fault.empty() &&
        (options.report.empty() || (options.fault != L"publish" && options.fault != L"history" &&
                                    options.fault != L"resize" && options.fault != L"detach" && options.fault != L"return" && options.fault != L"recycle" &&
                                    options.fault != L"delete" && options.fault != L"capture-timer" && options.fault != L"capture-full" &&
                                    options.fault != L"note-summary" && options.fault != L"note-editor" &&
                                    options.fault != L"note-window" && options.fault != L"note-commit" &&
                                    options.fault != L"reveal-window" &&
                                    options.fault != L"tray-unavailable" && options.fault != L"tray-frame" &&
                                    options.fault != L"gc-probe" && options.fault != L"head-observer" &&
                                    options.fault != L"slow-snapshot" && options.fault != L"shot-observe" && options.fault != L"shot-delay" &&
                                    options.fault != L"rope-create" && options.fault != L"rope-select" && options.fault != L"rope-change" &&
                                    options.fault != L"transfer" && options.fault != L"expand" &&
                                    options.fault != L"expand-window" && options.fault != L"replace-before-delivery"))))
        throw std::invalid_argument("Probes require an explicit profile and report");
#endif
    if (options.profile.empty()) {
        bool isolated_profile{};
#ifdef CLIPROPE_DESKTOP_PROBES
        wchar_t isolated_root[MAX_PATH]{};
        const auto isolated_size = GetEnvironmentVariableW(L"CLIPROPE_PROBE_LOCAL_APP_DATA", isolated_root, DWORD(std::size(isolated_root)));
        isolated_profile = isolated_size && isolated_size < std::size(isolated_root) && fs::path(isolated_root).is_absolute();
#endif
        if (!isolated_profile && !host::installed_desktop(desktop_binary(), win::LoginStartup(desktop_binary()).registration()))
            throw host::DevelopmentProfileRequired();
        PWSTR local{};
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local)))
            throw std::runtime_error("Local application data unavailable");
        std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> release(local, CoTaskMemFree);
        fs::path base(local);
        options.manage_sign_in = true;
#ifdef CLIPROPE_DESKTOP_PROBES
        wchar_t isolated[MAX_PATH]{};
        const DWORD size = GetEnvironmentVariableW(L"CLIPROPE_PROBE_LOCAL_APP_DATA", isolated, MAX_PATH);
        if (size && size < MAX_PATH) {
            base = isolated;
            options.manage_sign_in = false;
            // Qualification may supply a disposable registry subtree alongside the isolated profile.
            wchar_t id[33]{};
            if (GetEnvironmentVariableW(L"CLIPROPE_PROBE_STARTUP_ID", id, DWORD(std::size(id))) == 32 &&
                std::wstring_view(id).find_first_not_of(L"0123456789abcdef") == std::wstring_view::npos) {
                HKEY key{};
                const auto path = std::wstring(L"Software\\ClipRope.TestStartup.") + id;
                if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, &key))
                    throw std::runtime_error("The isolated registration fixture is unavailable.");
                options.test_user.reset(key, RegCloseKey);
                options.manage_sign_in = true;
            }
        }
#endif
        options.location = std::make_unique<persistence::ProfileLocation>(base);
        try { options.location->resume(); }
        catch (const persistence::StorageFailure& failure) {
            options.location_notice = storage_message(failure.reason());
        } catch (const persistence::LocationError&) {
            options.location_notice = win::tr(localization::TextKey::error_relocation);
        } catch (const std::exception&) {
            options.location_notice = win::tr(localization::TextKey::error_relocation);
        }
        options.profile = options.location->active();
    }
    return options;
}

std::wstring desktop_binary() {
    std::wstring binary(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, binary.data(), DWORD(binary.size()));
    if (!length || length >= binary.size()) throw std::runtime_error("Cannot locate ClipRope.");
    binary.resize(length);
    return binary;
}

class Desktop {
public:
    explicit Desktop(Options options, localization::LocalizationManager& language)
        : options_(std::move(options)), language_(language),
          store_(options_.profile, [this](auto boundary) { check_fault(boundary); }),
          records_(store_), browser_(records_), capture_(store_, clipboard_, CreateThreadpoolWait,
              [this](const auto& rope, const auto& snapshot) { return store_.admit_capture(rope, snapshot); }) {
        language_.set_language(persistence::read_language(store_.database()), win::preferred_ui_locale());
        rendering::WorkspacePaint::use_localization(&language_, win::ui_font_family());
        if (options_.manage_sign_in) {
            HKEY user = HKEY_CURRENT_USER;
#ifdef CLIPROPE_DESKTOP_PROBES
            if (options_.test_user) user = options_.test_user.get();
#endif
            startup_.emplace(desktop_binary(), user);
            try { prepare_startup(); }
            catch (const std::exception& error) { startup_notice_ = startup_failure(error); }
        }
        card_preferences_ = persistence::read_card_preferences(store_.database());
        diagnostics_.cleanup_deferred = store_.startup_collection().deferred;
        appearance_ = persistence::read_appearance(store_.database());
        win::fluent::set_theme(appearance_.theme);
        try { policy_ = persistence::read_privacy(store_.database()); }
        catch (const std::exception&) { policy_.paused = true; policy_damaged_ = true; }
#ifdef CLIPROPE_DESKTOP_PROBES
        if (!options_.report.empty()) {
            report_.open(options_.report, std::ios::app);
            if (!report_) throw std::runtime_error("Cannot open probe report");
        }
        fail_capture_commit_ = options_.fault == L"capture-full";
        slow_snapshot_ = options_.fault == L"slow-snapshot" || options_.fault == L"shot-delay";
#endif
    }
    ~Desktop() {
        preparation_.shutdown();
        external_drag_.cancel();
        capture_.abandon(); // Unregister and drain process callbacks before destroying the listener.
        capture_.settle_completed_snips();
        find_.close(false);
        find_worker_.reset();
        inline_editor_.discard();
        inline_editor_.drain();
        if (gc_job_.valid()) gc_job_.wait();
        snip_jobs_.clear();
        browser_.on_preview_ready({});
        if (power_registration_) {
            const bool removed = UnregisterSuspendResumeNotification(power_registration_) != FALSE;
            log(removed ? "power unsubscribed" : "power unsubscribe-failed");
        }
        if(session_registered_)WTSUnRegisterSessionNotification(listener_);
        if (listening_) RemoveClipboardFormatListener(listener_);
        if (listener_) KillTimer(listener_, capture_timer);
        if (screenshot_hover_) screenshot_hover_->cancel(true);
        ropes_.reset();
        editor_.reset();
        floating_.clear();
        tray_.reset();
        workspace_.reset();
        screenshot_hover_.reset();
        if (listener_) DestroyWindow(listener_);
    }

    static constexpr int restart_status = 75;
    int run() {
        WNDCLASSW wc{};
        wc.lpfnWndProc = listener_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = listener_class;
        if (!RegisterClassW(&wc)) throw std::runtime_error("Listener registration failed");
        listener_ = CreateWindowExW(WS_EX_TOOLWINDOW, listener_class, L"", WS_POPUP,
                                    0, 0, 0, 0, nullptr, nullptr, wc.hInstance, this);
        if (!listener_) throw std::runtime_error("Listener window failed");
        power_registration_ = RegisterSuspendResumeNotification(listener_, DEVICE_NOTIFY_WINDOW_HANDLE);
        log(power_registration_ ? "power subscribed" : "power subscription-unavailable");
        if(ProcessIdToSessionId(GetCurrentProcessId(),&session_id_))
            session_registered_=WTSRegisterSessionNotification(listener_,NOTIFY_FOR_THIS_SESSION)!=FALSE;
        browser_.on_preview_ready([this, window = listener_] {
#ifdef CLIPROPE_DESKTOP_PROBES
            ++preview_notices_;
#endif
            PostMessageW(window, preview_ready, 1, 0);
        });
        store_.on_snip_bound = [window = listener_] { PostMessageW(window, snip_bound, 0, 0); };
        screenshot_hover_ = std::make_unique<win::ScreenshotHover>(listener_, [this](const auto& entry, auto stop, auto deadline) {
            return persistence::ContentRepository(store_.database()).screenshot(entry,stop,deadline);
        });
        screenshot_hover_->enabled(card_preferences_.screenshot_hover_preview);
        workspace_ = std::make_unique<win::Workspace>(reads(), workspace_actions());
        inline_editor_.defer_write = [this](auto command) { return defer_history(std::move(command)); };
        inline_editor_.binding_changed = [this] {
            if (workspace_) workspace_->editor_changed();
            if (!inline_editor_.owner()) refresh_find(true);
            if (exit_after_edit_ && !inline_editor_.previewing()) {
                exit_after_edit_ = false;
                if (!inline_editor_.owner()) PostMessageW(listener_, WM_CLOSE, 0, 0);
            }
        };
        inline_editor_.committed = [this](const application::TextDraft& draft, const domain::Id& snapshot) {
            finish_head_change();
            // The first press may land on the submitted preview and the second
            // just after commit. Bind this pair to the text the user saw.
            if (bound_ && bound_->on_submitted_text && bound_->entry.id == draft.entry_id &&
                bound_->entry.revision == draft.revision) {
                bound_->entry.snapshot_id = snapshot;
                ++bound_->entry.revision;
            }
            collect_after_mutation();
        };
        inline_editor_.command_failed = [this] { error(); };
        inline_editor_.evicted = [this](const domain::Id& id) {
            if (floating_.active().contains(id)) floating_.retire(id);
            else workspace_->refresh();
        };
        workspace_->always_on_top(persistence::read_workspace_topmost(store_.database()));
        if (!workspace_->open()) throw std::runtime_error("Workspace creation failed");
        workspace_->match_color(appearance_.match_rgb());
        find_.changed = [this](std::wstring term) {
            find_term_ = std::move(term);
            refresh_find(false);
        };
        find_.closed = [this] {
            KillTimer(listener_, find_timer);
            if (find_worker_) find_worker_->submit({});
            workspace_->find_highlights({});
            find_term_.clear();
            find_hit_.reset();
            find_total_ = find_index_ = 0;
        };
        find_.navigate = [this](int step) {
            if (!find_total_) return;
            find_index_ = (find_index_ + find_total_ + (step > 0 ? 1 : find_total_ - 1)) % find_total_;
            submit_find(false);
        };
        workspace_->image_settings(browser_.image_preferences);
#ifdef CLIPROPE_DESKTOP_PROBES
        if (options_.fault == L"tray-unavailable") {
            tray_ = std::make_unique<win::Tray>(tray_actions(), [](DWORD, PNOTIFYICONDATAW) -> BOOL { return FALSE; }, true);
        } else
#endif
        tray_ = std::make_unique<win::Tray>(tray_actions());
        if (!tray_->start(listener_)) throw std::runtime_error("Tray registration failed");
        store_.on_head_change = [this](const auto& change) { head_changed(change); };
        if (!options_.location_notice.empty()) tray_->notify(options_.location_notice.c_str());
        if (!startup_notice_.empty()) tray_->notify(startup_notice_);
        if (policy_damaged_)
            tray_->notify(win::tr(localization::TextKey::error_capture_policy));
        editor_ = std::make_unique<win::CardEditor>(editor_actions());
        ropes_ = std::make_unique<win::RopeManager>(rope_actions());
        log(!tray_->hotkey_enabled() ? "hotkey disabled" : tray_->hotkey_registered() ? "hotkey registered" : "hotkey conflict");
        // Opening a resident host does not retroactively import earlier copies.
        capture_.wake = [window = listener_](std::uint64_t serial) {
            PostMessageW(window, capture_ready, static_cast<WPARAM>(serial), 0);
        };
        capture_.watch_system_snips();
        baseline_ = GetClipboardSequenceNumber();
        if (!AddClipboardFormatListener(listener_)) throw std::runtime_error("Clipboard listener unavailable");
        listening_ = true;
        if (GetClipboardSequenceNumber() != baseline_) PostMessageW(listener_, WM_CLIPBOARDUPDATE, 0, 0);
        for (const auto& id : records_.floating_ids(true)) {
            try { restore_float(id); }
            catch (const std::exception&) {
                log("restore-floating " + id + " failed");
                tray_->notify(win::tr(localization::TextKey::error_floating_restore));
            }
        }
        log("collector " + store_.active_rope());
        if (options_.show || tray_->needs_workspace()) show();
        else log("workspace hidden");
#ifdef CLIPROPE_DESKTOP_PROBES
        if (options_.fault == L"tray-unavailable" || options_.fault == L"tray-frame")
            workspace_fixture::frame(workspace_->window(), std::filesystem::path(options_.report).replace_extension(L".bmp"), false);
#endif
        SetWindowTextW(listener_, L"ClipRope");
        log("ready");
        MSG msg{};
        int status{};
        ULONGLONG next_fallback_poll{};
        for (;;) {
            // A failed USER timer cannot strand a staged snip or a missed worker wake.
            if (capture_.busy() && !timer_) {
                const auto now = GetTickCount64();
                if (now >= next_fallback_poll) {
                    next_fallback_poll = now + 12;
                    message(listener_, capture_ready, 0, 0);
                }
                if (!PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    const auto waited = MsgWaitForMultipleObjectsEx(0, nullptr, 12, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                    if (waited == WAIT_FAILED) throw std::runtime_error("Capture wake unavailable");
                    continue;
                }
                status = msg.message == WM_QUIT ? 0 : 1;
            } else status = GetMessageW(&msg, nullptr, 0, 0);
            if (status <= 0) break;
            if (msg.message == WM_MOUSEMOVE && !(msg.wParam & MK_LBUTTON) && !bound_) recall_session_.warm();
            if (!find_.translate(msg) && !inline_editor_.translate(msg) && !ropes_->dialog_message(msg) &&
                !editor_->dialog_message(msg) && !tray_->dialog_message(msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            // After the outermost dispatch returns, no Floating window procedure
            // is executing. This also covers nested OLE and context-menu loops.
            floating_.finish_dispatch();
        }
        RemoveClipboardFormatListener(listener_);
        listening_ = false;
        const bool captures_saved = capture_.settle_completed_snips();
        if (!captures_saved) log("capture history-failed during shutdown");
        capture_.abandon();
        const auto flushed = clipboard_.flush_current();
        if (FAILED(flushed.result) || flushed.result == clipboard_partial_flush) {
            const auto summary = flushed.summary();
            log(summary);
            OutputDebugStringA((summary + "\n").c_str());
        }
        if (options_.export_diagnostics)
            win::export_diagnostics(store_.database().root(), diagnostics_.report(store_.failed_head_notifications()));
        if (FAILED(flushed.result)) return 1;
        return status < 0 || !captures_saved ? 1 : restarting_ ? restart_status : static_cast<int>(msg.wParam);
    }
private:
    struct PendingHead {
        application::HeadChange change;
        DWORD clipboard_sequence;
    };

    host::OrderedCommands order_;
    host::CardPreparation preparation_;
    bool exit_after_admission_{}, closing_failure_{};
    bool order_pending() const { return capture_.writing() || preparation_.active() || !order_.empty(); }
    void queue_command(std::function<void()> command) {
        order_.enqueue(GetClipboardSequenceNumber(), std::move(command));
    }
    bool defer_history(std::function<void()> command) {
        if (!order_pending()) return false;
        queue_command(std::move(command));
        return true;
    }
    template<class F> void after_admission(F&& command) {
        if (order_pending()) queue_command(std::forward<F>(command));
        else command();
    }
    void observe_clipboard(win::HistoryCapture::Observation observation) {
        const auto state = capture_.observe_bound(std::move(observation), policy_);
        diagnostics_.capture(state.status, state.issue);
        if (state.status != "pending") log("capture " + state.status);
#ifdef CLIPROPE_DESKTOP_PROBES
        if (report_.is_open()) log("clipboard-update seq=" + std::to_string(capture_.health().observed_sequence) + " " + state.status);
#endif
    }
    void progress_commands() {
        order_.drain([this] { return capture_.writing() || preparation_.active(); }, [this](auto observation) {
            observe_clipboard(std::move(observation));
        }, [this] { error(); });
    }

    void head_changed(const application::HeadChange& change) {
#ifdef CLIPROPE_DESKTOP_PROBES
        if (GetCurrentThreadId() == ui_thread_ && options_.fault == L"head-observer") {
            options_.fault.clear();
            log("injected head observer failure");
            throw std::runtime_error("Synthetic post-commit observer failure");
        }
#endif
        {
            std::lock_guard lock(head_mutex_);
            if (change.serial <= last_head_serial_) return;
            last_head_serial_ = change.serial;
            const auto sequence = GetCurrentThreadId() == ui_thread_ ? order_.sequence().value_or(GetClipboardSequenceNumber())
                                                                     : GetClipboardSequenceNumber();
            pending_head_ = PendingHead{change, sequence};
        }
        if (GetCurrentThreadId() == ui_thread_) finish_head_change();
        else PostMessageW(listener_, head_change_ready, 0, 0);
    }

    void finish_head_change() {
        // Let an admitted external copy finish on its bound rope before replacing its IDataObject.
        if (capture_.busy()) return;
        std::optional<PendingHead> pending;
        {
            std::lock_guard lock(head_mutex_);
            pending = std::exchange(pending_head_, std::nullopt);
        }
        if (!pending || pending->clipboard_sequence != GetClipboardSequenceNumber() ||
            !pending->change.follows(card_preferences_)) return;
        const auto& change = pending->change;
        auto result = application::Publication::failed;
        try {
            if (store_.collector_head() != change.after) return;
            result = application::publish_head_change(change, card_preferences_, store_,
                [&](const auto& content) { return clipboard_.publish(content, false, pending->clipboard_sequence); });
        } catch (const std::exception&) {
            // The history commit has already succeeded even if its head cannot be read.
        }
        if (result == application::Publication::skipped) return;
        if (result == application::Publication::failed) {
            ++diagnostics_.failures;
            log("follow-head failed");
            tray_->notify(win::tr(localization::TextKey::clipboard_head_not_updated));
        } else log("follow-head published " + change.after.entry);
    }

    std::set<std::tuple<domain::Id, domain::Id, std::int64_t>> pending_menu_kinds_;
    using PathLookup = rendering::CardBrowser::PathLookup;

    void confirm_menu_observation(application::CardSummary& card) {
        constexpr auto lookup = PathLookup::inspect_for_menu;
        if (!order_pending()) {
            host::confirm_menu_observation(store_, browser_, card, lookup);
            return;
        }
        browser_.inspect_path_kind(card, lookup);
        if (!card.unclassified_path || card.observed_path_kind == domain::PathKind::unknown) return;
        // A full queue leaves confirmation with the popup bound to this exact entry.
        if (order_.full()) return;
        const auto key = std::make_tuple(card.entry.id, card.entry.snapshot_id, card.entry.revision);
        if (pending_menu_kinds_.contains(key)) return;
        queue_command([this, key, entry = card.entry, kind = card.observed_path_kind] {
            pending_menu_kinds_.erase(key);
            try { store_.save_path_kind(entry, kind); }
            catch (const application::StaleEdit&) { return; }
            PostMessageW(listener_, preview_ready, 0, 0);
        });
        pending_menu_kinds_.insert(key);
    }

    application::CardSummary display_summary(std::string_view id, bool preview = false, unsigned width = rendering::preview_width) {
        auto card = browser_.summary(id, preview, width);
        presentation::localize_card(card, language_);
        return card;
    }
    application::CardSummary display_menu_summary(std::string_view id) {
        auto card = browser_.menu_summary(id);
        confirm_menu_observation(card);
        presentation::localize_card(card, language_);
        return card;
    }
    std::vector<application::CardSummary> display_page(std::string_view rope, size_t offset, size_t count,
                                                     bool preview = false, unsigned width = rendering::preview_width) {
        auto cards = browser_.page(rope, offset, count, preview, width);
        for (auto& card : cards) presentation::localize_card(card, language_);
        return cards;
    }
    using Press = host::CardPress;

    void press_card(const domain::Id& id, HWND surface) {
        replace_drag();
        supersede_target();
        bound_.reset();
        auto target = recall_session_.press(surface, workspace_->track_card_press());
        bound_.emplace(Press{std::move(target), store_.entry(id),
                             inline_editor_.previewing() && inline_editor_.entry() == id});
        const auto interval = GetDoubleClickTime();
        paste_deadline_ = GetTickCount64() + interval;
        if (!SetTimer(listener_, paste_binding_timer, interval, nullptr)) bound_->target.reset();
    }
    void double_click(const domain::Id& id) {
        KillTimer(listener_, paste_binding_timer);
        if (recall_session_.active() || !bound_ || bound_->entry.id != id) return;
        auto press = std::move(*bound_);
        bound_.reset();
        if (!press.entry.rope_id) return;
        press.paste = card_preferences_.paste_on_double_click;
        if (order_pending() || (press.target && !press.target->binding_ready())) {
            press.confirmed_sequence = GetClipboardSequenceNumber();
            log("deferred double-click " + id);
        }
        if (inline_editor_.owner() == workspace_->window()) {
            auto gesture = std::make_shared<Press>(std::move(press));
            const bool saved_target = inline_editor_.entry() == id;
            inline_editor_.save([this, id, gesture, saved_target] {
                if (saved_target) {
                    auto current = store_.entry(id);
                    if (current.rope_id != gesture->entry.rope_id) return;
                    gesture->entry = std::move(current);
                }
                paste_hanging(std::move(*gesture), id);
            });
            return;
        }
        paste_hanging(std::move(press), id);
    }
    void paste_hanging(Press press, const domain::Id& id) {
        if (recall_session_.wait_for_binding(listener_, press)) return;
        auto binding = std::make_shared<Press>(std::move(press));
        after_admission([this, binding, id] {
            auto command = application::Invocation::hanging_double_click(std::move(binding->entry), binding->paste);
            invoke(command, Route::double_click, id, binding->target, binding->confirmed_sequence);
        });
    }

    void floating_double_click(const domain::Id& id) {
        if (!bound_ || bound_->entry.id != id) return;
        const auto pressed = std::move(bound_->entry);
        bound_.reset();
        if (pressed.rope_id) return;
        after_admission([this, pressed] { open_floating_content(pressed); });
    }
    void open_floating_content(const domain::Entry& pressed) {
        const auto& id = pressed.id;
        if (!floating_.active().contains(id)) return;
        const auto card = display_menu_summary(id);
        if (card.entry.rope_id || card.entry.snapshot_id != pressed.snapshot_id ||
            card.entry.revision != pressed.revision) return;
        auto& surface = *floating_.active().at(id);
        using Kind = application::CardMenuKind;
        switch (card.menu) {
        case Kind::text: {
            edit_card(pressed);
            break;
        }
        case Kind::single_file: case Kind::screenshot:
            visit_files(surface.window(), win::CardCommand::open, card);
            break;
        case Kind::multiple_files: case Kind::folder:
            visit_files(surface.window(), win::CardCommand::reveal, card);
            break;
        case Kind::other:
            editor_->show_information(card);
            break;
        }
    }

    void replace_drag() {
        if (external_drag_.staging()) log("external-drag owner-replaced");
        drag_serial_ = external_drag_.replace();
    }
    void external_drag(application::CardSummary card) {
        if (!bound_ || bound_->entry.id != card.entry.id) return;
        const auto expected = bound_->entry;
        bound_.reset();
        KillTimer(listener_, paste_binding_timer);
        const auto surface = expected.rope_id ? workspace_->window() : floating_.active().at(expected.id)->window();
        const auto sequence = GetClipboardSequenceNumber();
        after_admission([this, expected, surface, sequence, serial = drag_serial_, card = std::move(card)] {
            if (exit_after_admission_) { log("external-drag quit"); return; }
            auto result = external_drag_.run(listener_, surface, store_, expected, card, serial, sequence);
            log(std::string("external-drag ") + std::string(result));
        });
    }

    win::WorkspaceReads reads() {
        win::WorkspaceReads result;
        result.count = [this](auto rope) { return records_.count(rope); };
        result.page = [this](auto rope, auto offset, auto count) {
            return display_page(rope, offset, count, true, workspace_->picture_width(std::string(rope)));
        };
        result.position = [this](auto rope, auto entry) { return records_.position(rope, entry); };
        result.load_anchor = [this](auto rope) { return records_.load_anchor(rope); };
        result.save_anchor = [this](auto rope, const auto& anchor) {
            after_admission([this, id = std::string(rope), anchor] { store_.save_anchor(id, anchor); });
        };
        result.rope_count = [this] { return records_.rope_count(); };
        result.total_rope_count = [this] { return records_.rope_count(true); };
        result.rebin_visible = [this] { return store_.rebin_visible(); };
        result.ropes = [this](auto offset, auto count) { return records_.ropes(offset, count); };
        result.rope_position = [this](auto id) { return records_.rope_position(id); };
        result.load_compact = [this](auto id) { return store_.rope_compact(id); };
        result.save_compact = [this](auto id, bool compact, const auto& anchor) {
            after_admission([this, rope = std::string(id), compact, anchor] { store_.save_rope_compact(rope, compact, anchor); });
        };
        return result;
    }

    win::WorkspaceActions workspace_actions() {
        win::WorkspaceActions actions;
        actions.colors = &colors_;
        actions.screenshot_hover = screenshot_hover_.get();
        actions.resize = resize_action();
        actions.find_scope_changed = [this] { refresh_find(true); };
        actions.find = [this](POINT p) {
            start_find_worker();
            find_.open(workspace_->window(),p,find_label());
        };
        actions.editor = &inline_editor_;
        actions.edit = [this](const auto& id) { edit_card(store_.entry(id)); };
        actions.external_drag = [this](auto card) { external_drag(std::move(card)); };

        actions.lost_focus = [this](HWND surface) {
            recall_session_.lost_focus(surface);
        };
        actions.cancel_paste = [this] {
            if (bound_) bound_->target.reset();
        };
        actions.press = [this](const auto& id) { press_card(id, workspace_->window()); };
        actions.double_click = [this](const auto& id) { double_click(id); };
        actions.reorder = [this](const auto& id, size_t offset, win::DropCompleted landed) {
            if (!bound_ || bound_->entry.id != id) { landed({}); return; }
            auto press = std::move(*bound_);
            bound_.reset();
            after_admission([this, id, offset, expected = std::move(press.entry), landed = std::move(landed)] {
                try { store_.reorder(id, offset, &expected); }
                catch (...) { landed({}); throw; }
                committed();
                log("reorder " + id);
                landed(id);
            });
        };
        actions.erase = [this](const auto& id, const auto& rope) {
            bound_.reset();
            remove_card(id, rope, card_preferences_.recycle_hanging && rope != domain::rebin_id);
        };
        actions.detach = [this](const auto& id, POINT point, float x, float y) { detach(id, point, x, y); };
        actions.select = [this](const auto& rope) { select(rope); };
        actions.set_rebin_visible = [this](bool visible) {
            auto change = [this, visible] {
                write(L"rope-select", [&] { store_.set_rebin_visible(visible); });
                collector_changed(store_.active_rope());
            };
            auto submit = [this, change] { after_admission(change); };
            if (inline_editor_.owner() == workspace_->window()) inline_editor_.save(submit);
            else submit();
        };
        actions.rope_top = [this](const auto& rope) { return store_.rope_top(rope); };
        actions.save_rope_top = [this](const auto& rope, double top) {
            after_admission([this, rope, top] { store_.save_rope_top(rope, top); });
        };
        actions.reorder_rope = [this](const auto& from, const auto& to, const auto& head, win::DropCompleted landed) {
          after_admission([this, from, to, head, landed = std::move(landed)] {
            auto collector = store_.reorder_rope(from, to, head);
            log("collector " + collector);
#ifdef CLIPROPE_DESKTOP_PROBES
            if (deferred_) PostMessageW(listener_, resume_return, 0, 0);
#endif
            landed(collector);
          });
        };
        actions.rope_menu = [this](const auto& rope, POINT p, win::WorkspaceMenuState state) {
            win::menu_mark(win::MenuStage::focus);
            return ropes_->menu(workspace_->window(), p, rope, state);
        };
        actions.transfer = [this](const auto& id, const auto& rope, bool copy, size_t index, win::DropCompleted landed) {
            transfer(id, rope, copy, index, std::move(landed));
        };
        actions.context = [this](application::CardMenuSnapshot card, POINT point) {
            win::menu_mark(win::MenuStage::focus);
            editor_->menu(workspace_->window(), point, std::move(card));
        };
        actions.head = [this](const auto& rope) {
            auto first = display_page(rope, 0, 1);
            if (!first.empty()) recall(first.front().entry.id, Route::rope_head);
        };
        actions.hide = [this] { hide(); };
        actions.error = [this] { error(); };
        actions.shortcut_error = [this](DWORD code) {
            ++diagnostics_.failures;
            log("hover shortcut unavailable " + std::to_string(code));
            if (tray_) tray_->notify(win::message(localization::TextKey::error_hover_shortcuts, {{"code", std::to_wstring(code)}}));
        };
        actions.layout_changed = [this] { frame(); win::PopupMenuHost::file_preview_ready(); };
        return actions;
    }

    void select(const domain::Id& rope) {
        const bool hide_rebin = !workspace_->in_overview() && rope != domain::rebin_id;
        after_admission([this, rope, hide_rebin] {
            if (rope == store_.active_rope() || !workspace_->prepare_selection(rope)) return;
            write(L"rope-select", [&] { store_.set_active_rope(rope, hide_rebin); });
            try { collector_changed(rope); }
            catch (const std::exception&) { error(); }
        });
    }

    void collector_changed(const domain::Id& rope) {
        workspace_->set_active(rope);
        log("collector " + rope);
#ifdef CLIPROPE_DESKTOP_PROBES
        if (deferred_) PostMessageW(listener_, resume_return, 0, 0);
#endif
    }

    void transfer(const domain::Id& id, const domain::Id& target, bool copy, size_t index, win::DropCompleted landed) {
        if (!bound_ || bound_->entry.id != id) { landed({}); return; }
        auto press = std::move(*bound_);
        bound_.reset();
        const auto& expected = press.entry;
        if (!copy && (!expected.rope_id || *expected.rope_id == target)) { landed({}); return; }
        after_admission([this, expected, target, id, index, copy, landed = std::move(landed)] {
        const auto prefix = "transfer " + std::string(copy ? "copy " : "move ") + id + " " + target;
        domain::Id result;
        try {
            write(L"transfer", [&] {
                if (copy) result = store_.copy(id, target, &expected, index);
                else { store_.move(id, target, &expected, index); result = id; }
            });
        } catch (...) { log(prefix + " failed"); landed({}); throw; }
        log(prefix + " committed " + result);
        committed();
        landed(result);
        });
    }

    win::RopeActions rope_actions() {
        win::RopeActions actions;
        actions.defer_history = [this](auto command) { return defer_history(std::move(command)); };
        actions.preview_color = [this](std::string_view id, std::optional<domain::ColorSelection> color) {
            colors_.rope.reset();
            if (color)
                try {
                    auto rope = records_.rope(id);
                    colors_.rope = presentation::RopeColorPreview{
                        rope.id, *color == rope.color ? rope.material : domain::RopeMaterial::from(*color)};
                } catch (...) {
                }
            workspace_->repaint_colors();
        };
        actions.save_color = [this](std::string_view id, domain::ColorSelection original,
                                    domain::ColorSelection color) {
            store_.save_rope_color(id, original, color);
            try { committed(); }
            catch (const std::exception&) { error(); }
        };
        actions.find_hint = [this] { return tray_->shortcut_hint(application::find_cards); };
        actions.current = [this](const auto& id) { return records_.rope(id); };
        actions.default_capacity = [this] { return store_.default_capacity(); };
        actions.select = [this](const auto& rope) { select(rope); };
        actions.preview = [this](const auto& change) { return store_.preview_rope_change(change); };
        actions.apply = [this](const auto& preview) {
            const auto before = store_.active_rope();
            const auto created = write(preview.change.op == application::RopeOp::create ? L"rope-create" : L"rope-change",
                                       [&] { return store_.apply_rope_change(preview); });
            const auto op = preview.change.op;
            if (op == application::RopeOp::delete_all || op == application::RopeOp::clear_all ||
                (op == application::RopeOp::clear && preview.change.rope == before)) {
                capture_.abandon();
                baseline_ = GetClipboardSequenceNumber();
            }
            // The dialog may now close. A later display error must not leave
            // the same confirmation enabled to repeat a committed mutation.
            try {
                if (preview.change.op == application::RopeOp::delete_all)
                    while (!floating_.active().empty()) floating_.retire(floating_.active().begin()->first);
                const auto after = store_.active_rope();
                if (after != before) collector_changed(after);
                if (op == application::RopeOp::clear || op == application::RopeOp::clear_all ||
                    op == application::RopeOp::delete_all) diagnostics_.cleanup_deferred = store_.gc().deferred;
                committed();
            } catch (const std::exception&) { error(); }
            return created;
        };
        return actions;
    }

    void repaint_color_preview() {
        workspace_->repaint_colors();
        for (auto &[id, window] : floating_.active())
            InvalidateRect(window->window(), nullptr, FALSE);
        if (inline_editor_.owner())
            try {
                auto card = display_menu_summary(inline_editor_.entry());
                inline_editor_.colors(presentation::preview_style(colors_, card.entry.id, card.style));
            } catch (...) {
            }
    }

    win::EditorActions editor_actions() {
        win::EditorActions actions;
        actions.defer_history = [this](auto command) { return defer_history(std::move(command)); };
        actions.preview_color = [this](const domain::Entry &entry, bool paper,
                                       std::optional<domain::ColorSelection> color) {
            if (color)
                colors_.card = presentation::CardColorPreview{entry.id, paper, *color};
            else
                colors_.card.reset();
            repaint_color_preview();
        };
        actions.save_color = [this](const domain::Entry &entry, bool paper, domain::ColorSelection original,
                                    domain::ColorSelection color) {
            store_.save_color(entry, paper, original, color);
            refresh_card(entry.id);
        };
        actions.confirm_path_kind = [this](const auto& menu) {
            if (order_pending()) return records_.saved_path_kind(menu.entry);
            return store_.confirm_menu_kind(menu);
        };
        actions.current = [this](const domain::Entry& entry) {
            auto current = display_menu_summary(entry.id);
            if (current.entry.snapshot_id != entry.snapshot_id) throw application::StaleEdit{};
            return current;
        };
        actions.details = [this](const auto& card) {
            return records_.card_details(card);
        };
        actions.file = [this](win::CardCommand action, const auto& card, HWND) {
            after_admission([this, action, card] {
                const auto surface = card_surface(card.entry);
                if (!surface) return;
                if (action == win::CardCommand::refresh) {
                    browser_.recheck_files(card.file_list_hash);
                    refresh_card(card.entry.id);
                } else visit_files(surface, action, card);
            });
        };
        actions.edit_text = [this](const auto& entry) { edit_card(entry); };
        actions.expand = [this](const auto& entry, HWND) {
            after_admission([this, entry] {
                if (const auto owner = card_surface(entry)) expand_group(entry, owner);
            });
        };
        actions.save_style = [this](auto id, const auto& style, bool set_font) {
            store_.save_style(id, style, set_font);
            try { refresh_card(id); }
            catch (const std::exception&) { error(); }
        };
        return actions;
    }

    void edit_card(const domain::Entry& expected) {
        if (inline_editor_.entry() == expected.id) {
            if (inline_editor_.previewing())
                inline_editor_.save([this, expected] {
                    const auto current = store_.entry(expected.id);
                    if (current.rope_id == expected.rope_id) edit_card(current);
                });
            else inline_editor_.focus();
            return;
        }
        if (inline_editor_.owner()) {
            inline_editor_.save([this, expected] { edit_card(expected); });
            return;
        }
        const auto current = store_.entry(expected.id);
        if (expected.rope_id != current.rope_id || expected.revision != current.revision) return;
        auto draft = store_.begin_text_edit(expected.id);
        if (draft.revision != expected.revision) return;
        if (current.rope_id) workspace_->edit_text(std::move(draft), [this](const auto& initial, auto text) {
            return store_.save_text(initial.entry_id, initial.revision, text);
        });
        else floating_.active().at(expected.id)->edit_text(std::move(draft));
    }

    void refresh_card(std::string_view id) {
        // The save has already committed. Failure to repaint must not make
        // the editor retry an edit against its old revision.
        try {
            if (auto found = floating_.active().find(id); found != floating_.active().end())
                found->second->update(display_summary(id, true));
            committed();
        } catch (const std::exception&) { error(); }
    }

    void visit_files(HWND surface, win::CardCommand action, const application::CardSummary& card) {
        if (card.menu != application::CardMenuKind::screenshot) {
            win::visit_card_files(surface, action, card, store_);
            return;
        }
#ifdef CLIPROPE_DESKTOP_PROBES
        log("screenshot-prepare " + card.entry.id);
#endif
        preparation_.start(listener_, [this, entry = card.entry, action](auto stop) {
            if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) throw std::runtime_error("Image apartment unavailable");
            struct Apartment { ~Apartment() { CoUninitialize(); } } apartment;
            host::CardPreparation::Result result;
            result.snapshot = store_.read_for_display(entry.snapshot_id, stop);
            auto target = win::screenshot_target(store_, entry, result.snapshot.content.snapshot, action, stop);
            result.path = std::move(target.path);
            result.folder_only = target.folder_only;
            return result;
        }, [this, surface, action, card](auto ready) {
            if (exit_after_admission_ || !IsWindow(surface)) return;
            try { if (card_surface(card.entry) != surface) return; }
            catch (const application::MissingHistoryTarget&) { return; }
            host::CardPreparation::Result result;
            try { result = ready.get(); }
            catch (const std::exception& error) {
#ifdef CLIPROPE_DESKTOP_PROBES
                log("screenshot-prepare-failed reason=" + std::to_string(static_cast<int>(application::history_issue(error))));
#endif
                throw;
            }
#ifdef CLIPROPE_DESKTOP_PROBES
            if (options_.fault == L"shot-observe" || options_.fault == L"shot-delay") {
                log("screenshot-ready " + card.entry.id);
                return; // The isolated fixture verifies the PNG without launching the user's viewer.
            }
#endif
            win::ScreenshotTarget target{std::move(result.path), result.folder_only};
            win::visit_card_files(surface, action, card, store_, &target);
        }, surface);
    }

    HWND card_surface(const domain::Entry& expected) {
        const auto entry = store_.entry(expected.id);
        if (entry.rope_id != expected.rope_id || entry.snapshot_id != expected.snapshot_id || entry.revision != expected.revision) return {};
        if (entry.rope_id) return workspace_->window();
        const auto found = floating_.active().find(entry.id);
        return found == floating_.active().end() ? nullptr : found->second->window();
    }
    std::optional<std::uint64_t> pending_placement(const domain::Id& id) const {
        const auto found = floating_.active().find(id);
        if (!order_pending() || found == floating_.active().end()) return {};
        return found->second->gesture_epoch();
    }
    void rollback_placement(const domain::Id& id, const std::optional<std::uint64_t> epoch) {
        const auto found = floating_.active().find(id);
        if (!epoch || found == floating_.active().end() || found->second->gesture_epoch() != *epoch) return;
        found->second->update(display_summary(id, true));
        found->second->restore_placement(store_.layout(id));
    }
    decltype(win::WorkspaceActions::resize) resize_action() {
        return [this](const domain::Entry& entry, auto scale, const domain::Layout* placement) {
            auto layout = placement ? std::optional(*placement) : std::nullopt;
            after_admission([this, entry, scale, layout, epoch = pending_placement(entry.id)] {
                try { write(L"resize", [&] { store_.resize(entry, scale, layout ? &*layout : nullptr); }); }
                catch (...) {
                    if (entry.rope_id) workspace_->refresh();
                    else rollback_placement(entry.id, epoch);
                    throw;
                }
                PostMessageW(listener_, preview_ready, 0, 0);
            });
        };
    }
    win::FloatingActions floating_actions() {
        win::FloatingActions actions;
        actions.colors = &colors_;
        actions.screenshot_hover = screenshot_hover_.get();
        actions.resize = resize_action();
        actions.editor = &inline_editor_;
        actions.edit = [this](const auto& id) { edit_card(store_.entry(id)); };
        actions.external_drag = [this](auto card) { external_drag(std::move(card)); };
        actions.visible_overview = [this] { return workspace_->visible() && workspace_->in_overview(); };
        actions.copy_to_rope = [this](const auto& id, const auto& slot) {
            transfer(id, slot.rope.id, true, slot.index, [this, slot](domain::Id landed) {
                workspace_->settle_floating_drop(landed, slot);
            });
        };
        actions.lost_focus = [this](HWND surface) {
            recall_session_.lost_focus(surface);
        };
        actions.cancel_paste = [this] {
            if (bound_) bound_->target.reset();
        };
        actions.press = [this](const auto& id) {
            supersede_target();
            replace_drag();
            bound_.emplace(Press{nullptr, store_.entry(id)});
        };
        actions.double_click = [this](const auto& id) { floating_double_click(id); };
        actions.save_text = [this](const auto& draft, std::wstring_view text) {
            return store_.save_text(draft.entry_id, draft.revision, text);
        };
        actions.return_current = [this](const auto& id) { return_float(id, store_.active_rope()); };
        actions.preview_at = [this](POINT screen, const auto& card) { return workspace_->preview_floating(screen, &card); };
        actions.request_preview = [this] { PostMessageW(listener_, preview_ready, 0, 0); };
        actions.clear_preview = [this] { workspace_->clear_floating_preview(); };
        actions.drop = [this](const auto& id, const auto& target) {
            if (!bound_ || bound_->entry.id != id)
                throw std::runtime_error("The dragged Floating card changed");
            auto press = std::move(*bound_);
            bound_.reset();
            after_admission([this, id, target, expected = press.entry, epoch = pending_placement(id)] {
            try {
                floating_.rehang(id, [&] {
                    write(L"return", [&] { store_.rehang_at(id, target.rope.id, target.index, &expected); });
                    return true;
                });
            }
            catch (...) { log("return " + id + " failed"); rollback_placement(id, epoch); throw; }
            workspace_->clear_floating_preview(false);
            log("return " + id + " committed");
            committed();
            workspace_->settle_floating_drop(id, target);
            });
        };
        actions.close_card = [this](const auto& id) {
            if (!floating_.active().contains(id)) return;
            preparation_.cancel_source(floating_.active().at(id)->window());
            const bool recycle = card_preferences_.recycle_floating;
            auto remove = [this, id, recycle] { remove_card(id, std::nullopt, recycle); };
            if (inline_editor_.entry() == id) inline_editor_.save(std::move(remove));
            else remove();
        };
        actions.moved = [this](const auto& id, const auto& layout) {
            after_admission([this, id, layout, epoch = pending_placement(id)] {
            try { store_.save_layout(id, layout); }
            catch (...) { log("move-floating " + id + " failed"); rollback_placement(id, epoch); throw; }
            log("move-floating " + id + " committed");
            report_float(id);
            });
        };
        actions.context = [this](application::CardMenuSnapshot card, POINT point) {
            auto surface = floating_.active().at(card.entry.id)->window();
            win::menu_mark(win::MenuStage::focus);
            editor_->menu(surface, point, std::move(card));
        };
        actions.error = [this] { error(); };
        return actions;
    }

    void create_float(application::CardSummary card, const domain::Layout& layout, bool show_window) {
        floating_.open(std::move(card), layout, show_window, floating_actions());
    }

    void expand_floating_group(const application::ExpandFileGroup& group, HWND owner) {
        std::vector<RECT> occupied;
        for (const auto& [id, floating] : floating_.active()) {
            RECT rect{};
            if (IsWindowVisible(floating->window()) && GetWindowRect(floating->window(), &rect))
                occupied.push_back(rect);
        }
        const bool images = card_preferences_.image_file_previews && std::any_of(group.members.begin(), group.members.end(),
            [](const auto& member) { return image_file_group({member.path}); });
        const auto layouts = win::tile_file_group(owner, std::move(occupied), group.members.size(),
                                                   browser_.image_preferences.size, images);
        std::size_t prepared = 0;
        try {
            for (const auto& member : group.members) {
#ifdef CLIPROPE_DESKTOP_PROBES
                if (prepared == 1 && options_.fault == L"expand-window") {
                    options_.fault.clear();
                    throw std::runtime_error("Injected Floating creation failure");
                }
#endif
                application::CardSummary card;
                card.entry.id = member.id;
                card.kind = application::CardKind::files;
                card.text = presentation::leaf(member.path);
                create_float(std::move(card), layouts[prepared], false);
                ++prepared;
            }
            write(L"expand", [&] { store_.expand_file_group(group, {}, layouts); });
        } catch (...) {
            while (prepared) floating_.discard_uncommitted(group.members[--prepared].id);
            throw;
        }
        if (!group.keep_source) floating_.retire(group.source.id);
        for (const auto& member : group.members) {
            auto& window = floating_.active().at(member.id);
            try { window->update(display_summary(member.id, true)); }
            catch (const std::exception&) { PostMessageW(listener_, preview_ready, 0, 0); }
            window->show();
            report_float(member.id);
        }
    }

    void expand_group(const domain::Entry& expected, HWND owner) {
        const auto group = inspect_expansion(store_.prepare_expansion(expected, card_preferences_.keep_expanded_group));
        if (expected.rope_id) {
            for (;;) {
                const auto impact = store_.preview_expansion(group);
                if ((impact.required_capacity || !impact.removed_tail.empty()) &&
                    !win::review_file_group_expansion(owner, win::unicode::wide(records_.rope(*expected.rope_id).name),
                                                      group.members.size(), impact, group.keep_source))
                    return;
                if (write(L"expand", [&] { return store_.expand_file_group(group, impact); })) break;
            }
        } else {
            expand_floating_group(group, owner);
        }
        log("expand " + expected.id + " committed");
        committed();
    }

    void create_quick_note(const domain::Layout& layout) {
        if (inline_editor_.owner()) {
            inline_editor_.finish_then([this, layout] { create_quick_note(layout); });
            return;
        }
        const auto id = store_.prepare_note_id();
        application::CardSummary blank;
        blank.entry.id = id;
        note_fault(L"note-window");
        create_float(std::move(blank), layout, false);
        try { write(L"note-commit", [&] { store_.create_floating_text(id, layout); }); }
        catch (...) { floating_.discard_uncommitted(id); throw; }
        floating_.active().at(id)->show();
        present_note(id, true);
        report_float(id);
    }

    void note_fault(std::wstring_view point) {
#ifdef CLIPROPE_DESKTOP_PROBES
        if (options_.fault == point) {
            options_.fault.clear();
            throw std::runtime_error("Injected quick note presentation failure");
        }
#else
        (void)point;
#endif
    }
    void present_note(const domain::Id& id, bool allow_retry) {
        const auto window = floating_.active().find(id);
        if (window == floating_.active().end()) return;
        try {
            note_fault(L"note-summary");
            window->second->update(display_summary(id, true));
            if (inline_editor_.owner()) return; // A later action already owns the editor.
            note_fault(L"note-editor");
            window->second->edit_text(store_.begin_text_edit(id));
        } catch (const std::exception&) {
            if (allow_retry && PostMessageW(listener_, retry_note, 0, 0))
                retry_notes_.push_back(id);
            else error(false);
        }
    }

    void restore_float(const domain::Id& id) {
        const auto layout = store_.layout(id);
        auto card = display_summary(id, layout.visible);
        const bool provisional = presentation::adaptive_image(card) && !card.source.width;
        create_float(std::move(card), layout, layout.visible);
        // A temporary placeholder may be wider than the real image near a screen edge.
        if (provisional) { report_float(id); return; }
        auto placed = floating_.active().at(id)->layout();
        placed.visible = layout.visible;
        if (placed.monitor != layout.monitor || std::abs(placed.x - layout.x) > 1.0 ||
            std::abs(placed.y - layout.y) > 1.0 || std::abs(placed.width - layout.width) > 1.0 ||
            std::abs(placed.height - layout.height) > 1.0)
            store_.save_layout(id, placed);
        report_float(id);
    }

    void reveal_hidden(const domain::Id& id) {
        if (floating_.active().contains(id)) return;
        const auto saved = store_.layout(id);
        if (saved.visible) return;
        auto card = display_summary(id, true);
        const auto bound = card.entry;
        if (bound.rope_id) return;
        const bool provisional = presentation::adaptive_image(card) && !card.source.width;
        {
#ifdef CLIPROPE_DESKTOP_PROBES
            struct CreationHook { HHOOK handle{}; ~CreationHook() { if (handle) UnhookWindowsHookEx(handle); } } hook;
            if (options_.fault == L"reveal-window") {
                options_.fault.clear();
                hook.handle = SetWindowsHookExW(WH_CBT, [](int code, WPARAM w, LPARAM l) -> LRESULT {
                    return code == HCBT_CREATEWND ? 1 : CallNextHookEx(nullptr, code, w, l);
                }, nullptr, GetCurrentThreadId());
                if (!hook.handle) throw std::runtime_error("Cannot install Floating creation fault");
            }
#endif
            create_float(std::move(card), saved, false);
        }
        auto& window = *floating_.active().at(id);
        try {
            auto placed = provisional ? saved : window.layout();
            placed.visible = true;
            store_.reveal_floating(bound, saved, placed);
        } catch (...) { floating_.discard_uncommitted(id); throw; }
        window.show();
        report_float(id);
    }

    template<class F> std::invoke_result_t<F> write(std::wstring_view fault, F&& change) {
#ifdef CLIPROPE_DESKTOP_PROBES
        const bool injecting = options_.fault == fault;
        if (injecting) { options_.fault.clear(); fault_commit_ = true; }
        struct Reset {
            bool& armed;
            bool injecting;
            ~Reset() { if (injecting) armed = false; }
        } reset{fault_commit_, injecting};
#else
        (void)fault;
#endif
        return change();
    }

    void detach(const domain::Id& id, POINT point, float x, float y) {
        if (!bound_ || bound_->entry.id != id) return;
        auto press = std::move(*bound_);
        bound_.reset();
        auto card = display_summary(id, true, workspace_->picture_width({}));
        const auto layout = win::floating_layout_at(point, x, y, &card);
        card.entry.detach();
        after_admission([this, id, card = std::move(card), layout, expected = press.entry]() mutable {
        try {
            floating_.detach(std::move(card), layout, floating_actions(),
                [&] { write(L"detach", [&] { store_.detach(id, layout, &expected); }); });
        } catch (...) { log("detach " + id + " failed"); throw; }
        log("detach " + id + " committed");
        report_float(id);
        committed();
        });
    }

    void return_float(const domain::Id& id, const domain::Id& rope) {
        if (!floating_.active().contains(id)) return;
        preparation_.cancel_source(floating_.active().at(id)->window());
        const auto target = rope;
        if (inline_editor_.entry() == id) {
            inline_editor_.save([this, id, target] { return_float(id, target); });
            return;
        }
        auto command = application::bind_invocation(store_, Route::return_button, id, target);
#ifdef CLIPROPE_DESKTOP_PROBES
        if (options_.defer_floating) {
            if (deferred_) throw std::runtime_error("Another return is pending");
            deferred_.emplace(std::move(command));
            log("pending-floating " + id);
            return;
        }
#endif
        auto bound = std::make_shared<application::Invocation>(std::move(command));
        after_admission([this, bound, id] { commit_return(*bound, id); });
    }

    void commit_return(application::Invocation& command, const domain::Id& id) {
        application::InvocationResult result;
        if (!floating_.rehang(id, [&] {
                result = write(L"return", [&] { return command.run(store_, {}); });
                return result.history == application::HistoryEffect::committed;
            })) {
            diagnostics_.history(application::Diagnostics::Stage::command, result.issue);
            log("return " + id + " failed");
            error();
            return;
        }
        log("return " + id + " committed");
        committed();
    }

#ifdef CLIPROPE_DESKTOP_PROBES
    // The fixture pauses only after a real control has bound its target. A
    // real collector switch resumes that exact one-shot command on the UI STA.
    static constexpr UINT resume_return = WM_APP + 210;
    void resume_floating() {
        if (!deferred_) return;
        auto command = std::move(*deferred_);
        deferred_.reset();
        commit_return(command, command.bound_entry().id);
    }
#endif

    void remove_card(const domain::Id& id, const std::optional<domain::Id>& source, bool recycle) {
        const std::string operation = recycle ? "recycle " : "delete ";
        const auto entry = store_.entry(id);
        if (entry.rope_id != source) throw std::runtime_error("Card membership changed");
        after_admission([this, id, source, entry, recycle, operation] {
          try {
            write(recycle ? L"recycle" : L"delete", [&] {
                if (!recycle) store_.erase(id, &entry);
                else if (source) store_.move(id, domain::rebin_id, &entry);
                else store_.rehang(id, domain::rebin_id, &entry);
            });
        } catch (...) { log(operation + id + " failed"); throw; }
        if (!source) floating_.retire(id);
        log(operation + id + " committed");
        committed();
        });
    }

    void report_float(const domain::Id& id) {
#ifdef CLIPROPE_DESKTOP_PROBES
        if (!report_.is_open()) return;
        RECT r{};
        if (GetWindowRect(floating_.active().at(id)->window(), &r)) {
            std::ostringstream line;
            line << "floating " << id << ' ' << r.left << ' ' << r.top << ' ' << r.right << ' ' << r.bottom;
            log(line.str());
        }
#else
        (void)id;
#endif
    }

    void apply_card_preferences(const application::CardPreferences& value) {
        if (value.follow_card_head != card_preferences_.follow_card_head ||
            value.follow_collector != card_preferences_.follow_collector) {
            std::lock_guard lock(head_mutex_);
            if (pending_head_ && pending_head_->change.follows(value) != pending_head_->change.follows(card_preferences_))
                pending_head_.reset();
        }
        if(screenshot_hover_) screenshot_hover_->enabled(value.screenshot_hover_preview);
        const bool redraw = value.font_size != card_preferences_.font_size ||
            value.hover_resolution != card_preferences_.hover_resolution ||
            value.image_file_previews != card_preferences_.image_file_previews;
        card_preferences_ = value;
        if (redraw) {
            try {
                if (inline_editor_.owner())
                    inline_editor_.font_size(display_summary(inline_editor_.entry()).style.font_size);
                workspace_->refresh();
                PostMessageW(listener_, preview_ready, 0, 0);
            } catch (const std::exception&) { error(); }
        }
    }
    void prepare_startup() {
        startup_->initialize();
        auto& db = store_.database();
        if (persistence::pending_sign_in_rollback(db)) {
            startup_->set(false);
            persistence::mark_sign_in_rollback(db, false);
        }
    }
    void reset_settings() {
        auto& db = store_.database();
        const bool previous_topmost = persistence::read_workspace_topmost(db);
        bool restore_startup = false;
        if (startup_) {
            prepare_startup();
            restore_startup = !startup_->enabled();
            if (restore_startup) persistence::mark_sign_in_rollback(db, true);
        }
        try {
            if (restore_startup) startup_->set(true);
            workspace_->always_on_top(true);
            persistence::reset_global_preferences(db, restore_startup);
        } catch (...) {
            workspace_->always_on_top(previous_topmost);
            if (restore_startup) prepare_startup();
            throw;
        }
        appearance_ = persistence::read_appearance(db);
        win::fluent::set_theme(appearance_.theme);
        workspace_->match_color(appearance_.match_rgb());
        apply_card_preferences(persistence::read_card_preferences(db));
        tray_->reload_shortcuts();
        apply_language(persistence::read_language(db));
    }
    void apply_language(localization::Language selected) {
        if (language_.set_language(selected, win::preferred_ui_locale())) win::notify_language_changed();
    }
    win::TrayActions tray_actions() {
        win::TrayActions actions;
        actions.toggle = [this] { if (show_after_bind_ || workspace_->visible()) hide(); else show(); };
        actions.open_workspace = [this] { if (!workspace_->visible()) show(); };
        actions.create_quick_note = [this] {
            POINT pointer{};
            if (!GetCursorPos(&pointer)) {
                RECT bounds{};
                if (!GetWindowRect(workspace_->window(), &bounds)) throw std::runtime_error("Cannot locate the quick note monitor");
                pointer = {(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
            }
            const auto layout = win::centered_text_layout(pointer);
            after_admission([this, layout] { create_quick_note(layout); });
        };
        actions.menu = [this] {
            const auto rope = store_.active_rope();
            win::TrayModel result;
            result.rope_id = rope;
            auto hidden = records_.floating_ids(false, 1, last_reveal_attempt_);
            if (hidden.empty() && !last_reveal_attempt_.empty()) {
                last_reveal_attempt_.clear();
                hidden = records_.floating_ids(false, 1);
            }
            if (!hidden.empty()) result.hidden_card = hidden.front();
            result.rope = win::unicode::wide(records_.rope(rope).name);
            for (auto& summary : display_page(rope, 0, 5))
                result.items.push_back({std::move(summary.entry.id), std::move(summary.text)});
            return result;
        };
        actions.recall = [this](const auto& id, const auto& rope) { recall(id, Route::tray, rope); };
        actions.reveal_hidden = [this](const auto& id) {
            after_admission([this, id] {
                last_reveal_attempt_ = id;
                try { reveal_hidden(id); }
                catch (const std::exception&) {
                    log("reveal-floating " + id + " failed");
                    error();
                    tray_->notify(win::tr(localization::TextKey::error_floating_restore));
                }
            });
        };
        actions.head = [this](const auto& id, const auto& rope) { recall(id, Route::rope_head, rope); };
        actions.exit = [this] { PostMessageW(listener_, WM_CLOSE, 0, 0); };
        actions.read_shortcuts = [this] { return persistence::read_shortcuts(store_.database()); };
        actions.save_shortcut = [this](auto action, const auto& binding) {
            persistence::save_shortcut(store_.database(), action, binding);
        };
        actions.set_scoped = [this](const auto& keys) { workspace_->shortcuts(keys); };
        actions.hover_status = [this] { return workspace_->hover_keyboard_status(); };
        actions.snapshot = [this] {
            win::SettingsSnapshot snapshot;
            snapshot.defer_history = [this](auto command) { return defer_history(std::move(command)); };
            snapshot.refresh_settings = [this](win::SettingsSnapshot& values) {
                auto& db = store_.database();
                values.language = persistence::read_language(db);
                values.topmost = persistence::read_workspace_topmost(db);
                values.cards = card_preferences_;
                values.appearance = appearance_;
                values.default_capacity = store_.default_capacity();
                values.launch_at_sign_in.reset();
                values.startup_notice.clear();
                if (startup_) {
                    try {
                        prepare_startup();
                        values.launch_at_sign_in = startup_->enabled();
                    } catch (const std::exception& error) { values.startup_notice = startup_failure(error); }
                } else values.startup_notice = win::tr(localization::TextKey::settings_launch_isolated);
            };
            if (startup_) snapshot.save_startup = [this](bool on) { prepare_startup(); startup_->set(on); };
            snapshot.save_topmost = [this](bool on) {
                auto& db = store_.database();
                const bool previous = persistence::read_workspace_topmost(db);
                workspace_->always_on_top(on);
                try { persistence::save_workspace_topmost(db, on); }
                catch (...) { workspace_->always_on_top(previous); throw; }
            };
            snapshot.read_exclusions = [this] { return persistence::read_privacy(store_.database()).excluded_apps; };
            snapshot.save_exclusions = [this](const auto& original, auto names) {
                policy_ = persistence::save_exclusions(store_.database(), original, std::move(names));
                policy_damaged_ = false;
            };
            snapshot.reset_settings = [this] { reset_settings(); };
            snapshot.language = language_.selected_language();
            snapshot.save_language = [this](localization::Language selected) {
                persistence::save_language(store_.database(), selected);
                apply_language(selected);
            };
            snapshot.appearance = appearance_;
            snapshot.preview_match = [this](std::optional<domain::ColorSelection> color) {
                workspace_->match_color(color ? color->resolve(domain::match_default).rgb() : appearance_.match_rgb());
            };
            snapshot.save_match = [this](domain::ColorSelection original, domain::ColorSelection color) {
                try { persistence::save_match_color(store_.database(), original, color); }
                catch (...) {
                    workspace_->match_color(appearance_.match_rgb());
                    throw;
                }
                appearance_.match_color = color;
                workspace_->match_color(appearance_.match_rgb());
            };
            snapshot.save_appearance = [this](application::Appearance value) {
                persistence::save_appearance(store_.database(), value);
                appearance_ = value;
                win::fluent::set_theme(value.theme);
                workspace_->match_color(value.match_rgb());
            };
            snapshot.profile = store_.database().root();
            snapshot.measure = [root = snapshot.profile] { return persistence::profile_disk_usage(root); };
            snapshot.location_notice = options_.location_notice;
            if (options_.location) {
                snapshot.pending = options_.location->pending();
                snapshot.schedule = [this](const fs::path& target) {
                    options_.location->schedule(target);
                    options_.location_notice.clear();
                };
                snapshot.restart = [this] {
                    inline_editor_.finish_then([this] {
                        restarting_ = true;
                        if (!capture_.busy()) PostMessageW(listener_, WM_CLOSE, 0, 0);
                    });
                };
            }
            snapshot.cards = card_preferences_;
            snapshot.save_cards = [this](const application::CardPreferences& value) {
                persistence::save_card_preferences(store_.database(), value);
                apply_card_preferences(value);
            };
            snapshot.save_capacity = [this](int capacity) {
                application::RopeChange change{application::RopeOp::defaults};
                change.capacity = capacity;
                store_.apply_rope_change(store_.preview_rope_change(change));
            };
            snapshot.default_capacity = store_.default_capacity();
            return snapshot;
        };
        return actions;
    }

    void recall(const domain::Id& id, Route route, const std::optional<domain::Id>& rope = {}) {
        if (inline_editor_.entry() == id && inline_editor_.previewing()) {
            inline_editor_.save([this, id, route, rope] { recall(id, route, rope); });
            return;
        }
        auto command = application::bind_invocation(store_, route, id);
        if (rope && command.bound_entry().rope_id != rope)
            throw std::runtime_error("The selected entry has left its rope");
        auto bound = std::make_shared<application::Invocation>(std::move(command));
        after_admission([this, bound, route, id] { invoke(*bound, route, id); });
    }
    void supersede_target() {
        recall_session_.cancel();
        progress_target();
    }
    void progress_target() {
        if (show_after_bind_ && recall_session_.opening_ready()) finish_show();
        if (auto press = recall_session_.take_bound()) {
            const auto id = press->entry.id;
            paste_hanging(std::move(*press), id);
        }
        recall_session_.poll(clipboard_, [this](const auto& pending) {
            report_invocation(pending.id, pending.route, pending.result, pending.target.get());
        });
        if (!show_after_bind_ && !recall_session_.pending()) KillTimer(listener_, target_timer);
    }
    void invoke(application::Invocation& command, Route route, const std::string& id, std::shared_ptr<win::EditGesture> target = {},
                std::optional<DWORD> confirmed_sequence = {}) {
        if (exit_after_admission_) return;
        if (!store_.has_external_payload(command.bound_entry().snapshot_id)) {
            invoke_ready(command, route, id, std::move(target), confirmed_sequence);
            return;
        }
        const auto sequence = GetClipboardSequenceNumber();
        auto expected = confirmed_sequence ? confirmed_sequence : order_.sequence();
        if (expected && *expected != sequence && !clipboard_.owns_current()) {
            report_invocation(id, route, {application::Publication::failed}, target.get());
            return;
        }
        auto bound = std::make_shared<application::Invocation>(std::move(command));
        const bool for_folder = target && target->bound_folder();
        preparation_.start(listener_, [this, snapshot = bound->bound_entry().snapshot_id, for_folder](auto stop) {
            if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) throw std::runtime_error("Recall apartment unavailable");
            struct Apartment { ~Apartment() { CoUninitialize(); } } apartment;
            host::CardPreparation::Result result;
            result.snapshot = store_.read_for_display(snapshot, stop);
            if (stop.stop_requested()) throw std::runtime_error("Recall preparation cancelled");
            result.offer = win::HistoryClipboard::prepare(result.snapshot.content.snapshot, for_folder);
            if (!result.offer) throw std::runtime_error("No restorable representations");
            result.snapshot.content.snapshot = {};
            return result;
        }, [this, bound, route, id, target = std::move(target), sequence](auto ready) {
            host::CardPreparation::Result prepared;
            try { prepared = ready.get(); }
            catch (const std::exception& error) {
                application::InvocationResult failed{application::Publication::failed};
                failed.issue = application::history_issue(error);
                report_invocation(id, route, failed, target.get());
                return;
            }
            if (GetClipboardSequenceNumber() != sequence || exit_after_admission_) {
                report_invocation(id, route, {application::Publication::failed}, target.get());
                return;
            }
            invoke_ready(*bound, route, id, target, sequence, &prepared);
        });
    }
    void invoke_ready(application::Invocation& command, Route route, const std::string& id,
                      std::shared_ptr<win::EditGesture> target, std::optional<DWORD> confirmed_sequence,
                      host::CardPreparation::Result* prepared = nullptr) {
        using namespace application;
        host::RecallSession::Active active(recall_session_, target.get());
        PublicationId pending_receipt{};
        auto result = command.run(store_, [&](const auto& content) -> PublicationId {
            auto expected = confirmed_sequence ? confirmed_sequence : order_.sequence();
            if (expected && *expected != GetClipboardSequenceNumber()) {
                if (prepared || !clipboard_.owns_current()) {
                    log("invoke superseded " + id);
                    return false;
                }
                expected = GetClipboardSequenceNumber();
            }
#ifdef CLIPROPE_DESKTOP_PROBES
            if (route == Route::double_click && options_.fault == L"publish") {
                options_.fault.clear();
                return false;
            }
#endif
            const auto receipt = prepared ? clipboard_.publish_prepared(std::move(*prepared->offer), content.lease, expected)
                                          : clipboard_.publish(content, target && target->bound_folder(), expected);
#ifdef CLIPROPE_DESKTOP_PROBES
            if (receipt && options_.fault == L"history") { options_.fault.clear(); fault_commit_ = true; }
#endif
            return receipt;
        }, [&](const auto& content) {
#ifdef CLIPROPE_DESKTOP_PROBES
            if (options_.fault == L"replace-before-delivery") {
                options_.fault.clear();
                const HWND published_owner = GetClipboardOwner();
                log("probe awaiting clipboard replacement");
                const auto deadline = GetTickCount64() + 1500;
                while (GetClipboardOwner() == published_owner && GetTickCount64() < deadline) {
                    MSG sent{};
                    PeekMessageW(&sent, nullptr, WM_QUIT, WM_QUIT, PM_NOREMOVE);
                    Sleep(2);
                }
                if (clipboard_.owns_current()) throw std::runtime_error("Fixture did not replace the publication");
            }
#endif
            if (!target) return Delivery::no_field;
            if (auto delivery = clipboard_.poll_delivery(*target, content)) return *delivery;
            pending_receipt = content;
            return Delivery::skipped;
        }, [&](PublicationId receipt) {
            // A hidden foreground window can transfer focus to an unrelated app.
            if (target && clipboard_.owns_publication(receipt)) target->restore_interrupted_focus();
            try {
                if (route == Route::double_click) hide();
                committed();
            } catch (const std::exception&) { error(); }
        }, prepared ? &prepared->snapshot : nullptr);
#ifdef CLIPROPE_DESKTOP_PROBES
        fault_commit_ = false;
#endif
        if (pending_receipt) {
            if (recall_session_.wait_for_delivery(listener_, {target, pending_receipt, result, id, route})) return;
            result.delivery = Delivery::unconfirmed;
        }
        report_invocation(id, route, result, target.get());
    }
    void report_invocation(const domain::Id& id, Route route, const application::InvocationResult& result,
                           win::EditGesture* target) {
        using namespace application;
        diagnostics_.history(Diagnostics::Stage::command, result.issue);
        const bool timeout = target && target->provider_timeout();
        if (timeout) ++diagnostics_.target_timeouts;
        if (timeout || result.publication == Publication::failed || result.history == HistoryEffect::failed)
            ++diagnostics_.failures;
#ifdef CLIPROPE_DESKTOP_PROBES
        if (target) {
            log("focus-target " + target->target_status());
            log(std::string("focus-restore ") + target->restore_status());
            log(std::string("file-target ") + target->file_target_status());
        }
#endif
        static constexpr const char* publication_names[] = {"skipped", "failed", "published"};
        static constexpr const char* history_names[] = {"skipped", "failed", "committed"};
        const char* route_name = route == Route::double_click ? "double-click" : (route == Route::tray ? "tray" : "head");
        log("invoke " + std::string(route_name) + " " + id + " " + publication_names[static_cast<int>(result.publication)] +
            " " + delivery_name(result.delivery) + " " + history_names[static_cast<int>(result.history)]);
        if (result.publication == Publication::failed) {
            browser_.recheck_files();
            PostMessageW(listener_, preview_ready, 0, 0);
            tray_->notify(win::tr(localization::TextKey::clipboard_publication_failed));
            return;
        }
        win::UiText notice;
        if (result.delivery == Delivery::unconfirmed) notice = win::tr(localization::TextKey::clipboard_dispatch_uncertain);
        else if (result.delivery != Delivery::skipped && result.delivery != Delivery::shortcut_sent && result.delivery != Delivery::no_field)
            notice = win::tr(localization::TextKey::clipboard_target_failed);
        if (result.history == HistoryEffect::failed) {
            if (notice.empty()) notice = result.delivery == Delivery::shortcut_sent ? win::tr(localization::TextKey::clipboard_paste_sent) : win::tr(localization::TextKey::clipboard_copied);
            notice += win::tr(localization::TextKey::clipboard_history_failed);
        }
        if (!notice.empty()) tray_->notify(notice);
    }

    void check_fault(std::string_view boundary) {
#ifdef CLIPROPE_DESKTOP_PROBES
        if (boundary == "snapshot-read" && slow_snapshot_.exchange(false)) {
            const auto name = L"Local\\ClipRope.TestSnapshotRead." + std::to_wstring(GetCurrentProcessId());
            if (const auto event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str())) {
                SetEvent(event);
                CloseHandle(event);
            }
            Sleep(200);
        }
        if (boundary == "gc-scan-start" && probe_gc_.exchange(false)) {
            PostMessageW(listener_, win::gc_probe_read, 0, 0);
            Sleep(150);
        }
        if (GetCurrentThreadId() != ui_thread_ && boundary == "transaction" && fail_capture_commit_.exchange(false))
            throw persistence::StorageFailure(persistence::StorageProblem::full);
        if (GetCurrentThreadId() == ui_thread_ && fault_commit_ && boundary == "transaction") {
            fault_commit_ = false;
            throw std::runtime_error("Synthetic history failure");
        }
#else
        (void)boundary;
#endif
    }

    void start_find_worker() {
        if (!find_worker_)
            find_worker_ = std::make_unique<FindWorker>(
                options_.profile, [window = listener_] { PostMessageW(window,  find_ready,  0,  0); });
    }
    win::UiText find_label(const domain::Id& matched = {}) {
        if (!workspace_->in_overview()) return win::message(localization::TextKey::find_scope_rope, {{"rope", win::unicode::wide(records_.rope(store_.active_rope()).name)}});
        return matched.empty() ? win::tr(localization::TextKey::rope_overview_all) : win::message(localization::TextKey::find_scope_all, {{"rope", win::unicode::wide(records_.rope(matched).name)}});
    }
    void refresh_find(bool retain) {
        if (!find_.visible()) return;
        ++find_version_;
        find_worker_->cancel();
        KillTimer(listener_, find_timer);
        if (!retain) {
            find_index_ = 0;
            find_hit_.reset();
            workspace_->find_highlights({});
        }
        find_total_ = 0;
        find_.status(find_term_.empty() ? L"" : win::tr(localization::TextKey::find_searching),
                     find_label(), false);
        if (!find_term_.empty()) SetTimer(listener_, find_timer, 150, nullptr);
        else find_worker_->submit({});
    }
    void submit_find(bool retain) {
        if (find_.visible() && !find_term_.empty())
            find_worker_->submit(
                {workspace_->find_scope(), find_term_, find_index_, find_version_, retain ? find_hit_ : std::nullopt});
    }
    void accept_find() {
        if (!find_worker_ || !find_.visible()) return;
        auto reply = find_worker_->take();
        if (!reply) return;
        find_hit_ = std::move(reply->hit);
        find_total_ = reply->total;
        find_index_ = reply->index;
        if (reply->error != FindWorker::Error::none) {
            workspace_->find_highlights({});
            const auto key = reply->error == FindWorker::Error::unsupported_format
                ? localization::TextKey::error_storage_unsupported_format : localization::TextKey::hotkey_check_unavailable;
            find_.status(win::tr(key), find_label(), false);
            return;
        }
        if (find_total_ && (!find_hit_ || !workspace_->reveal_match(*find_hit_))) {
            refresh_find(true);
            return;
        }
        workspace_->find_highlights(find_total_ ? find_term_ : L"", find_hit_);
        find_.status(std::to_wstring(find_total_ ? find_index_ + 1 : 0) + L" / " + std::to_wstring(find_total_),
                     find_label(find_hit_ ? find_hit_->rope : domain::Id{}), find_total_ != 0);
    }
    void committed() {
        if(screenshot_hover_) screenshot_hover_->cancel(true);
        workspace_->refresh();
        if (editor_) editor_->refresh_information();
        collect_after_mutation();
    }
    void collect_after_mutation() {
        refresh_find(true);
        if (++mutations_ < 32) return;
        if (gc_job_.valid()) {
            if (gc_job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
            try { diagnostics_.cleanup_deferred = gc_job_.get().deferred; } catch (const std::exception&) { error(); }
        }
        gc_job_ = std::async(std::launch::async, [this] { return store_.gc(); });
        mutations_ = 0;
    }
    void show() {
        if (show_after_bind_) return;
        recall_session_.remember_editor(workspace_->window());
        if (!recall_session_.opening_ready()) {
            if (SetTimer(listener_, target_timer, 10, nullptr)) { show_after_bind_ = true; return; }
            recall_session_.cancel_opening();
        }
        finish_show();
    }
    void finish_show() {
        show_after_bind_ = false;
        browser_.recheck_files();
        workspace_->show(store_.active_rope());
        if (GetForegroundWindow() != workspace_->window()) recall_session_.forget_editor();
        log("workspace visible");
    }
    void hide() {
        show_after_bind_ = false;
        preparation_.cancel_source(workspace_->window());
        if (inline_editor_.owner() == workspace_->window()) {
            inline_editor_.save([this] { hide(); });
            return;
        }
        recall_session_.forget_editor();
        bound_.reset();
        find_.close(false);
        workspace_->hide();
        log("workspace hidden");
    }
    void error(bool count = true) {
        if (count) ++diagnostics_.failures;
        log("command failed");
        if (tray_) tray_->notify(win::tr(localization::TextKey::error_history_action));
    }

    void record_power_message([[maybe_unused]] const char* kind) {
#ifdef CLIPROPE_DESKTOP_PROBES
        if (!report_.is_open()) return;
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        const auto ticks = (static_cast<std::uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        log(std::string("power ") + kind + " " + std::to_string(ticks));
#endif
    }
    void log(const std::string& message) {
#ifdef CLIPROPE_DESKTOP_PROBES
        if (report_.is_open()) report_ << message << std::endl;
#else
        (void)message;
#endif
    }
    void frame() {
#ifdef CLIPROPE_DESKTOP_PROBES
        if (!report_.is_open() || !workspace_) return;
        const auto slots = workspace_->slots();
        std::ostringstream block;
        block << "frame " << ++frame_ << ' ' << slots.size() << '\n';
        for (const auto& [id, rect] : workspace_->rows())
            block << "row " << id << ' ' << rect.left << ' ' << rect.top << ' ' << rect.right << ' ' << rect.bottom << '\n';
        for (const auto& [id, rect] : slots)
            block << "slot " << id << ' ' << rect.left << ' ' << rect.top << ' ' << rect.right << ' ' << rect.bottom << '\n';
        block << "endframe " << frame_;
        log(block.str());
#endif
    }

    LRESULT message(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
        if (tray_) {
            if (auto handled = tray_->message(msg, w, l)) return *handled;
        }
        switch (msg) {
        case retry_note:
            if (!retry_notes_.empty()) {
                auto id = std::move(retry_notes_.front());
                retry_notes_.pop_front();
                if (!exit_after_admission_) present_note(id, false);
            }
            return 0;
        case snip_bound:
            refresh_find(true);
            return 0;
        case head_change_ready:
            finish_head_change();
            return 0;
        case win::ExternalDragSession::ready_message: {
            auto result = external_drag_.finish(store_, w);
            if (!result.empty()) log(std::string("external-drag ") + std::string(result));
            return 0;
        }
        case WM_CAPTURECHANGED:
        case WM_LBUTTONUP:
            if (external_drag_.staging() && external_drag_.cancel())
                log(msg == WM_LBUTTONUP ? "external-drag released" : "external-drag owner-replaced");
            break;
        case WM_HOTKEY:
            if (w == win::ExternalDragSession::escape_hotkey) {
                if (external_drag_.cancel()) log("external-drag canceled");
                return 0;
            }
            break;
        case WM_DEVICECHANGE:
            if (w == DBT_DEVNODES_CHANGED || w == DBT_DEVICEARRIVAL || w == DBT_DEVICEREMOVECOMPLETE) {
                browser_.recheck_files();
                PostMessageW(hwnd, preview_ready, 0, 0);
            }
            break;
        case WM_POWERBROADCAST:
            if (w == PBT_APMSUSPEND) {
                record_power_message("suspend");
                cancel_transient_input();
            }
            if (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND) {
                record_power_message(w == PBT_APMRESUMEAUTOMATIC ? "resume-automatic" : "resume-user");
                recover_desktop_views();
            }
            return TRUE;
        case WM_WTSSESSION_CHANGE:
            if (static_cast<DWORD>(l) != session_id_)
                return 0;
            if (w == WTS_SESSION_LOCK || w == WTS_REMOTE_DISCONNECT || w == WTS_CONSOLE_DISCONNECT)
                cancel_transient_input();
            if (w == WTS_SESSION_UNLOCK || w == WTS_REMOTE_CONNECT || w == WTS_CONSOLE_CONNECT)
                recover_desktop_views();
            return 0;
        case WM_FONTCHANGE:
            win::invalidate_ui_fonts();
            win::fluent::Surface::clear_control_artwork();
            break;
        case win::language_changed:
            win::fluent::Surface::clear_control_artwork();
            rendering::WorkspacePaint::use_localization(&language_, win::ui_font_family());
            if (workspace_) workspace_->retranslate();
            return 0;
        case win::screenshot_preview_ready:
            if(screenshot_hover_) screenshot_hover_->ready();
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_DWMCOMPOSITIONCHANGED:
            win::fluent::Surface::clear_control_artwork();
            recover_desktop_views();
            return 0;
        case WM_SETTINGCHANGE:
            if (language_.selected_language() == localization::Language::automatic)
                apply_language(localization::Language::automatic);
            win::fluent::Surface::clear_control_artwork();
            if (w == SPI_SETWORKAREA)
                recover_desktop_views();
            break;
#ifdef CLIPROPE_DESKTOP_PROBES
        case WM_NULL:
            if (report_.is_open() && w) {
                LARGE_INTEGER tick{};
                QueryPerformanceCounter(&tick);
                log("heartbeat " + std::to_string(w) + " " + std::to_string(tick.QuadPart));
            }
            return 0;
        case resume_return:
            resume_floating();
            return 0;
#endif
#ifdef CLIPROPE_DESKTOP_PROBES
        case win::gc_probe_start:
            if (options_.fault == L"gc-probe") {
                probe_gc_ = true;
                mutations_ = 31;
                collect_after_mutation();
            }
            return 0;
        case win::gc_probe_read: {
            const auto name = L"Local\\ClipRope.GCPage." + std::to_wstring(GetCurrentProcessId());
            if (HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str())) {
                SetEvent(event);
                CloseHandle(event);
            }
            browser_.page(store_.active_rope(), 0, 12);
            return 0;
        }
#endif
        case win::performance_query:
            if (!workspace_) return 0;
            switch(static_cast<win::PerformanceCounter>(w)){
#ifdef CLIPROPE_DESKTOP_PROBES
            case win::PerformanceCounter::preview_notices: return preview_notices_.load();
            case win::PerformanceCounter::preview_batches: return preview_batches_;
            case win::PerformanceCounter::floating_summary_reads: return floating_reads_;
            case win::PerformanceCounter::floating_frames:
            case win::PerformanceCounter::visible_file_consumers: {
                std::uint64_t value{};
                for (const auto& [id, f] : floating_.active()) {
                    if (w == WPARAM(win::PerformanceCounter::floating_frames)) value += f->presented_frames();
                    else if ((GetWindowLongPtrW(f->window(), GWL_STYLE) & WS_VISIBLE) && f->shows_file_pixels(static_cast<std::uint64_t>(l))) ++value;
                }
                return value;
            }
#endif
            case win::PerformanceCounter::first_card_point:
            case win::PerformanceCounter::first_rope_point: {
                auto regions = w == static_cast<WPARAM>(win::PerformanceCounter::first_card_point)
                    ? workspace_->slots() : workspace_->rows();
                if (regions.empty()) return 0;
                const auto& r = regions.front().second;
                return MAKELONG((r.left + r.right) / 2, (r.top + r.bottom) / 2);
            }
            case win::PerformanceCounter::visible_cards: return workspace_->slots().size();
            case win::PerformanceCounter::preview_decodes: return rendering::preview_decodes();
            case win::PerformanceCounter::thumbnail_bytes: return browser_.thumbnail_bytes();
            case win::PerformanceCounter::preview_bitmap_bytes: return rendering::WorkspacePaint::preview_bitmap_bytes();
            case win::PerformanceCounter::moving_face_bytes: return rendering::WorkspacePaint::moving_face_bytes();
            case win::PerformanceCounter::bitmap_bytes: return rendering::WorkspacePaint::resident_bitmap_bytes();
            case win::PerformanceCounter::painted_frames: return workspace_->painted_frames();
            case win::PerformanceCounter::compositor_fence: return SUCCEEDED(DwmFlush());
            case win::PerformanceCounter::render_target_creations: return rendering::WorkspacePaint::target_creations();
            case win::PerformanceCounter::clipboard_events: return clipboard_events_;
            case win::PerformanceCounter::clipboard_sequence: return clipboard_sequence_;
            case win::PerformanceCounter::capture_observed_sequence: return capture_.health().observed_sequence;
            case win::PerformanceCounter::capture_observation: return capture_.health().observation;
            case win::PerformanceCounter::capture_writing: return capture_.writing();
            case win::PerformanceCounter::admission_peak_bytes: return capture_.peak_pending_bytes();
            case win::PerformanceCounter::capture_completed_sequence: return capture_.health().completion.sequence;
            case win::PerformanceCounter::capture_outcome: return capture_.health().completion.outcome;
            case win::PerformanceCounter::capture_worker_exit: return capture_.health().completion.worker_exit;
            case win::PerformanceCounter::capture_hresult: return capture_.health().completion.hresult;
            case win::PerformanceCounter::capture_format_count: return capture_.health().completion.format_count;
            case win::PerformanceCounter::capture_saved_count: return capture_.health().completion.saved_count;
            case win::PerformanceCounter::capture_retries: return capture_.health().completion.retries;
            case win::PerformanceCounter::capture_unicode: return capture_.health().completion.unicode;
            case win::PerformanceCounter::capture_job_peak_kib: return capture_.health().completion.job_peak_kib;
            case win::PerformanceCounter::capture_reacquisitions: return capture_.health().completion.oom.reacquisitions;
            case win::PerformanceCounter::capture_recovery_guard: return static_cast<LRESULT>(capture_.health().completion.oom.guard);
            case win::PerformanceCounter::capture_monitor_query: return capture_.health().completion.oom.monitor_query;
            case win::PerformanceCounter::capture_history_query: return capture_.health().completion.oom.history_query;
            case win::PerformanceCounter::capture_native_first: return capture_.health().completion.oom.first_format;
            case win::PerformanceCounter::capture_native_count: return capture_.health().completion.oom.native_count;
            case win::PerformanceCounter::capture_native_error: return capture_.health().completion.oom.native_error;
            case win::PerformanceCounter::capture_user_objects: return capture_.health().completion.oom.user_objects;
            case win::PerformanceCounter::capture_gdi_objects: return capture_.health().completion.oom.gdi_objects;
            case win::PerformanceCounter::capture_worker_handles: return capture_.health().completion.oom.handles;
            case win::PerformanceCounter::screenshot_requests: return screenshot_hover_ ? screenshot_hover_->counters().requests : 0;
            case win::PerformanceCounter::screenshot_decodes: return screenshot_hover_ ? screenshot_hover_->counters().decodes : 0;
            case win::PerformanceCounter::screenshot_cache_bytes: return screenshot_hover_ ? screenshot_hover_->counters().bytes : 0;
            case win::PerformanceCounter::screenshot_failures: return screenshot_hover_ ? screenshot_hover_->counters().failures : 0;
            case win::PerformanceCounter::screenshot_shutdown_failures: return screenshot_hover_ ? screenshot_hover_->counters().shutdown_failures : 0;
            case win::PerformanceCounter::screenshot_termination_error: return screenshot_hover_ ? screenshot_hover_->counters().last_termination_error : 0;
            case win::PerformanceCounter::screenshot_wait_error: return screenshot_hover_ ? screenshot_hover_->counters().last_wait_error : 0;
            case win::PerformanceCounter::screenshot_exit_confirmed: return screenshot_hover_ && screenshot_hover_->counters().last_exit_confirmed;
            }
            return 0;
        case find_ready: accept_find(); return 0;
        case preview_ready: {
#ifdef CLIPROPE_DESKTOP_PROBES
            ++preview_batches_;
#endif
            bool all = w == 0;
            MSG ready{};
            while (PeekMessageW(&ready, listener_, preview_ready, preview_ready, PM_REMOVE))
                all |= ready.wParam == 0;
            auto changes = browser_.take_preview_changes();
            changes.all |= all;
            if (!changes.all && changes.blobs.empty()) return 0;
            if (editor_) editor_->refresh_information();
            if (workspace_->visible()) workspace_->preview_changed(changes);
            for (auto& [id, window] : floating_.active())
                if ((GetWindowLongPtrW(window->window(), GWL_STYLE) & WS_VISIBLE) && changes.contains(window->preview_key())) {
                    const auto limit = presentation::image_limit(browser_.image_preferences.size);
#ifdef CLIPROPE_DESKTOP_PROBES
                    ++floating_reads_;
#endif
                    const auto width = MulDiv(int(limit.width), GetDpiForWindow(window->window()), 96);
                    window->update(display_summary(id, true, std::max(1, width)));
                }
            win::PopupMenuHost::file_preview_ready();
            return 0;
        }
        case WM_CLIPBOARDUPDATE: {
            ++clipboard_events_;
            clipboard_sequence_ = GetClipboardSequenceNumber();
            if (!listening_ || exit_after_admission_ || clipboard_sequence_ == baseline_) return 0;
            baseline_ = 0;
            auto observation = capture_.notification();
            if (order_pending()) order_.observe(std::move(observation));
            else observe_clipboard(std::move(observation));
            if (capture_.busy() && !timer_) {
#ifdef CLIPROPE_DESKTOP_PROBES
                if (options_.fault != L"capture-timer")
#endif
                    timer_ = SetTimer(hwnd, capture_timer, 12, nullptr);
                if (!timer_) log("capture timer unavailable");
            }
            return 0;
        }
        case WM_TIMER:
            if (w == host::CardPreparation::ready_message) {
                return message(hwnd, host::CardPreparation::ready_message, 0, 0);
            }
            if (w == target_timer) { progress_target(); return 0; }
            if (w == win::ExternalDragSession::ready_message) {
                const auto result = external_drag_.finish(store_, drag_serial_);
                if (!result.empty()) log(std::string("external-drag ") + std::string(result));
                else if (external_drag_.cancel()) log("external-drag preparation-expired");
                return 0;
            }
            if (w == find_timer) { KillTimer(hwnd,find_timer); submit_find(true); return 0; }
            if (w == paste_binding_timer) {
                if (GetTickCount64() < paste_deadline_) return 0;
                KillTimer(hwnd, paste_binding_timer);
                if (bound_) bound_->target.reset();
                return 0;
            }
            if (w != capture_timer) break;
            [[fallthrough]];
        case capture_ready:
            for (const auto& result : capture_.poll()) {
                diagnostics_.capture(result.status, result.issue);
                log("capture " + result.status + (result.entry.empty() ? "" : " " + result.entry) + " " + result.detail);
                if (result.locate_snip) {
                    std::erase_if(snip_jobs_, [](auto& job) {
                        return job.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
                    });
                    if (snip_jobs_.size() < 2) {
                        auto entry = store_.entry(result.entry);
                        snip_jobs_.push_back(std::async(std::launch::async, [this, entry] {
                            if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
                            try { win::bind_snip_original(store_, entry, store_.load_snapshot(entry.snapshot_id).snapshot); }
                            catch (const std::exception&) { /* The item can be evicted while checking. */ }
                            CoUninitialize();
                        }));
                    }
                }
                if (!result.entry.empty()) committed();
                else if (result.status == "history-failed") {
                    closing_failure_ |= exit_after_admission_;
                    restarting_ = false;
                    error(false);
                }
            }
            progress_commands();
            if (!capture_.busy()) {
                KillTimer(hwnd, capture_timer); timer_ = 0;
                finish_head_change();
                if (restarting_ || exit_after_admission_) PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
            return 0;
        case host::CardPreparation::ready_message:
            try { preparation_.poll(); } catch (const std::exception&) { error(); }
            progress_commands();
            if (exit_after_admission_ && !preparation_.active() && !capture_.writing())
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        case WM_CLOSE:
            preparation_.cancel();
            show_after_bind_ = false;
            recall_session_.stop_opening();
            supersede_target();
            if (external_drag_.cancel()) log("external-drag quit");
            if (restarting_ && capture_.busy()) return 0;
            if (inline_editor_.owner() && options_.location && !options_.location->pending().empty()) {
                inline_editor_.finish_then([this] { PostMessageW(listener_, WM_CLOSE, 0, 0); });
                return 0;
            }
            if (inline_editor_.previewing()) {
                exit_after_edit_ = true;
                return 0;
            }
            exit_after_admission_ = true;
            order_.discard_observation();
            closing_failure_ |= !capture_.stop_observation();
            progress_commands();
            if (!preparation_.active() && !capture_.writing() && order_.empty()) PostQuitMessage(closing_failure_ ? 1 : 0);
            return 0;
        case WM_ENDSESSION:
            if (w) {
                preparation_.cancel();
                show_after_bind_ = false;
                recall_session_.stop_opening();
                recall_session_.discard();
                if (external_drag_.cancel()) log("external-drag quit");
                // The session is ending. Settle accepted writes; the queued native
                // commands have not run and can no longer dispatch to a live editor.
                log("session-ending cancelled pending commands=" + std::to_string(order_.size()));
                order_.cancel();
                PostQuitMessage(0);
            }
            return 0;
        }
        return DefWindowProcW(hwnd, msg, w, l);
    }
    void cancel_transient_input() {
        show_after_bind_ = false;
        if(screenshot_hover_) screenshot_hover_->cancel(true);
        if (external_drag_.cancel()) log("external-drag canceled");
        supersede_target();
        recall_session_.forget_editor();
        bound_.reset();
        if (workspace_)
            SendMessageW(workspace_->window(), WM_CANCELMODE, 0, 0);
        for (const auto &[id, window] : floating_.active())
            SendMessageW(window->window(), WM_CANCELMODE, 0, 0);
    }
    void recover_desktop_views() {
        if (language_.selected_language() == localization::Language::automatic)
            apply_language(localization::Language::automatic);
        cancel_transient_input();
        rendering::WorkspacePaint::discard_target();
        if (workspace_)
            workspace_->display_changed();
        for (const auto &[id, window] : floating_.active()) {
            SendMessageW(window->window(), WM_DISPLAYCHANGE, 0, 0);
            InvalidateRect(window->window(), nullptr, FALSE);
        }
    }
    static LRESULT CALLBACK listener_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
        auto* self = reinterpret_cast<Desktop*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<Desktop*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, msg, w, l);
        try { return self->message(hwnd, msg, w, l); }
        catch (const std::exception&) { self->error(); return 0; }
    }

    std::vector<std::future<void>> snip_jobs_;
    Options options_;
    localization::LocalizationManager& language_;
    const DWORD ui_thread_ = GetCurrentThreadId();
    std::mutex head_mutex_;
    std::uint64_t last_head_serial_{};
    std::optional<PendingHead> pending_head_;
#ifdef CLIPROPE_DESKTOP_PROBES
    std::ofstream report_;
    std::atomic<bool> probe_gc_{};
    std::atomic_bool slow_snapshot_{};
    std::atomic<unsigned> preview_notices_{};
    unsigned preview_batches_{}, floating_reads_{};
    bool fault_commit_{};
    std::atomic_bool fail_capture_commit_{};
    unsigned frame_{};
    std::optional<application::Invocation> deferred_;
#endif
    presentation::ColorPreview colors_;
    persistence::HistoryStore store_;
    persistence::Browser records_;
    domain::Id last_reveal_attempt_;
    rendering::CardBrowser browser_;
    ULONGLONG paste_deadline_{};
    host::RecallSession recall_session_;
    bool show_after_bind_{};
    win::HistoryClipboard clipboard_;
    win::HistoryCapture capture_;
    application::Appearance appearance_;
    application::CardPreferences card_preferences_;
    std::optional<win::LoginStartup> startup_;
    win::UiText startup_notice_;
    bool restarting_{};
    application::CapturePolicy policy_;
    application::Diagnostics diagnostics_;
    bool policy_damaged_{};
    std::optional<Press> bound_;
    win::ExternalDragSession external_drag_;
    std::uint64_t drag_serial_{};
    win::InlineEditorHost inline_editor_;
    win::FindPopup find_;
    std::unique_ptr<FindWorker> find_worker_;
    std::unique_ptr<win::ScreenshotHover> screenshot_hover_;
    std::wstring find_term_;
    std::optional<application::SearchHit> find_hit_;
    size_t find_index_{}, find_total_{};
    std::uint64_t find_version_{};
    std::unique_ptr<win::Workspace> workspace_;
    std::unique_ptr<win::Tray> tray_;
    std::unique_ptr<win::CardEditor> editor_;
    std::unique_ptr<win::RopeManager> ropes_;
    host::FloatingWindows floating_{
        [this](HWND window) {
            preparation_.cancel_source(window);
            if (inline_editor_.owner() == window) inline_editor_.discard();
        },
        [this](const domain::Id& id) {
            PostMessageW(listener_, WM_NULL, 0, 0);
            log("floating-closed " + id);
        }};
    std::deque<domain::Id> retry_notes_;
    HWND listener_{};
    UINT_PTR timer_{};
    DWORD baseline_{};
    DWORD clipboard_events_{}, clipboard_sequence_{};
    unsigned mutations_{};
    std::future<persistence::CollectionResult> gc_job_;
    bool listening_{}, exit_after_edit_{};
    HPOWERNOTIFY power_registration_{};
    bool session_registered_{};
    DWORD session_id_{};
};
} // namespace

static void report_startup_error(cliprope::win::UiText text) {
    OutputDebugStringA("ClipRope startup failed\n");
    USEROBJECTFLAGS flags{};
    DWORD size{};
    if (GetUserObjectInformationW(GetProcessWindowStation(), UOI_FLAGS, &flags, sizeof(flags), &size) &&
        (flags.dwFlags & WSF_VISIBLE))
        MessageBoxW(nullptr,text.c_str(),L"ClipRope",MB_OK|MB_ICONERROR);
}

_Use_decl_annotations_ int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    OutputDebugStringA("ClipRope.Desktop.Runtime.v1\n");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(OleInitialize(nullptr))) return 1;
    int status = 1;
    cliprope::localization::LocalizationManager language(cliprope::localization::builtin_catalog(), [](std::string_view key) {
        const auto diagnostic = "Localization: " + std::string(key) + "\n";
        OutputDebugStringA(diagnostic.c_str());
    });
    language.set_language(cliprope::localization::Language::automatic, cliprope::win::preferred_ui_locale());
    cliprope::win::UiLocalization localization_binding(language);
    cliprope::rendering::WorkspacePaint::use_localization(&language, cliprope::win::ui_font_family());
    try {
        int argc{};
        auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv) throw std::runtime_error("Command line unavailable");
        std::unique_ptr<void, decltype(&LocalFree)> arguments(argv, LocalFree);
        {
            Desktop host(parse_options(argc, argv), language);
            status = host.run();
        }
        if (status == Desktop::restart_status) {
            const auto binary = desktop_binary();
            STARTUPINFOW startup{sizeof(startup)};
            PROCESS_INFORMATION process{};
            std::wstring command = L"\"" + binary + L"\"";
            if (!CreateProcessW(binary.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
                throw std::runtime_error("Cannot restart ClipRope.");
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            status = 0;
        }
    } catch (const cliprope::host::DevelopmentProfileRequired& error) {
        OutputDebugStringA(error.what());
        status = 2; // Fail before opening history or registering startup, without an interactive test dialog.
    } catch (const cliprope::persistence::LocationError&) {
        report_startup_error(win::tr(localization::TextKey::error_relocation));
    } catch (const cliprope::persistence::StorageFailure& failure) {
        report_startup_error(storage_message(failure.reason()));
    } catch (const std::exception&) {
        report_startup_error(win::tr(localization::TextKey::error_startup));
    }
    OleUninitialize();
    return status;
}
