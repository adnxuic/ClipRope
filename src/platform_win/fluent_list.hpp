#pragma once
#include "platform_win/fluent.hpp"

namespace cliprope::win::fluent {
// Native controls remain the authority for items, selection, keyboard input and
// accessibility. Their HWNDs continue accepting the normal LB_/CB_ messages.
HWND list_box(Surface& surface, HWND parent, UiText name, int id);
HWND choice_box(Surface& surface, HWND parent, UiText name, int id);
int choice_height(HWND choice, int face_height = 32);
} // namespace cliprope::win::fluent
