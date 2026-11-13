# ADR 0024 · Follow the current collector’s first card

By default, the clipboard follows the first Hanging card on the current collecting rope. Two global switches control when ClipRope copies that card’s saved formats.

| Committed result of a manual command | Setting and effect |
| --- | --- |
| The collecting rope changed | Apply **Ropes → Update clipboard when switching ropes**. Copy the first card on the selected rope, including a visible ReBin. |
| The collector is the same and its first entry or saved snapshot changed | Apply **Cards → General → Update clipboard when the first card changes**. Copy the new first entry or the newly saved head text. |
| Empty result or identical collector, entry and snapshot | Keep the existing clipboard. |

Moving the first entry from A to B while A collects copies A’s successor. Returning a Floating entry, replacing a file group and recycling into a collecting ReBin use the same rule. A canceled gesture, failed transaction, edit draft, appearance change or scroll produces no publication. Starting ClipRope preserves the current system clipboard. Capture retains the external owner’s formats. Explicit recall and Hanging double-click publish once through their existing routes.

The writer reads both identities in the same transaction. After commit, the desktop loads the resulting snapshot with a lease and publishes on the UI STA using the existing self-write detection. An external capture already in progress finishes on its bound rope first. A deferred publication must still match the latest collector head and clipboard sequence. A newer external copy takes precedence. Automatic publication never dispatches a paste shortcut or collapses the workspace. Publication failure leaves the committed history available and reports **Clipboard was not updated**.

Existing profiles default both switches on. The optional singleton `clipboard_follow_preferences` stores them together. A successful save applies immediately. Global reset deletes the row in the existing settings transaction. The core schema stays at v11.
