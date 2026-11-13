# ADR 0017 · Find in Cards

**Accepted.** Open **Find in Cards** from the penultimate rope menu action or press Ctrl+F in the focused workspace. Hovering an unobscured visible rope row also owns Ctrl+F by default. The **Find in Cards** switch and recorded chord in Settings → Shortcuts apply to both routes and persist across restarts. The existing row hook owns the entire press, including repeats and release. Editors, dialogs, gestures and out-of-scope applications retain their keyboard routes.

The activatable owned popup gives its native Unicode field focus. Enter and Down select the next occurrence. Shift+Enter and Up select the previous one. Both directions wrap. IME composition owns its keys first. Ordinary focus loss retains the query. Clear the field to clear matches, or use Escape or × to close and leave the viewport at the current card. Closing the workspace ends the session.

Single view searches every Hanging entry of its displayed rope. Overview searches the navigable circular rope order beginning at its head, including ReBin only while shown. Entry order follows the permanent Hanging sort order, then saved field order and nonoverlapping occurrences. A copied entry has its own match positions even when it shares an immutable snapshot. Literal English ASCII comparisons ignore case. Other characters compare exactly.

Capture and edit transactions install each snapshot’s searchable fields before commit. Binding a screenshot origin updates its index in the same transaction. Find opens a read-only WAL connection to count matches. The scope carries the session's ReBin visibility. Only relevant saved representations supply body text, anchor targets, paths, virtual filenames and source metadata. File contents and image pixels are outside the search scope.

Locating a match checks the entry, rope and snapshot again. The worker keeps counts per field and a bounded page of nearby occurrence contexts. The workspace realizes the target page directly. Search never invokes history promotion, clipboard publication, paste delivery or collector selection. Ordinary navigation and paste commands keep their existing contracts.

The renderer highlights the actual UTF-16 layout ranges. The active occurrence uses the saved global match color. [Appearance settings](0019-appearance-and-profile-location.md) A clipped body temporarily shows surrounding text. A hidden filename or metadata field appears inside the existing card bounds. Closing removes these transient surfaces. Drafts retain their view and become searchable on commit. After a history change, retain the surviving occurrence or move to its next survivor in order. A generation change discards obsolete work.

[Qualification](../../Tasks/FIND.md) · [Shortcut scopes](0011-keyboard-scopes.md) · [ReBin](0014-rebin.md)
