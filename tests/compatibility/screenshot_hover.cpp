#include "desktop_app.hpp"
#include "../clipboard_sources/media_sample.hpp"
#include "platform_win/workspace_commands.hpp"
#include "platform_win/performance_counters.hpp"
#include "persistence/hotkey_settings.hpp"
#include "rendering/image_preview.hpp"
#include <psapi.h>
#include <dwmapi.h>

using namespace workspace_fixture;
int wmain(int argc, wchar_t **argv) {
    if (argc != 2 && argc != 3)
        return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(OleInitialize(nullptr)))
        return 1;
    try {
        history_fixture::Directory profile;
        std::string image_id, text_id;
        {
            HistoryStore store(profile.path);
            save_shortcut(store.database(), application::workspace_toggle, {true, "Ctrl+Alt+F11"});
            auto sample = media_sample::dib(false);
            BITMAPINFOHEADER header{};
            std::memcpy(&header, sample.bytes.data(), sizeof header);
            header.biWidth = 1280;
            header.biHeight = 720;
            header.biSizeImage = 1280 * 720 * 3;
            sample.bytes.resize(sizeof header + header.biSizeImage, 120);
            std::memcpy(sample.bytes.data(), &header, sizeof header);
            for (int y = 0; y < 720; ++y)
                for (int x = 0; x < 1280; ++x) {
                    auto *pixel = sample.bytes.data() + sizeof header + (size_t(y) * 1280 + x) * 3;
                    pixel[0] = static_cast<unsigned char>(y < 360 ? 255 : 0);
                    pixel[1] = static_cast<unsigned char>(x >= 640 ? 255 : 0);
                    pixel[2] = static_cast<unsigned char>(y >= 360 ? 255 : 0);
                }
            domain::Format image;
            image.standard_id = CF_DIB;
            image.bytes = sample.bytes;
            domain::Format text;
            text.standard_id = CF_UNICODETEXT;
            text.bytes = {'a', 0, 0, 0};
            text_id = store.admit("main", domain::Snapshot{{text}});
            auto other = store.create_rope("Second", 100);
            store.admit(other, domain::Snapshot{{text}});
            image_id = store.admit("main", domain::Snapshot{{image}});
            auto floating = store.admit("main", domain::Snapshot{{image}});
            store.detach(floating, {"", 700, 500, 200, 160});
        }
        RECT work{};
        require(SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0), "Work area unavailable");
        HWND editor =
            CreateWindowExW(WS_EX_TOOLWINDOW, L"EDIT", L"Synthetic selection",
                            WS_POPUP | WS_VISIBLE | ES_MULTILINE, work.left + 20, work.bottom - 100,
                            300, 60, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(editor != nullptr, "External editor unavailable");
        struct Window {
            HWND h;
            ~Window() { DestroyWindow(h); }
        } cleanup{editor};
        SetForegroundWindow(editor);
        SetFocus(editor);
        SendMessageW(editor, EM_SETSEL, 2, 8);
        Desktop app(fs::absolute(argv[1]), profile.path, true);
        DWORD pid{};
        GetWindowThreadProcessId(app.workspace, &pid);
        const auto sequence = GetClipboardSequenceNumber();
        const auto original = order(profile.path);
        auto count = [&](win::PerformanceCounter counter) {
            return request(owned_window(pid, L"ClipRope.Listener"), win::performance_query,
                           WPARAM(counter));
        };
        auto overlay = [&] { return owned_window(pid, L"ClipRope.ScreenshotPreview"); };
        auto visible = [&] {
            auto h = overlay();
            return h && IsWindowVisible(h);
        };
        auto leave = [&] {
            const bool was_visible = visible();
            const auto start = std::chrono::steady_clock::now();
            SetCursorPos(work.left + 10, work.bottom - 10);
            require(until([&] { return !visible(); }, 150ms), "Preview did not hide after leave");
            if (was_visible)
                std::cout << "close_ms="
                          << std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - start)
                                 .count()
                          << '\n';
        };
        auto check = [&](HWND source, POINT point) {
            leave();
            aim(editor, {work.left + 40, work.bottom - 75});
            mouse(MOUSEEVENTF_LEFTDOWN);
            mouse(MOUSEEVENTF_LEFTUP);
            require(until([&] { return GetForegroundWindow() == editor; }),
                    "External focus unavailable");
            SetFocus(editor);
            SendMessageW(editor, EM_SETSEL, 2, 8);
            const auto foreground = GetForegroundWindow();
            require(foreground == editor, "External focus unavailable");
            aim(source, point);
            const auto start = std::chrono::steady_clock::now();
            brief(350ms);
            require(!visible(), "Preview appeared before dwell");
            require(until(visible, 2500ms), "Screenshot preview did not appear");
            const auto metadata=owned_window(pid,L"ClipRope.Tooltip");
            require(!metadata || !IsWindowVisible(metadata),"Screenshot preview and metadata tooltip appeared together");
            auto elapsed =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count();
            require(GetForegroundWindow() == foreground && GetFocus() == editor,
                    "Preview stole focus");
            DWORD first{}, last{};
            SendMessageW(editor, EM_GETSEL, reinterpret_cast<WPARAM>(&first),
                         reinterpret_cast<LPARAM>(&last));
            require(first == 2 && last == 8, "Preview changed selection");
            RECT rect{};
            GetWindowRect(overlay(), &rect);
            require(WindowFromPoint(center(rect)) != overlay(),
                    "Preview captured cross-process hit testing");
            std::cout << "display_after_dwell_ms=" << elapsed - 500 << '\n';
        };
        check(app.workspace, center(app.card(image_id)));
        if (argc == 3) {
            RECT rect{};
            GetWindowRect(overlay(), &rect);
            const int w = rect.right - rect.left, h = rect.bottom - rect.top;
            BITMAPINFO info{};
            info.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
            HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
            void *bits{};
            HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
            require(screen && dc && bitmap, "Reference surface unavailable");
            auto old = SelectObject(dc, bitmap);
            DwmFlush();
            const bool copied =
                BitBlt(dc, 0, 0, w, h, screen, rect.left, rect.top, SRCCOPY | CAPTUREBLT) != FALSE;
            std::vector<unsigned char> dib(sizeof(BITMAPINFOHEADER) + size_t(w) * h * 4);
            std::memcpy(dib.data(), &info.bmiHeader, sizeof info.bmiHeader);
            std::memcpy(dib.data() + sizeof info.bmiHeader, bits,
                        dib.size() - sizeof info.bmiHeader);
            SelectObject(dc, old);
            DeleteObject(bitmap);
            DeleteDC(dc);
            ReleaseDC(nullptr, screen);
            require(copied && visible(), "Reference capture failed");
            auto png = rendering::encode_dib_png(CF_DIB, dib);
            std::ofstream output(fs::path(argv[2]), std::ios::binary);
            output.write(reinterpret_cast<const char *>(png.data()), std::streamsize(png.size()));
            require(bool(output), "Reference image could not be saved");
        }
        const auto requests = count(win::PerformanceCounter::screenshot_requests);
        leave();
        aim(app.workspace, center(app.card(text_id)));
        brief(700ms);
        require(!visible() && count(win::PerformanceCounter::screenshot_requests) == requests,
                "Text card requested a full preview");
        check(app.workspace, center(app.card(image_id)));
        leave();
        SendMessageW(app.workspace, WM_COMMAND, WPARAM(win::WorkspaceCommand::overview), 0);
        require(until([&] { return app.frame().rows.size() == 2; }), "Overview did not open");
        check(app.workspace, center(app.card(image_id)));
        leave();
        SendMessageW(app.workspace, WM_COMMAND, WPARAM(win::WorkspaceCommand::overview), 0);
        require(until([&] { return app.frame().rows.size() == 1; }), "Single view did not return");
        auto floating = app.floating_window();
        require(floating != nullptr, "Floating screenshot unavailable");
        RECT floating_rect{};
        GetWindowRect(floating, &floating_rect);
        auto floating_point = center(floating_rect);
        floating_point.y += MulDiv(19, GetDpiForWindow(floating), 96);
        check(floating, floating_point);
        leave();
        require(GetClipboardSequenceNumber() == sequence && order(profile.path) == original,
                "Hover changed clipboard or history");
        app.stop();
        require(app.clean_exit, "Desktop did not exit cleanly");
        std::cout << "Screenshot single/overview/Floating dwell, warm preview, exclusions, focus, "
                     "hit testing and cancellation passed\n";
        OleUninitialize();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        OleUninitialize();
        return 1;
    }
}
