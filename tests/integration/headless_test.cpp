#include "persistence/history_store.hpp"
#include "application/diagnostics.hpp"
#include "persistence/hotkey_settings.hpp"
#include "platform_win/history_clipboard.hpp"
#include "../clipboard_sources/external_copy.hpp"
#include "../clipboard_sources/sample.hpp"
#include "history_fixture.hpp"
#include "../clipboard_sources/media_sample.hpp"
#include <fstream>
#include <algorithm>
#include <filesystem>

namespace {
using namespace cliprope;
using namespace cliprope::application;
using persistence::HistoryStore;
using probe::ExternalCopy;
using probe::require;
namespace fs = std::filesystem;

domain::Snapshot plain() {
    domain::Format text;
    text.standard_id = CF_UNICODETEXT;
    text.bytes = sample::formats(false).front().bytes;
    return {{text}};
}

CaptureOutcome drain(win::HistoryCapture& capture) {
    CaptureOutcome outcome;
    probe::pump_until([&] {
        for (auto& event : capture.poll()) outcome = std::move(event);
        return !capture.busy();
    }, "Capture did not drain");
    return outcome;
}

void expect_size(HistoryStore& store, const domain::Id& rope, size_t count) {
    require(store.hanging(rope).size() == count, "Unexpected Hanging count");
}

void snip_publications(HistoryStore& store) {
    auto rope = store.create_rope("Synthetic snips", 2);
    store.set_active_rope(rope);
    store.admit(rope, plain());
    const auto survivor = store.admit(rope, plain());
    CaptureCoordinator capture(store);
    domain::Format png;
    png.name = "PNG";
    png.bytes = {1, 2, 3};
    domain::Snapshot picture{{png}};
    Diagnostics diagnostics;
    auto publish = [&](unsigned version, unsigned round, domain::Snapshot snapshot) {
        CaptureOutcome out;
        auto ticket = capture.begin(version, "SnippingTool.exe", false, {}, out, round);
        require(ticket.has_value(), "Snip ticket was lost");
        diagnostics.capture(out.status);
        auto result = capture.finish(*ticket, version, std::move(snapshot));
        diagnostics.capture(result.status);
        return result;
    };
    require(publish(1, 1, picture).status == "snip-pending", "First publication trimmed too early");
    auto richer = picture;
    richer.formats.front().tymed = TYMED_ISTREAM;
    richer.formats.push_back(plain().formats.front());
    const auto first = publish(2, 1, richer).entry;
    require(diagnostics.captured == 1 && !diagnostics.partial && !diagnostics.failures && !diagnostics.skipped,
            "Merged snip counted an intermediate result");
    auto order = store.hanging(rope);
    require(!first.empty() && order.size() == 2 && order[0].id == first && order[1].id == survivor &&
            store.load(first).snapshot.formats.size() == 2, "One snip lost formats or trimmed twice");
    publish(3, 2, picture);
    const auto second = publish(4, 2, picture).entry;
    order = store.hanging(rope);
    require(second != first && order[0].id == second && order[1].id == first, "Independent identical snips merged");
    publish(5, 3, picture);
    require(capture.settle(true).value().entry.size() && !capture.settling(), "Single-publication snip lost");
    publish(6, 4, picture);
    capture.abandon();
    require(!capture.settle(true), "Cancelled snip was admitted");
    publish(7, 5, picture);
    domain::Snapshot private_update;
    private_update.capture_status = "excluded-marker";
    require(publish(8, 5, private_update).status == "excluded-marker" && !capture.settle(true),
            "A privacy marker on the second publication admitted the staged snip");
    require(diagnostics.skipped == 1 && !diagnostics.failures, "Privacy refusal was counted twice");
    publish(9, 6, picture);
    auto edited = picture;
    edited.formats.front().bytes.back() = 4;
    const auto updated = publish(10, 6, edited);
    require(!updated.entry.empty() && !capture.settle(true) &&
            store.load(updated.entry).snapshot.formats.front().bytes == edited.formats.front().bytes,
            "Re-encoding an image in the same selection created another entry or lost the final bytes");
    store.set_active_rope("main");
}

void admission_decisions(HistoryStore& store) {
    std::vector<Admission> accepted;
    CaptureCoordinator capture(store, [&](Admission item) {
        accepted.push_back(std::move(item));
        return CaptureOutcome{"admission-pending", {}};
    });
    auto begin = [&](DWORD version, unsigned round) {
        CaptureOutcome out;
        auto ticket = capture.begin(version, "fixture", false, {}, out, round);
        require(ticket.has_value(), "Missing decision fixture ticket");
        return *ticket;
    };
    domain::Format png;
    png.name = "PNG";
    png.bytes = {1,2,3};
    const auto original = store.active_rope();
    const auto first = begin(1, 1);
    require(capture.finish(first, 1, {{png}}).status == "snip-pending" && accepted.empty(), "Early snip submitted");
    const auto other = store.create_rope("Decision target", 2);
    store.set_active_rope(other);
    const auto next = begin(2, 2);
    const auto switched = capture.settle();
    require(accepted.size() == 1 && accepted.front().ticket.serial == first.serial &&
            accepted.front().ticket.rope == original && switched && switched->status == "admission-pending", "Round switch rebound staged capture");
    capture.finish(next, 2, {{png}});
    const auto closed = capture.settle(true);
    require(closed && closed->status == "admission-pending" && accepted.size() == 2 &&
            accepted.back().ticket.rope == other && !capture.settle(true), "Shutdown lost or repeated staged capture");
    store.set_active_rope(original);
    require(store.hanging(other).empty(), "Decision made a durable change");
    const auto committed = accepted.back().commit(store);
    require(committed.status == "captured" && store.entry(committed.entry).rope_id == other, "Accepted capture rebound at commit");
    Diagnostics diagnostics;
    diagnostics.capture(closed->status);
    const auto stale = begin(3, 0);
    diagnostics.capture(capture.finish(stale, 4, plain()).status);
    diagnostics.capture(accepted.front().commit([](const auto&, const auto&) -> domain::Id {
        throw std::runtime_error("Rejected test write");
    }).status);
    require(!diagnostics.captured && diagnostics.skipped == 1 && diagnostics.failures == 1,
            "Intermediate admission, superseded ticket or rejected write counted incorrectly");
}

void policy_and_races(HistoryStore& store, const wchar_t* producer) {
    const auto main = store.active_rope();
    const auto seed = store.admit(main, plain());
    const auto other = store.create_rope("Second", 12);
    win::HistoryClipboard clipboard;
    win::HistoryCapture capture(store, clipboard);
    auto expect_filtered = [&](const wchar_t* mode, CapturePolicy policy, std::string_view status) {
        ExternalCopy copy(producer, mode);
        auto outcome = capture.observe(policy);
        if (outcome.status == "pending") outcome = drain(capture);
        require(outcome.status == status && outcome.entry.empty(), "Incorrect filtering outcome");
        require(WaitForSingleObject(copy.reading, 0) == WAIT_TIMEOUT, "Excluded source payload requested");
        require(store.hanging(main).front().id == seed, "Filtering changed prior history");
        expect_size(store, main, 1);
    };
    expect_filtered(L"paused", {true, {}}, "paused");
    expect_filtered(L"paused", {false, {"CLIPROPE_OLE_SOURCE.EXE"}}, "excluded-app");
    expect_filtered(L"exclude", {}, "excluded-marker");
    expect_filtered(L"no-history", {}, "excluded-marker");
    expect_filtered(L"failed", {}, "no-usable-format");
    {
        ExternalCopy copy(producer, L"no-cloud");
        require(capture.observe().status == "pending", "Cloud-only marker blocked local history");
        require(!drain(capture).entry.empty(), "Cloud-only content missing");
        expect_size(store, main, 2);
    }
    {
        ExternalCopy copy(producer, L"partial");
        capture.observe();
        auto result = drain(capture);
        require(result.status == "partial" && !result.entry.empty(), "Partial capture not admitted");
        auto content = store.load(result.entry);
        unsigned saved{}, skipped{}, failed{};
        for (const auto& format : content.snapshot.formats) {
            saved += format.outcome == "saved";
            skipped += format.outcome == "skipped";
            failed += format.outcome == "failed";
        }
        require(saved == 1 && skipped == 1 && failed == 1, "Partial manifest lost outcomes");
        require(capture.observe().status == "duplicate", "Duplicate event admitted");
        expect_size(store, main, 3);
    }
    {
        ExternalCopy copy(producer, L"delayed");
        capture.observe();
        probe::pump_until([&] { return WaitForSingleObject(copy.reading, 0) == WAIT_OBJECT_0; }, "No delayed read");
        store.set_active_rope(other);
        SetEvent(copy.proceed);
        auto result = drain(capture);
        require(!result.entry.empty() && store.entry(result.entry).rope_id == main, "Late result admitted to new rope");
        expect_size(store, other, 0);
    }
    {
        ExternalCopy copy(producer, L"delayed");
        capture.observe();
        probe::pump_until([&] { return WaitForSingleObject(copy.reading, 0) == WAIT_OBJECT_0; }, "No pending deletion");
        store.erase_rope(other);
        SetEvent(copy.proceed);
        require(drain(capture).status == "history-failed", "Deleted capture target reused");
        expect_size(store, main, 4);
    }
    {
        ExternalCopy old(producer, L"delayed");
        capture.observe();
        probe::pump_until([&] { return WaitForSingleObject(old.reading, 0) == WAIT_OBJECT_0; }, "No pending replacement");
        ExternalCopy newer(producer, L"second");
        capture.observe();
        bool admitted = false;
        probe::pump_until([&] {
            for (const auto& event : capture.poll()) admitted |= !event.entry.empty();
            return admitted;
        }, "New source blocked behind obsolete source");
        const auto top = store.hanging(main).front();
        require(store.load(top.id).snapshot.formats.front().bytes == sample::formats(true).front().bytes,
                "Wrong content admitted after supersession");
        SetEvent(old.proceed);
        drain(capture);
        expect_size(store, main, 5);
    }
    {
        CaptureCoordinator coordinator(store);
        CaptureOutcome event;
        auto ticket = *coordinator.begin(70, "source.exe", false, {}, event);
        require(coordinator.finish(ticket, 71, plain()).status == "superseded", "Mismatched version accepted");
        ticket = *coordinator.begin(72, "source.exe", false, {}, event);
        coordinator.abandon();
        require(coordinator.finish(ticket, 72, plain()).status == "abandoned", "Abandoned work accepted");
        store.set_rebin_visible(true);
        ticket = *coordinator.begin(73, "source.exe", false, {}, event);
        store.set_rebin_visible(false);
        auto recycled = coordinator.finish(ticket, 73, plain());
        require(store.entry(recycled.entry).rope_id == cliprope::domain::rebin_id &&
                store.active_rope() == main, "Hiding ReBin redirected a pending capture");
        expect_size(store, main, 5);
    }
    {
        ExternalCopy first(producer, L"held");
        capture.observe();
        probe::pump_until([&] { return WaitForSingleObject(first.reading, 0) == WAIT_OBJECT_0; }, "First slot did not start");
        ExternalCopy second(producer, L"held");
        capture.observe();
        probe::pump_until([&] { return WaitForSingleObject(second.reading, 0) == WAIT_OBJECT_0; }, "Second slot did not start");
        ExternalCopy pending(producer, L"paused");
        capture.observe();
        ExternalCopy latest(producer, L"second");
        capture.observe();
        SetEvent(first.proceed);
        SetEvent(second.proceed);
        drain(capture);
        require(WaitForSingleObject(pending.reading, 0) == WAIT_TIMEOUT, "Superseded pending ticket read payload");
        expect_size(store, main, 6);
        const auto latest_id = store.hanging(main).front().id;
        require(store.load(latest_id).snapshot.formats.front().bytes == sample::formats(true).front().bytes,
                "Bounded pending slot did not preserve latest copy");
    }
    std::cout << "policy=paused,source,history-marker,cloud-only; partial=1; total-failure=0; late-target=original; "
                 "deleted-target=0; blocked-old-source=discarded; replacement=once; saturated=two-active+latest\n";
}

void child(const wchar_t* exe, const fs::path& folder, const wchar_t* action, const std::wstring& id = {}) {
    std::vector<std::wstring> arguments{exe, folder.wstring(), action};
    if (!id.empty()) arguments.push_back(id);
    HANDLE process = probe::launch(arguments, probe::desktop_path());
    require(process != nullptr, "Headless process did not launch");
    probe::pump_until([&] { return WaitForSingleObject(process, 0) == WAIT_OBJECT_0; }, "Headless process timed out");
    DWORD code{};
    bool success = GetExitCodeProcess(process, &code) && code == 0;
    CloseHandle(process);
    require(success, "Headless command failed");
}

// The CLI can be either this build or a frozen, hash-verified v11 reader.
void reader_compatibility(const fs::path& root, const wchar_t* reader) {
    const auto profile=root/"v11", backup=root/"backup", restored=root/"restored";
    const std::wstring text(5000,L'K');
    const auto* first=reinterpret_cast<const unsigned char*>(text.c_str());
    domain::Snapshot saved{{{CF_UNICODETEXT,{}, {first,first+(text.size()+1)*sizeof(wchar_t)}}}};
    const auto png=media_sample::png();
    saved.formats.push_back({0,"PNG",png.bytes});
    const domain::Layout hidden_layout{"synthetic-display",37.,41.,310.,170.,false};
    domain::Id retained, returning, hidden, victim, snapshot;
    CardPreferences preferences;
    preferences.paste_on_double_click=false;
    preferences.follow_collector=false;
    preferences.font_size=17;
    {
        HistoryStore store(profile);
        retained=store.admit("main",saved);
        snapshot=store.entry(retained).snapshot_id;
        returning=store.copy(retained,"main");
        store.detach(returning,{"synthetic-display",22.,25.,240.,170.,true});
        hidden=store.copy(retained,"main");
        store.detach(hidden,hidden_layout);
        victim=store.admit("main",plain());
        store.reorder(retained,0);
        store.set_capacity("main",2);
        persistence::save_card_preferences(store.database(),preferences);
    }
    // Exercise the old reader on the original v11 profile and inspect a separate sealed backup.
    persistence::Database::export_profile(profile,backup);
    persistence::Database::restore_backup(backup,restored);
    auto inspect=[&](const fs::path& path,bool returned) {
        HistoryStore store(path);
        persistence::Statement version(store.database().handle(),"PRAGMA user_version");
        require(version.step() && version.integer(0)==11,"Reader changed schema version");
        auto order=store.hanging("main");
        require(order.size()==2 && order[0].id==(returned ? returning : retained) &&
                order[1].id==(returned ? retained : victim),"Reader changed the wrong tail");
        persistence::Statement exists(store.database().handle(),"SELECT count(*) FROM entries WHERE id=?");
        exists.bind(1,victim);
        require(exists.step() && exists.integer(0)==(returned ? 0 : 1),"Tail victim survived the return");
        require(store.floating().size()==(returned ? 1u : 2u) && !store.entry(hidden).rope_id &&
                !store.entry(hidden).sort_key,"Reader changed Floating membership");
        auto layout=store.layout(hidden);
        require(layout.monitor==hidden_layout.monitor && layout.x==hidden_layout.x && layout.y==hidden_layout.y &&
                layout.width==hidden_layout.width && layout.height==hidden_layout.height && !layout.visible,
                "Reader changed hidden Floating placement");
        for (const auto& id : {retained,returning,hidden}) {
            auto payload=store.load(id);
            require(store.entry(id).snapshot_id==snapshot && payload.snapshot.formats.size()==2 &&
                    payload.snapshot.formats[0].bytes==saved.formats[0].bytes &&
                    payload.snapshot.formats[1].bytes==png.bytes,"Reader changed shared snapshot or sealed bytes");
        }
        require(persistence::read_card_preferences(store.database())==preferences,"Compatible reader changed preferences");
    };
    inspect(restored,false);
    const auto thumbnail=profile/"thumbnails"/(persistence::sha256(png.bytes)+"-png.thumb");
    fs::create_directories(thumbnail.parent_path());
    { std::ofstream cache(thumbnail,std::ios::binary); cache<<"derived fixture"; }
    child(reader,profile,L"return",std::wstring(returning.begin(),returning.end()));
    require(fs::exists(thumbnail),"Reader removed a reachable legacy cache name");
    auto actual=capture_clipboard(GetClipboardSequenceNumber()).saved;
    auto output=std::ranges::find(actual,UINT(CF_UNICODETEXT),&Representation::format);
    require(output!=actual.end() && output->bytes==saved.formats[0].bytes,"Reader published different sealed bytes");
    require(probe::clear_private_sample(text),"Could not release the private reader sample");
    inspect(profile,true);
    persistence::Database::verify_store(profile);
    persistence::Database::verify_store(restored);
    std::cout<<"reader schema=11 shared_entries=3 surviving_hanging=2 hidden_floating=1 exact_victim=1 "
                 "sealed_text_bytes="<<saved.formats[0].bytes.size()<<" png_bytes="<<png.bytes.size()
             <<" legacy_cache_retained=1 backup_restore=verified"<<std::endl;
}

void follow_head(const fs::path& root, const wchar_t* exe) {
    win::HistoryClipboard clipboard;
    domain::Id returning;
    bool reject = false;
    {
        HistoryStore store(root, [&](auto point) {
            if (reject && point == "transaction") throw std::runtime_error("Rejected manual change");
        });
        const auto main = store.active_rope();
        const auto other = store.create_rope("Other", 100);
        auto prefs = persistence::read_card_preferences(store.database());
        require(prefs.follow_card_head && prefs.follow_collector, "Existing profile did not default to follow");
        const auto a = store.admit(main, plain());
        const auto b = store.admit(main, plain());
        const auto c = store.admit(other, plain());
        unsigned published{};
        bool fail_publish = false;
        Publication result{};
        store.on_head_change = [&](const auto& heads) {
            result = publish_head_change(heads, prefs, store, [&](const auto& saved) {
                if (fail_publish) return PublicationId{};
                ++published;
                return clipboard.publish(saved);
            });
        };
        win::HistoryCapture capture(store, clipboard);
        for (bool card : {false, true}) for (bool rope : {false, true}) {
            prefs.follow_card_head = card;
            prefs.follow_collector = rope;
            persistence::save_card_preferences(store.database(), prefs);
            require(persistence::read_card_preferences(store.database()) == prefs, "Switches did not save independently");
            auto count = published;
            auto seq = GetClipboardSequenceNumber();
            store.reorder(a, 0);
            require(published == count + card && (seq != GetClipboardSequenceNumber()) == card, "Wrong card switch");
            if (card) require(capture.observe().status == "self", "Follow publication recorded a duplicate");
            store.reorder(b, 0);
            count = published;
            seq = GetClipboardSequenceNumber();
            store.set_active_rope(other);
            require(published == count + rope && (seq != GetClipboardSequenceNumber()) == rope, "Wrong rope switch");
            store.set_active_rope(main);
        }
        const auto changed = [&](const auto& action, bool expected) {
            const auto count = published;
            action();
            require(published == count + expected, "Unexpected publication count for manual command");
        };
        changed([&] { store.move(b, other); }, true);
        changed([&] { store.reorder(c, 0); }, false);
        changed([&] { store.save_text(a, store.entry(a).revision, L"Edited head"); }, true);
        const auto bytes = store.load(a).snapshot.formats[0].bytes;
        require(capture_clipboard(GetClipboardSequenceNumber()).saved.front().bytes == bytes, "Clipboard lost edited text");
        changed([&] { store.copy(c, main); }, true);
        const auto copy = store.collector_head().entry;
        reject = true;
        history_fixture::rejects([&] { store.erase(copy); }, "Faulted deletion committed");
        reject = false;
        require(store.collector_head().entry == copy, "Rollback lost head");
        fail_publish = true;
        const auto seq = GetClipboardSequenceNumber();
        store.erase(copy);
        require(result == Publication::failed && store.collector_head().entry == a && seq == GetClipboardSequenceNumber(),
                "Publication failure rolled back history or replaced the clipboard");
        fail_publish = false;
        changed([&] { store.detach(a); }, false); // The collector is now empty.
        changed([&] { store.set_rebin_visible(true); }, false);
        changed([&] { store.rehang(a, domain::rebin_id); }, true);
        changed([&] { store.set_rebin_visible(false); }, false);
        changed([&] { store.admit(main, plain()); }, false);
        returning = a;
        store.detach(a);
        prefs.follow_card_head = prefs.follow_collector = false;
        persistence::save_card_preferences(store.database(), prefs);
    }
    const auto seq = GetClipboardSequenceNumber();
    {
        HistoryStore store(root);
        const auto saved = persistence::read_card_preferences(store.database());
        require(!saved.follow_card_head && !saved.follow_collector, "Reopen lost switches");
        persistence::reset_global_preferences(store.database());
        require(persistence::read_card_preferences(store.database()) == CardPreferences{}, "Reset lost follow defaults");
    }
    require(seq == GetClipboardSequenceNumber(), "Reopen or reset published history");
    child(exe, root, L"return", std::wstring(returning.begin(), returning.end()));
    HistoryStore store(root);
    require(store.collector_head().entry == returning &&
            capture_clipboard(GetClipboardSequenceNumber()).saved.front().bytes == store.load(returning).snapshot.formats[0].bytes,
            "Headless return did not publish the committed snapshot");
    require(probe::clear_private_sample(L"Edited head"), "Could not retire the synthetic headless publication");
}

void round_trip(const fs::path& root, const wchar_t* producer, const wchar_t* exe) {
    {
        ExternalCopy first(producer, L"normal");
        child(exe, root, L"capture");
        ExternalCopy second(producer, L"second");
        require(capture_clipboard(GetClipboardSequenceNumber()).saved == sample::formats(true), "Intervening overwrite failed");
    }
    child(exe, root, L"head");
    const auto restored = capture_clipboard(GetClipboardSequenceNumber());
    // Flushed OLE can enumerate Windows-synthesized formats in a different order.
    auto expected = sample::formats(false);
    require(std::is_permutation(restored.saved.begin(), restored.saved.end(), expected.begin(), expected.end()),
            "Restart lost a saved format");
    {
        HistoryStore store(root);
        auto main = store.active_rope();
        expect_size(store, main, 1);
        auto id = store.hanging(main).front().id;
        win::HistoryClipboard clipboard;
        win::HistoryCapture captures(store, clipboard);
        auto command = bind_invocation(store, Route::tray, id);
        auto result = command.run(store, [&](const auto& data) { return clipboard.publish(data); });
        require(result.publication == Publication::published && result.delivery == Delivery::skipped, "Tray publish failed");
        require(captures.observe().status == "self" && !captures.busy(), "Self capture admitted");
        ExternalCopy equal(producer, L"normal");
        captures.observe();
        auto admitted = drain(captures);
        require(!admitted.entry.empty() && admitted.entry != id, "Same bytes from new external owner suppressed");
        expect_size(store, main, 2);
    }
    std::cout << "restart=two-processes; formats=Unicode+HTML; producer-exited; self=0; equal-external=1\n";
}

void commands(const fs::path& root, const wchar_t* producer) {
    bool fail_commit = false;
    HistoryStore store(root, [&](std::string_view boundary) {
        if (fail_commit && boundary == "transaction") throw std::runtime_error("Synthetic transaction failure");
    });
    const auto main = store.active_rope();
    const auto target = store.create_rope("Full", 1);
    const auto seed = store.admit(target, plain());
    auto id = store.admit(main, plain());
    unsigned publish_count{}, delivery_count{};
    bool view_updated = false;
    Publish publish = [&](const auto&) { ++publish_count; return true; };
    Deliver deliver = [&](const auto&) { ++delivery_count; return Delivery::shortcut_sent; };
    auto first = bind_invocation(store, Route::double_click, id);
    auto result = first.run(store, publish, [&](auto receipt) {
        require(view_updated, "Shortcut preceded history and view update");
        return deliver(receipt);
    }, [&](auto) { view_updated = true; });
    require(result.delivery == Delivery::shortcut_sent && result.history == HistoryEffect::committed, "Selected field route failed");
    first.run(store, publish, deliver);
    require(publish_count == 1 && delivery_count == 1, "Invocation replayed");
    for (auto route : {Route::tray, Route::rope_head}) {
        auto tray = bind_invocation(store, route, id);
        require(tray.run(store, publish, deliver).delivery == Delivery::skipped, "Publication-only route inserted");
    }
    auto copy_only = Invocation::hanging_double_click(store.entry(id), false);
    result = copy_only.run(store, publish, deliver);
    require(result.publication == Publication::published && result.history == HistoryEffect::committed &&
            result.delivery == Delivery::skipped && delivery_count == 1, "Copy-only gesture sent native input");
    auto failed = bind_invocation(store, Route::double_click, id);
    result = failed.run(store, [](const auto&) { return false; }, deliver);
    require(result.publication == Publication::failed && result.history == HistoryEffect::skipped &&
            result.delivery == Delivery::skipped && store.entry(id).rope_id == main, "Failed publication mutated history");
    auto uncertain = bind_invocation(store, Route::double_click, id);
    fail_commit = true;
    result = uncertain.run(store, publish, deliver);
    fail_commit = false;
    require(result.delivery == Delivery::shortcut_sent && result.history == HistoryEffect::failed,
            "History failure lost independent delivery result");
    auto lost = bind_invocation(store, Route::double_click, id);
    result = lost.run(store, publish, [](const auto&) { return Delivery::unconfirmed; });
    require(result.delivery == Delivery::unconfirmed && result.history == HistoryEffect::committed,
            "Unconfirmed delivery prevented promotion");
    auto manual = bind_invocation(store, Route::double_click, id);
    require(manual.run(store, publish).delivery == Delivery::no_field, "No-field double-click failed");
    store.detach(id);
    history_fixture::rejects([&] { bind_invocation(store, Route::double_click, id); },
                            "Floating entry accepted for clipboard recall");
    auto button = bind_invocation(store, Route::return_button, id, target);
    const auto before = publish_count;
    result = button.run(store, publish, deliver);
    require(result.history == HistoryEffect::committed && result.delivery == Delivery::skipped &&
            publish_count == before && delivery_count == 2, "Return button changed clipboard or field");
    require(store.hanging(target).size() == 1 && store.hanging(target).front().id == id && id != seed,
            "Return button did not trim the bound rope");

    const auto moved_rope = store.create_rope("Moved during gesture", 3);
    domain::Id newer;
    auto reentrant = bind_invocation(store, Route::double_click, id);
    result = reentrant.run(store, [&](const auto& content) {
        const auto receipt = publish(content);
        store.move(id, moved_rope);
        newer = store.admit(moved_rope, plain());
        return receipt;
    }, deliver);
    const auto after_move = store.hanging(moved_rope);
    require(result.publication == Publication::published && result.delivery == Delivery::shortcut_sent &&
            result.history == HistoryEffect::failed && after_move.size() == 2 &&
            after_move[0].id == newer && after_move[1].id == id, "Stale invocation promoted into another rope");

    auto large = plain();
    large.formats.front().bytes.clear();
    const std::wstring text(4096, L'x');
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.c_str());
    large.formats.front().bytes.assign(bytes, bytes + (text.size() + 1) * sizeof(wchar_t));
    auto large_id = store.admit(main, large);
    std::weak_ptr<void> lease;
    win::HistoryClipboard clipboard;
    auto saved = store.load_snapshot(store.entry(large_id).snapshot_id);
    const auto older = clipboard.publish(saved);
    const auto newer_receipt = clipboard.publish(saved);
    require(older && !clipboard.owns_publication(older) && clipboard.owns_publication(newer_receipt),
            "A later publication authenticated an older receipt");
    saved = {};
    auto head = bind_invocation(store, Route::rope_head, large_id);
    result = head.run(store, [&](const auto& data) { lease = data.lease; return clipboard.publish(data); });
    require(result.publication == Publication::published && !lease.expired(), "Publication lost its lease");
    store.erase(large_id);
    store.gc();
    require(!lease.expired() && capture_clipboard(GetClipboardSequenceNumber()).saved.front().bytes ==
                large.formats.front().bytes, "Deleting last entry invalidated served bytes");
    ExternalCopy replacement(producer, L"second");
    clipboard.release_if_replaced();
    require(lease.expired(), "Superseded publication kept a lease");
    store.gc();
    std::cout << "selected=once; no-field=manual; rejected-publication=unchanged; history-fault=independent; "
                 "uncertain=no-retry; tray/head=publish-only; return=history-only; reentrant-move=preserved; "
                 "served-lease=held-through-GC\n";
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 4 || !probe::private_station() || FAILED(OleInitialize(nullptr))) return 2;
    int result = 0;
    try {
        history_fixture::Directory temporary;
        const auto& root = temporary.path;
        if (std::wstring_view(argv[1]) == L"capture") {
            HistoryStore store(root);
            policy_and_races(store, argv[2]);
            snip_publications(store);
            admission_decisions(store);
        } else if (std::wstring_view(argv[1]) == L"reader") {
            reader_compatibility(root,argv[3]);
        } else if (std::wstring_view(argv[1]) == L"follow") {
            follow_head(root, argv[3]);
        } else {
            round_trip(root, argv[2], argv[3]);
            commands(root, argv[2]);
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    OleUninitialize();
    return result;
}
