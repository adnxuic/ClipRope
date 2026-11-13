#pragma once
#include "presentation/image_card_layout.hpp"
#include "platform_win/fluent_tooltip.hpp"
#include <windows.h>


namespace cliprope::win {
// Textual image metadata shares the inert Fluent tooltip host.
class ImageTip {
  public:
    void hide() { tooltip_.clear(); text_.clear(); }
    void update(HWND owner, const application::CardSummary* card) {
        if (!card || !presentation::adaptive_image(*card)) { hide(); return; }
        UiText text = card->file_list_hash.empty() ? tr(TextKey::card_clipboard_image) : UiText(card->text);
        if (!card->location.empty()) text += L"\n" + card->location;
        if (!card->capabilities.empty()) text += L"\n" + card->capabilities;
        if (card->availability == domain::FileAvailability::on_request) text += L"\n" + tr(TextKey::card_original_refresh);
        if (card->availability == domain::FileAvailability::decode_failed) text += L"\n" + tr(TextKey::card_preview_decode_failed);
        POINT pointer{};
        if (!GetCursorPos(&pointer) || !ScreenToClient(owner,&pointer)) { hide(); return; }
        const int inset=fluent::px(owner,8);
        const RECT area{pointer.x-inset,pointer.y-inset,pointer.x+inset+1,pointer.y+inset+1};
        if (text != text_) {
            hide();
            text_ = std::wstring(text.view());
        }
        tooltip_.track(owner,owner,text,false,420,&area);
    }
  private:
    fluent::Tooltip tooltip_;
    std::wstring text_;
};
} // namespace cliprope::win
