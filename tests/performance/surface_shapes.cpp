#include "process_metrics.hpp"
#include "../compatibility/workspace_fixture_frame.hpp"
#include "platform_win/floating.hpp"
#include "platform_win/menu_bitmap.hpp"
#include "platform_win/workspace.hpp"
#include "rendering/workspace_paint.hpp"
#include <ole2.h>
#include <dwmapi.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace cliprope;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

void check(bool condition, const char* detail) {
    if (!condition) throw std::runtime_error(detail);
}
RECT client(HWND window) {
    RECT result{};
    check(GetClientRect(window, &result) != FALSE, "Could not read fixture surface dimensions");
    return result;
}
bool shown(HWND window) { return (GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) != 0; }
void settle(unsigned milliseconds) {
    const auto end = GetTickCount64() + milliseconds;
    do {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const auto now = GetTickCount64();
        if (now >= end) return;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(end - now), QS_ALLINPUT);
    } while (true);
}
void resize_and_draw(HWND window, int width, int height) {
    check(SetWindowPos(window, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER) != FALSE,
          "Could not set the fixture window size");
    check(RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW) != FALSE, "Floating redraw failed");
    const auto bounds = client(window);
    check(bounds.right == width && bounds.bottom == height, "Floating frame did not use the requested geometry");
}

void floating_frame(HWND window, const fs::path& path) {
    const auto r = client(window);
    win::MenuBitmap capture;
    check(capture.create(r.right, r.bottom), "Could not allocate capture DIB");
    SendMessageW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(capture.dc), PRF_CLIENT);
    GdiFlush();
    const DWORD pixels = r.right * r.bottom;
    const auto first = capture.pixels[0] & 0xffffff;
    check(std::count_if(capture.pixels, capture.pixels + pixels,
                        [=](auto pixel) { return (pixel & 0xffffff) != first; }) > pixels / 100,
          "Floating frame contains no visible card");
    const DWORD bytes = pixels * 4;
    const BITMAPFILEHEADER header{0x4d42, 54 + bytes, 0, 0, 54};
    const BITMAPINFOHEADER info{sizeof info, r.right, -r.bottom, 1, 32, BI_RGB};
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&header), sizeof header);
    out.write(reinterpret_cast<const char*>(&info), sizeof info);
    out.write(reinterpret_cast<const char*>(capture.pixels), bytes);
    check(bool(out), "Could not write Floating frame");
}

application::CardSummary card(std::string id, std::wstring text) {
    application::CardSummary result;
    result.entry.id = std::move(id);
    result.entry.snapshot_id = result.entry.id + "-snapshot";
    result.text = std::move(text);
    result.style.opacity = .9;
    return result;
}

