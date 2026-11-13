#include "platform_win/inline_editor_host.hpp"
#include <ole2.h>
#include <richedit.h>
#include <iostream>

using namespace cliprope;
void require(bool ok, const char *reason) {
    if (!ok)
        throw std::runtime_error(reason);
}
std::wstring text(HWND h) {
    wchar_t value[256]{};
    SendMessageW(h, WM_GETTEXT, 256, LPARAM(value));
    return value;
}
int main() {
    try {
        OleInitialize(nullptr);
        auto owner = CreateWindowExW(0, L"STATIC", L"Synthetic color editor", WS_OVERLAPPEDWINDOW, 50, 50,
                                     400, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ShowWindow(owner, SW_SHOW);
        {
            win::InlineEditorHost editor;
            application::CardSummary card;
            card.entry.id = "color-editor";
            editor.bind(
                owner, {card.entry.id, 0, L"Original text", false}, card,
                [](const auto &, auto) {
                    throw std::runtime_error("Color preview saved text");
                    return domain::Id{};
                },
                [] { return true; }, [](auto) {});
            const RECT bounds{100, 100, 380, 260};
            editor.place(bounds, bounds, true);
            auto host = FindWindowW(L"ClipRope.InlineEditorHost", nullptr), field = GetDlgItem(host, 1760);
            require(field != nullptr, "Inline field missing");
            SendMessageW(field, EM_SETSEL, 0, 0);
            SendMessageW(field, EM_REPLACESEL, TRUE, LPARAM(L"New "));
            const auto selection = SendMessageW(field, EM_GETSEL, 0, 0),
                       modified = SendMessageW(field, EM_GETMODIFY, 0, 0);
            auto style = card.style;
            style.text_color = 0x123456;
            style.background_color = domain::ColorRGBA8{0, 80, 255, 128};
            for (int i = 0; i < 100; ++i) {
                style.text_color = unsigned(0x123400 + i);
                editor.colors(style);
            }
            require(text(field) == L"New Original text" && SendMessageW(field, EM_CANUNDO, 0, 0) &&
                        SendMessageW(field, EM_GETSEL, 0, 0) == selection &&
                        SendMessageW(field, EM_GETMODIFY, 0, 0) == modified,
                    "Preview damaged text, selection or undo");
            editor.colors(card.style);
            SendMessageW(field, EM_UNDO, 0, 0);
            require(text(field) == L"Original text", "Color edits entered the text undo history");
            editor.discard();
        }
        DestroyWindow(owner);
        OleUninitialize();
        std::cout << "Inline color preview preserves selection, text and undo\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
