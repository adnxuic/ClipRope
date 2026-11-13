namespace {
void middle_desktop(const fs::path& exe, const fs::path& root, const fs::path& output) {
    using Clock = std::chrono::steady_clock;
    fs::create_directories(output);
    std::ofstream csv(output / "middle-desktop.csv");
    csv << "rope,population,action,action_ms,maximum_heartbeat_ms,heartbeats,from,to\n";
    for (unsigned size : {100u, 1024u, 4096u}) for (bool bin : {false, true}) {
        const domain::Id rope = bin ? domain::rebin_id : "main";
        for (std::string_view action : {"middle_move", "middle_insert", "repeat_head"}) {
            const bool insert = action == "middle_insert", head = action == "repeat_head";
            const auto profile = root / (bin ? "rebin" : "ordinary") / std::to_string(size) / action;
            const auto middle = size / 2;
            Order before;
            domain::Id moving;
            {
                HistoryStore seed(profile);
                domain::Format text;
                text.standard_id = CF_UNICODETEXT;
                text.bytes = sample::formats(false).front().bytes;
                order_fixture::seed(seed, size, rope, {{text}});
                application::CardPreferences prefs;
                prefs.paste_on_double_click = false;
                prefs.follow_collector = false;
                save_card_preferences(seed.database(), prefs);
                save_privacy(seed.database(), {true, {}});
                for (const auto& entry : seed.hanging(rope)) before.push_back(entry.id);
                moving = head ? before.front() : before[middle];
                if (insert) moving = seed.create_floating_text({"",40,550,240,160});
                if (!head) seed.save_anchor(rope, {before[middle-1],0});
            }
            Desktop host(exe, profile, true);
            if (bin) SendMessageW(host.workspace, WM_COMMAND, WPARAM(win::WorkspaceCommand::rebin), 0);
            DWORD pid{};
            GetWindowThreadProcessId(host.workspace, &pid);
            const auto listener = owned_window(pid, L"ClipRope.Listener");
            require(listener != nullptr, "Missing heartbeat listener");
            auto visible = [&] { return (GetWindowLongPtrW(host.workspace, GWL_STYLE) & WS_VISIBLE) != 0; };
            if (head) {
                const auto point = center(hanging_bounds(host, moving));
                click(host.workspace, point);
                click(host.workspace, point, WM_LBUTTONDBLCLK);
                require(until([&] { return !visible() && host.contains("invoke double-click " + moving); }),
                        "Did not prime repeated-head promotion");
                SendMessageW(listener, win::Tray::callback, 0, MAKELPARAM(NIN_SELECT, win::Tray::icon_id));
                require(until(visible), "Could not reopen the workspace for repeated-head control");
            }
            HWND source = insert ? host.floating_window() : host.workspace;
            require(source != nullptr, "Native order source unavailable");
            const auto start = insert ? at_dip(source, 95, 48) : center(hanging_bounds(host, moving));
            POINT end = start;
            UINT release = WM_LBUTTONUP;
            if (head) {
                click(source, start);
                release = WM_LBUTTONDBLCLK;
            } else {
                const auto target = hanging_bounds(host, before[middle+2]);
                end = center(target);
                end.x += (target.right - target.left) / 4;
                pointer(source, WM_LBUTTONDOWN, start, MK_LBUTTON);
                pointer(source, WM_MOUSEMOVE, end, MK_LBUTTON);
            }
            ScreenToClient(source, &end);
            const auto seq = GetClipboardSequenceNumber();
            const auto started = Clock::now();
            require(PostMessageW(source, release, head ? MK_LBUTTON : 0, MAKELPARAM(end.x,end.y)), "Could not submit order gesture");
            if (head) PostMessageW(source, WM_LBUTTONUP, 0, MAKELPARAM(end.x,end.y));
            double maximum{};
            unsigned beats{};
            const auto receipt = (insert ? "return " : "reorder ") + moving + (insert ? " committed" : "");
            bool finished{};
            do {
                const auto tick = Clock::now();
                DWORD_PTR value{};
                require(SendMessageTimeoutW(listener, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 3000, &value), "UI heartbeat timed out");
                maximum = std::max(maximum, std::chrono::duration<double,std::milli>(Clock::now()-tick).count());
                ++beats;
                finished = head ? !visible() && GetClipboardSequenceNumber() != seq : host.contains(receipt);
                if (!finished) SwitchToThread();
            } while (!finished && Clock::now() - started < std::chrono::seconds(5));
            require(finished, "Order gesture did not reach its terminal result");
            const auto elapsed = std::chrono::duration<double,std::milli>(Clock::now()-started).count();
            close_host(host);
            if (head) require(probe::clear_private_sample(sample::text(false)), "Could not retire the private head");
            HistoryStore reopened(profile);
            Order after;
            for (const auto& item : reopened.hanging(rope)) after.push_back(item.id);
            const auto found = std::find(after.begin(),after.end(),moving);
            const auto to = static_cast<size_t>(found-after.begin());
            require(found != after.end() && reopened.entry(moving).rope_id == rope, "Gesture lost its entry");
            if (head) require(before == after, "Repeated promotion changed order");
            else {
                require(to > middle && to <= middle+4, "Drag missed the middle of its bound rope");
                if (!insert) before.erase(before.begin()+middle);
                before.insert(before.begin()+to, moving);
                if (insert && !bin) before.pop_back();
                require(before == after && (!insert || reopened.floating().empty()), "Drop changed the wrong IDs or tail");
            }
            csv << rope << ',' << size << ',' << action << ',' << elapsed << ',' << maximum << ',' << beats << ','
                << (head ? 0 : insert ? -1 : static_cast<int>(middle)) << ',' << to << std::endl;
        }
    }
    require(bool(csv), "Could not finish desktop heartbeat CSV");
}
}
