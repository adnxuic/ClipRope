namespace {
void hidden_peer(const fs::path& exe, const fs::path& profile) {
    Order ids;
    {
        HistoryStore store(profile);
        domain::Layout layout{"",80,100,240,160,false};
        ids.push_back(store.create_floating_text(layout));
        ids.push_back(store.create_floating_text(layout));
        std::sort(ids.begin(),ids.end());
        exec(store.database().handle(),"CREATE TRIGGER reject_first BEFORE UPDATE OF visible ON card_presentation "
            "WHEN NEW.visible=1 AND NEW.entry_id=(SELECT min(id) FROM entries WHERE state='Floating') "
            "BEGIN SELECT RAISE(ABORT,'blocked peer'); END");
    }
    Desktop host(exe,profile);
    DWORD pid{};
    GetWindowThreadProcessId(host.workspace,&pid);
    const auto listener=probe::owned_window(pid,L"ClipRope.Listener");
    auto reveal=[&]{hidden_fixture::reveal(hidden_fixture::tray_menu(listener,pid));};
    reveal();
    require(until([&]{return host.contains("reveal-floating "+ids.front()+" failed");}),"First reveal did not fail");
    reveal();
    auto db=history_fixture::observe(profile,SQLITE_OPEN_READWRITE);
    require(until([&]{
        Statement rows(db.get(),"SELECT entry_id FROM card_presentation WHERE visible=1 ORDER BY entry_id");
        return rows.step() && rows.text(0)==ids.back() && !rows.step();
    }),"One inaccessible card blocked its independent hidden peer");
    exec(db.get(),"DROP TRIGGER reject_first");
    reveal();
    close_host(host);
    HistoryStore reopened(profile);
    require(reopened.layout(ids.front()).visible && reopened.layout(ids.back()).visible &&
            reopened.floating().size()==2, "Reveal cursor lost the failed card on its next pass");
}
void hidden_restore(const fs::path& exe, const fs::path& profile, bool fail_window) {
    domain::Id id, snapshot;
    const domain::Layout stored{"Removed synthetic monitor",-80000,90000,240,160,false};
    {
        HistoryStore store(profile);
        id = store.create_floating_text(stored);
        snapshot = store.entry(id).snapshot_id;
        save_language(store.database(),fail_window ? localization::Language::english : localization::Language::simplified_chinese);
    }
    Desktop host(exe,profile,false,fail_window ? L"reveal-window" : nullptr);
    DWORD pid{};
    GetWindowThreadProcessId(host.workspace,&pid);
    const auto listener = probe::owned_window(pid,L"ClipRope.Listener");
    const auto sequence = GetClipboardSequenceNumber();
    auto observer = history_fixture::observe(profile,SQLITE_OPEN_READWRITE);
    if (!fail_window) exec(observer.get(),"CREATE TRIGGER reject_reveal BEFORE UPDATE OF visible ON card_presentation "
                                         "WHEN NEW.visible=1 BEGIN SELECT RAISE(ABORT,'reveal rejection'); END");
    require(!host.floating_window(), "Hidden card allocated a startup HWND");
    const auto menu = hidden_fixture::tray_menu(listener,pid);
    const auto button = GetDlgItem(menu,win::tray_command::reveal_hidden);
    require(button && (GetWindowLongPtrW(button,GWL_STYLE) & WS_TABSTOP), "Reveal is not a focusable native button");
    wchar_t label[128]{};
    GetWindowTextW(button,label,128);
    require(std::wstring_view(label)==(fail_window ? L"Show a hidden Floating card" : L"显示一张隐藏的悬浮卡片"),
            "Native reveal command lost its localized caption");
    hidden_fixture::reveal(menu);
    require(until([&] { return host.contains("reveal-floating " + id + " failed"); }), "Reveal fault did not reach the host");
    {
        Statement state(observer.get(),"SELECT visible,monitor FROM card_presentation WHERE entry_id=?");
        state.bind(1,id);
        require(state.step() && !state.integer(0) && state.text(1)==stored.monitor && !host.floating_window(),
                "Failed reveal changed saved placement or left a provisional HWND");
    }
    if (!fail_window) exec(observer.get(),"DROP TRIGGER reject_reveal");
    hidden_fixture::reveal(hidden_fixture::tray_menu(listener,pid));
    require(until([&] { return host.floating_window() && (GetWindowLongPtrW(host.floating_window(),GWL_STYLE)&WS_VISIBLE); }),
            "Reveal retry lost its visible window");
    const auto empty = hidden_fixture::tray_menu(listener,pid);
    require(!GetDlgItem(empty,win::tray_command::reveal_hidden), "Tray kept a stale hidden item");
    PostMessageW(empty,WM_KEYDOWN,VK_ESCAPE,0);
    require(until([&] { return !IsWindow(empty); }), "Tray failed to close");
    require(GetClipboardSequenceNumber()==sequence, "Showing a Floating card replaced the clipboard");
    close_host(host);
    {
        HistoryStore reopened(profile);
        const auto entry = reopened.entry(id);
        const auto placed = reopened.layout(id);
        require(!entry.rope_id && entry.snapshot_id==snapshot && reopened.floating().size()==1 && reopened.hanging("main").empty() &&
                placed.visible && placed.monitor!=stored.monitor && std::abs(placed.width-stored.width)<=1 &&
                std::abs(placed.height-stored.height)<=1, "Reopen lost the revealed identity or rescued layout");
    }
    Desktop restarted(exe,profile);
    require(restarted.floating_window() && (GetWindowLongPtrW(restarted.floating_window(),GWL_STYLE)&WS_VISIBLE),
            "Revealed card was not restored at the next launch");
    close_host(restarted);
}
}
