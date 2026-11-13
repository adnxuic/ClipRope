# Global, hovered-row and focused-window shortcuts

**Accepted.** This decision updates the keyboard scope in section 8 of the original implementation plan. A user can keep an external editor active while navigating the visible workspace. The original Chinese plan remains a historical source.

| Scope | Default keys | Result |
| --- | --- | --- |
| Global | Ctrl+Shift+V | Toggle workspace visibility and its taskbar entry. |
| Global | Ctrl+Shift+Space | Create one independent Floating text card per press and focus its editor. |
| Hovered visible row or focused workspace body | Tab | Select the next collector in circular order. |
| Hovered visible row or focused workspace body | Ctrl+Tab | Toggle overview and its complete list's first rope in single view. |
| Hovered visible row or focused workspace body | Ctrl+F | Open Find in Cards and focus the search field. |

Settings → Shortcuts offers an enable switch, an action label, a chord recorder and a **Check availability** button for each action. Click the recorder or focus it and press Enter. Release the main key to apply the chord. Escape or focus loss cancels recording. Only the displayed combination matches. Each scoped switch covers both hover and explicit workspace focus. The tray and rope menu commands remain accessible independently.

Each shortcut fits on one row. A successful check displays **✓ Registered**, **✓ Available** or **Available (off)** in the button for three seconds. Checking again restarts that interval. A saved conflict appears as **Unavailable** on entry. A check with an incomplete or failed result shows **Not verified**, **Limited** or **Unavailable**, with the reason next to the button. Escape dismisses the tip. Recording and save errors appear beside the control that initiated the change. The native buttons retain keyboard focus and Invoke, and the settings viewport announces the action name and result to automation clients. Feedback updates leave row geometry and the scroll offset in place. [Idle layout](../../tests/compatibility/fluent/shortcuts-compact-light.png) · [Successful check](../../tests/compatibility/fluent/shortcut-registered-light.png)

The hovered scope uses the row's full visible width, including its rope, cards and transparent gaps. A window above the workspace, including a Floating card or an inline sheet, owns its covered area. Rows outside the visible workspace have no scope. A menu, mouse capture, card or rope gesture, or focused ClipRope dialog suspends hover navigation. Unbound combinations pass through.

The initial main-key press binds the press to either its existing recipient or the workspace. A captured press executes once and consumes its repeats and final release. Moving the pointer, changing modifiers, rebinding or hiding the workspace during that press preserves its ownership. A press begun outside the scope stays with its recipient until release. A focused workspace uses its local keyboard route so a hover cannot execute the command a second time.

Tab selects through the existing persisted collector command. A rejected write keeps the previous collector and reports the failure. In overview, selecting a collector rotates the circular switching order so its row is first, while keeping the overview open. Scrolling only changes which rows are visible. These navigation commands preserve card ownership and order. A collector change follows the [clipboard switch setting](0024-collector-head-clipboard.md). Hover navigation preserves the external foreground window, editable field and selection. [Clouds and collector order](0012-rope-clouds.md)

## Registration and storage

Each action has a stable ID in the optional SQLite shortcut preferences table. Missing rows use the built-in binding for that action. ClipRope reports the affected action by name. Disabled actions retain their saved chord.

The tray acquires a replacement global registration before committing the new preference and releasing the old registration. A failed save releases the candidate. An availability check retries an enabled registration or briefly probes and releases a disabled one. The scoped check reports the installation state of the hover hook. Before the workspace first opens it reports that verification is pending.

## Windows adapter

The focused message handler and the hovered-row input thread use the same chord map. The keyboard hook tracks key-down and press ownership and posts a command to the UI thread. It reuses the visible row's wheel bounds and z-order checks. A temporary focused recorder captures candidate keys before Windows dispatches registered global shortcuts.

The first show installs the service. Hidden workspaces disable routing and discard pending commands while the service can still finish an owned key release. A generation check rejects messages crossing suspension or a new gesture, and execution rechecks dialog, menu and capture state. Shutdown removes both hooks and joins their thread. Keyboard installation errors are reported through a dedicated workspace callback and a tray notification with the Windows error code. Mouse and focused-window navigation remain available, and reopening retries the failed installation.

The two global actions use Windows registration and repeat suppression. Scoped actions use row hit testing on the initial press. [Registration contract](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registerhotkey) · [Keyboard hook contract](https://learn.microsoft.com/en-us/windows/win32/winmsg/lowlevelkeyboardproc)

[Tasks and qualification](../../Tasks/KEY-shortcuts.md) · [Find in Cards](0017-find-in-cards.md)
