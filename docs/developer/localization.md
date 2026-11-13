# Maintaining desktop translations

The desktop has one UI and two compiled catalogs. General selects Auto, English or
简体中文. Auto follows supported Windows UI preferences in order. A save failure retains
the previous selection and language. [Decision and boundaries](../decisions/0021-bilingual-localization.md)

## Add text

1. Add a stable dotted key to both UTF-8 files under `resources/i18n`. Use a feature
   prefix such as `settings`, `card`, `rope` or `error`. Keep user content out of catalogs.
2. Reference the generated identifier with `tr(TextKey::settings_language)` in native UI,
   or `manager.tr(TextKey::settings_language)` in presentation. `UiText` retains resource
   identity across refreshes. A raw `std::wstring` remains literal.
3. For dynamic text, use `message(key, {{"count", value}})` in native UI or
   `manager.format(key, {{"count", value}})` with owned `std::wstring` arguments.
   A placeholder is `{name}`. Escape literal braces with `{{` and `}}`. Select separate
   singular/plural keys using numeric state. Do not concatenate translated fragments
   when grammar or argument order can vary.
4. Use native `set_ui_text` / `set_ui_item_text` and `accessible_name` /
   `accessible_description` for application-owned labels. Bind no resource to an edit
   field holding a user's text. Use enums, indices and booleans for control state.
5. Run validation and affected tests. Update both catalogs in the same change.

```powershell
python tools/check_i18n.py
cmake --build --preset windows-qualification -- /m:1 /nr:false
ctest --preset windows-qualification -R '^I18N-' --output-on-failure
```

Python 3.10 or newer is a build-time dependency. The generated header and table live in
`<build>/generated/i18n`, never the source tree. The checker rejects duplicate/invalid
keys, invalid UTF-8/UTF-16 values, empty values, key differences and incompatible named
arguments. It is also the code generator, so validation and compilation share syntax.
For an incomplete development draft only, configure a non-release build with
`-DCLIPROPE_I18N_STRICT=OFF`, or run the checker with `--allow-incomplete`.
English keys remain mandatory. Both release presets enforce completeness.

## Refresh a surface

The host owns `LocalizationManager`. Its native `UiLocalization` binding lasts for the
UI-thread scope. Reusable native fixtures without a binding use English. Presentation
receives explicit read access. The manager changes only after a preference transaction
commits, and events stay on the UI thread. Effective-language changes increment its
revision; selecting Auto that resolves to the same locale does not.

Handle `win::language_changed` by relabeling and measuring existing controls. The shared
Surface subclass refreshes bound labels, fonts and accessibility metadata. Preserve
current page, scroll, focus and edit state. Do not rebuild an editor or call a history
refresh merely to translate a cached summary; use the semantic presentation formatter.
Include locale/revision in any cache that contains application text. Invalidate text
artwork without discarding thumbnails, the graphics device or user font choices.

Assign UI font roles explicitly with `Surface::font`, `field` or `card_style`. The
shared refresh visits only tagged controls. Untagged content editors retain their own
font and character formatting. Inline editing relabels and measures its surrounding
notice separately, preserving the native field's scroll and selection.

Internal exception messages remain English. Map known failures to error keys for the UI;
unknown failures use the generic translated error. Diagnostics may record key/error
identity, never interpolation values or clipboard bodies. `main`, `ReBin`, custom names,
paths and registered clipboard format identifiers are literal, including in reports.

## Qualification

`I18N-01` covers pure matching, formatting, fallback and validator failures. `I18N-02`
covers old profiles, writes, reset, restart and backup. `I18N-03` covers semantic reports,
typed menu commits and native text bindings. `I18N-04` launches the actual desktop on a
private desktop with capture paused, retaining a synthetic draft while switching.
Its second case forces the preferred UI font to be unavailable. Run the native tests
from an account permitted to create desktops.

```powershell
python tests/compatibility/localization_visuals.py --out build/i18n-visuals
python tests/compatibility/localization_visuals.py --out build/i18n-fallback --fallback
```

The fixture uses stable window classes/control IDs and its disposable persisted language
selection. It prints its own windows without global keyboard/mouse input. The matrix
covers both languages, Light/Dark/high-contrast design palettes, 96/120/144/192 design
DPI, narrow Settings, a long reset confirmation, Unicode content and font fallback.
These design overrides do not replace real mixed-monitor DPI or physical IME acceptance.
The older English-caption fixtures explicitly set `CLIPROPE_PROBE_UI_LANGUAGE=en-US`.
That probe overrides only Auto detection, is compiled out of production, and cannot
override a manual preference. Theme, DPI and font probes follow the same rule.

Run the [menu latency workload](../../tests/performance/README.md#context-menu-latency)
with `--language en-US` and `--language zh-CN` on an unlocked idle desktop. Each workload
seeds its own manual preference, so it also works with the production executable.
Record startup readiness, process scope, Private Bytes and post-warm-up resource counts.
Keep unavailable conditions separate from passing evidence in the
[phase record](../../Tasks/I18N-bilingual.md).
