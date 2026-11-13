#pragma once

#include <windows.h>

namespace fixture {
inline constexpr wchar_t class_name[] = L"ClipRope.DeliveryFields";
inline constexpr UINT command = WM_APP + 31;
inline constexpr UINT inspect = WM_APP + 32;
enum class Action { reset, caret, focus_button, focus_b, destroy_a, readonly_a, deny_query,
                    overwrite, suppress_after, suppress_without, paste_b, focus_ansi, focus_attachment };
enum class Query { changes_a, changes_b, a_unchanged, a_replaced, a_inserted, b_unchanged,
                   b_pasted, b_pasted_new, edit_attempts, ansi_png_pastes, a_without_selection, attachments };
int run_fields(DWORD parent_pid);
}