void run(const fs::path& output, bool large) {
    fs::create_directories(output);
    std::ofstream csv(output / "surface-shapes.csv");
    csv << "trial,stage,stage_ms,workspace_w,workspace_h,primary_w,primary_h,workspace_dpi,primary_dpi,shown_workspace,shown_floating,"
           "workspace_frames,realized_cards,target_creations,bitmap_bytes,preview_bitmap_bytes,moving_face_bytes,"
           "pid,processes,fixture_private_bytes,worker_private_bytes,group_working_set_bytes,group_cpu_100ns,"
           "group_handles,group_threads,fixture_gdi,fixture_user,gpu_dedicated,gpu_shared,"
           "dwm_private,dwm_gpu_dedicated,dwm_gpu_shared,hanging_id,floating_entry_snapshot_ids,identity_checks,dib_width,dib_height,dib_allocations\n"
        << std::fixed << std::setprecision(4);
    check(bool(csv), "Could not open surface CSV");
    performance::GpuCounters gpu;
    auto hanging = card("wide-hanging", L"Hanging surface fixture");
    hanging.entry.rope_id = "main";
    hanging.entry.sort_key = 0;
    win::WorkspaceReads reads{[](auto) { return 1u; }, [&](auto, size_t offset, size_t count) {
        return !offset && count ? std::vector{hanging} : std::vector<application::CardSummary>{};
    }, [](auto, auto) { return std::optional<size_t>{0}; }, [](auto) { return application::Anchor{}; }, [](auto, auto&) {}};
    win::Workspace workspace(std::move(reads), {});
    check(workspace.open(), "Could not create fixture Workspace");
    std::array<std::unique_ptr<win::Floating>, 3> floating;
    std::string recipient, snapshot;
    for (size_t i = 0; i < floating.size(); ++i) {
        win::FloatingActions actions;
        actions.context = [&](application::CardMenuSnapshot target, POINT) {
            recipient = target.entry.id;
            snapshot = target.entry.snapshot_id;
        };
        floating[i] = std::make_unique<win::Floating>(std::move(actions));
        const auto model = card("shape-" + std::to_string(i), L"Floating shape " + std::to_wstring(i));
        check(floating[i]->open(model, {"", 40. + i * 40., 55. + i * 30., 300, 180}, false),
              "Could not create fixture Floating HWND");
    }
    const auto primary = floating.front()->window();
    unsigned identity_checks = 0;
    std::string hanging_id, floating_ids;
    auto identity = [&] {
        hanging_id.clear();
        floating_ids.clear();
        for (size_t i = 0; i < floating.size(); ++i) {
            recipient.clear();
            snapshot.clear();
            // Send directly to owned HWNDs. No native pointer movement, menu or clipboard operation.
            SendMessageW(floating[i]->window(), WM_CONTEXTMENU, WPARAM(floating[i]->window()), MAKELPARAM(40, 40));
            const auto expected = "shape-" + std::to_string(i);
            check(recipient == expected && snapshot == expected + "-snapshot", "Floating entry/snapshot receiver changed");
            if (i) floating_ids += '|';
            floating_ids += recipient + "@" + snapshot;
            ++identity_checks;
        }
        if (workspace.visible()) {
            const auto slots = workspace.slots();
            check(slots.size() == 1 && slots.front().first == hanging.entry.id, "Visible Hanging identity changed");
            hanging_id = slots.front().first;
            ++identity_checks;
        }
    };
    auto sample = [&](int trial, const char* stage, Clock::time_point start) {
        const auto stage_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        identity();
        const auto group = performance::process_tree(GetCurrentProcessId());
        const auto graphics = gpu.read(group.ids);
        const auto wr = client(workspace.window()), fr = client(primary);
        const auto count = std::count_if(floating.begin(), floating.end(), [](const auto& f) { return shown(f->window()); });
        csv << trial << ',' << stage << ',' << stage_ms << ',' << wr.right << ',' << wr.bottom << ',' << fr.right
            << ',' << fr.bottom << ',' << GetDpiForWindow(workspace.window()) << ',' << GetDpiForWindow(primary)
            << ',' << workspace.visible() << ',' << count << ',' << workspace.painted_frames()
            << ',' << workspace.realized_count() << ',' << rendering::WorkspacePaint::target_creations()
            << ',' << rendering::WorkspacePaint::resident_bitmap_bytes() << ',' << rendering::WorkspacePaint::preview_bitmap_bytes()
            << ',' << rendering::WorkspacePaint::moving_face_bytes() << ',' << GetCurrentProcessId() << ',' << group.processes
            << ',' << group.host_private << ',' << group.worker_private << ',' << group.working_set << ',' << group.cpu_ticks
            << ',' << group.handles << ',' << group.threads << ',' << GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS)
            << ',' << GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS) << ',' << graphics.dedicated << ',' << graphics.shared
            << ',' << graphics.dwm_private << ',' << graphics.dwm_dedicated << ',' << graphics.dwm_shared
            << ',' << hanging_id << ',' << floating_ids << ',' << identity_checks << ','
            << rendering::frame_backing().width << ',' << rendering::frame_backing().height << ','
            << rendering::frame_backing().allocations << '\n';
        check(bool(csv), "Could not record surface sample");
    };
    sample(0, "hidden_start", Clock::now());
    floating.front()->show();
    if (large) {
        std::ofstream timings(output / "large-frames.csv");
        timings << "trial,shape,draw_ms,draw_and_fence_ms,retained_w,retained_h,allocations,private_bytes\n";
        auto draw = [&](unsigned trial, const char* shape, int width, int height) {
            const auto start = Clock::now();
            const auto before = floating.front()->presented_frames();
            resize_and_draw(primary,width,height);
            check(floating.front()->presented_frames() > before, "Large surface did not present");
            const auto rendered = Clock::now();
            check(SUCCEEDED(DwmFlush()), "Compositor fence unavailable");
            const auto fenced = Clock::now();
            const auto backing = rendering::frame_backing();
            if (std::string_view(shape) != "full_4k")
                check(std::uint64_t(backing.width)*backing.height*4 <= 8ull*1024*1024,
                      "Backing retained the product of unrelated surface dimensions");
            timings << trial << ',' << shape << ','
                    << std::chrono::duration<double,std::milli>(rendered-start).count() << ','
                    << std::chrono::duration<double,std::milli>(fenced-start).count() << ','
                    << backing.width << ',' << backing.height << ',' << backing.allocations << ','
                    << performance::process_tree(GetCurrentProcessId()).host_private << '\n';
        };
        for (unsigned trial=0; trial<6; ++trial) {
            draw(trial,"wide",3840,432);
            draw(trial,"tall",432,2160);
        }
        // The physical monitor supplies the HWND DPI. Exercise DirectWrite and card geometry at 200% separately.
        win::MenuBitmap image;
        check(image.create(960,480), "High-DPI target unavailable");
        auto paint=rendering::WorkspacePaint::acquire();
        check(paint->draw_floating(image.dc,{0,0,960,480},192,card("large-dpi",L"High DPI"),rendering::FloatingControl::none,false),
              "200% card drawing failed");
        // A complete workspace frame must erase a preceding opaque surface, including its alpha.
        GdiFlush();
        constexpr DWORD previous_paper = 0xff387abc;
        std::fill_n(image.pixels, 960 * 480, previous_paper);
        check(paint->draw(image.dc, {0, 0, 960, 120}, 192, {}, {}), "Transparent workspace draw failed");
        GdiFlush();
        check(std::all_of(image.pixels, image.pixels + 960 * 120, [](DWORD pixel) { return pixel == 0; }),
              "Workspace retained pixels from the preceding surface");
        check(std::all_of(image.pixels + 960 * 120, image.pixels + 960 * 480,
                          [](DWORD pixel) { return pixel == previous_paper; }),
              "Workspace cleared pixels outside its bound rectangle");
        draw(0,"full_4k",3840,2160);
        workspace.show("main");
        sample(0,"mixed_large",Clock::now());
        workspace.hide();
        check(!workspace.visible() && shown(primary),"Hiding the workspace hid the independent Floating card");
        draw(0,"floating_while_hidden",300,200);
        floating_frame(primary,output/"visible-after-hide.bmp");
        ShowWindow(primary,SW_HIDE);
        const auto hidden=workspace.painted_frames();
        settle(1000);
        check(!workspace.visible() && !shown(primary) && workspace.painted_frames()==hidden,"Hidden workspace continued drawing");
        sample(0,"idle_after_large",Clock::now());
        const auto start=Clock::now();
        workspace.show("main");
        sample(0,"first_reopen",start);
        check(workspace.visible() && workspace.painted_frames()>hidden,"Workspace failed to present after hide");
        workspace_fixture::frame(workspace.window(),output/"reopened.bmp");
        check(bool(timings) && bool(csv),"Could not finish high-resolution series");
        return;
    }
    for (int trial = 1; trial <= 3; ++trial) {
        for (const auto& [stage, size] : std::array<std::pair<const char*, SIZE>, 2>{{{"wide", {960, 216}}, {"tall", {216, 640}}}}) {
            const auto start = Clock::now();
            resize_and_draw(primary, size.cx, size.cy);
            sample(trial, stage, start);
            floating_frame(primary, output / (std::to_string(trial) + "-" + stage + ".bmp"));
        }
    }
    auto started = Clock::now();
    workspace.show("main");
    floating[1]->show();
    floating[2]->show();
    settle(250);
    check(workspace.visible() && std::all_of(floating.begin(), floating.end(), [](const auto& f) { return shown(f->window()); }),
          "Mixed scene did not show the four fixture windows");
    sample(0, "mixed_settled", started);
    workspace_fixture::frame(workspace.window(), output / "mixed-workspace.bmp");
    started = Clock::now();
    workspace.hide();
    for (const auto& window : floating) ShowWindow(window->window(), SW_HIDE);
    settle(250);
    check(!workspace.visible() && std::none_of(floating.begin(), floating.end(), [](const auto& f) { return shown(f->window()); }),
          "Hide retained a visible fixture window");
    sample(0, "hidden_settled", started);
    const auto paints = workspace.painted_frames();
    started = Clock::now();
    settle(1000);
    check(workspace.painted_frames() == paints, "Hidden Workspace continued presenting frames");
    sample(0, "hidden_idle", started);
}
}

int wmain(int argc, wchar_t** argv) {
    const bool large=argc==3 && std::wstring_view(argv[2])==L"--large";
    if (argc != 2 && !large) return 2;
    const auto previous = GetThreadDesktop(GetCurrentThreadId());
    const auto name = L"ClipRope.SurfaceShapes." + std::to_wstring(GetCurrentProcessId());
    const auto isolated = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!isolated) return 2;
    if (!SetThreadDesktop(isolated)) { CloseDesktop(isolated); return 2; }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int code = 1;
    if (SUCCEEDED(OleInitialize(nullptr))) {
        try {
            run(fs::absolute(argv[1]),large);
            std::cout << "Wide/tall, mixed and hidden surface frames retained their entry/snapshot receivers\n";
            code = 0;
        } catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
        OleUninitialize();
    }
    SetThreadDesktop(previous);
    CloseDesktop(isolated);
    return code;
}
