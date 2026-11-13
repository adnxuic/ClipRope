# ADR 0021 · English and Simplified Chinese

**Accepted · 2026-11-04.** One desktop implementation supports English and Simplified
Chinese. General offers Auto, English and 简体中文. Auto follows the ordered Windows
preferred UI language list. A manual override survives restart. Global reset restores
Auto in the same transaction as the other global preferences. This supersedes the
reserved selector in [ADR 0020](0020-general-settings.md).

## Resource and dependency boundary

The UTF-8 catalogs in `resources/i18n` are the maintained source. A single Python parser
validates them and generates typed `TextKey` identifiers and immutable UTF-16 C++ tables
in the build tree. Every ordinary build requires complete catalogs. A developer can
explicitly allow missing Chinese entries; pinned release configurations reject that
option. No JSON parser, resource file loading or separate language executable is needed
at runtime.

`localization::LocalizationManager` is independent of Windows, SQLite and graphics.
The host owns it and injects read access into presentation and rendering. The native
adapter supplies Windows detection and a UI-thread binding for reusable native controls.
The domain has no localization dependency. Plain strings containing user data never
become translation requests by matching their contents.

English is the complete base catalog. Missing Chinese entries fall back to English.
Failed lookup or formatting displays `[key]` and emits an English diagnostic containing
the key, without user values. Named arguments can change order; `{{` and `}}` escape
braces. Separate singular and plural keys cover the current English messages.
Static views remain valid for the process lifetime, including across switches.

## Selection and commit

The platform calls [GetUserPreferredUILanguages](https://learn.microsoft.com/en-us/windows/win32/api/winnls/nf-winnls-getuserpreferreduilanguages)
with language names and scans the returned order. `en` and its variants match English.
Explicit `zh-Hans` matches Chinese, including regions that otherwise imply Traditional
Chinese. Explicit `zh-Hant` is unsupported and scanning continues. Without an explicit
script, `zh-CN` and `zh-SG` match Chinese. Unsupported lists and detection failure use
English. Startup, settings notifications and session recovery recalculate Auto.

The optional singleton `localization_preferences` stores `auto`, `en-US` or `zh-CN`.
Missing rows and unknown values mean Auto. It does not advance core history schema v10.
The sequence is **commit preference, update manager, post language event, refresh UI**.
A rejected commit retains the old manager and selection and reports the error in the
old language. Only an effective-locale change increments the language revision.

## Runtime preservation

Native text bindings and custom paint descriptors retain typed resource identities.
Events update titles, labels, tooltips and accessible names/descriptions in place.
Menus resolve new strings on open; open editable menus relabel their stable items.
Settings keeps its page and scroll position. Native edit fields keep their HWND, draft,
selection and composition state. Cached card summaries contain semantic state and raw
user data; display labels are resolved without rewriting snapshots or search indexes.

The shared UI font policy prefers Segoe UI Variable Text for English and Microsoft
YaHei UI for Chinese, falling back to available system fonts. DirectWrite uses
[system font fallback](https://learn.microsoft.com/en-us/windows/win32/api/dwrite_2/nf-dwrite_2-idwritetextformat1-setfontfallback).
User content editors keep their chosen font and style. Locale/revision participate in
text cache identity. Only text formats/layouts and artwork containing UI text expire;
source image thumbnails and graphics devices survive. Menus measure translated labels,
settings wrap descriptions and confirmations measure their complete body.

Language changes do not reinitialize history, listeners, capture workers or shortcut
registration. Clipboard contents, filenames, paths, custom rope names, `main`, `ReBin`
and Windows format identifiers remain literal. Installer, command-line output and
internal diagnostics remain English. Native Windows picker controls follow Windows.

## Evidence and maintenance

See the [developer workflow](../developer/localization.md),
[stage plan and actual acceptance results](../../Tasks/I18N-bilingual.md) and
[qualification fixture](../../tests/integration/localization_desktop.cpp).
Release acceptance includes ordered detection, failure rollback, live state preservation,
both-language theme/DPI captures, resource retention and the existing menu latency gates.
Unavailable desktop, Office or physical IME conditions remain explicit open checks.
