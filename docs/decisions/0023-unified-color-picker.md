# ADR 0023 · Unified color picker

Historical schema upgrade procedures in this decision were superseded by [ADR 0028](0028-current-storage-formats.md).

**Accepted · 2026-11-04.** One Win32/Direct2D/DirectWrite picker owns the four existing
color scenarios: active Find match, whole-card ink, paper tint and rope tint. It does
not add plaque colors, theme accents, selected-text formatting or a new toolbar.

**COLOR-04 revision:** opening always shows the complete picker from the contributor's
reference. There is no Compact/Advanced switch or recent-color tab. The immutable
Default choice remains the first common swatch, as explicitly confirmed after the
reference was supplied. It has a localized tooltip and native checked state; a
custom color matching the default RGB still does not select it.

The 336-DIP face has a title, large current swatch with HEX, small original swatch,
section dividers, common colors, a rounded 296 x 146 DIP SV field, hue, optional alpha,
HEX and one comma-separated RGB field. Slider values align to the right. The fixed
footer contains only Cancel and Apply, plus contextual validation or contrast feedback.
The warm Fluent container has a 12-DIP radius and an owned composited shadow.

Original, Draft and Committed remain separate. Preview changes rendering only.
Apply validates the original target and field, and commits once. A failed save retains
the draft for retry; dismissal removes its preview. A rope properties form accepts a
staged color and saves it with the rest of its fields. Recent colors are recorded only
inside the successful durable transaction. History order, snapshots and the clipboard
are unaffected. Paper alpha controls tint strength independently of whole-card opacity.

V11 replaces numeric sentinel fields with `default` or explicit `#RRGGBBAA` values.
Every pre-existing color becomes explicit. Old white paper becomes `#FBF7EFFF`.
Old zero-valued rope color becomes explicit `#C9A26BFF` with natural material, its five
original shades and its texture seed. Other rope materials preserve the old tint
formula and seed. The active database drops the obsolete columns. Migration is atomic;
the normal upgrade recovery backup remains available. New records inherit by default.
An explicit white or black never means Default, even if it matches a resolved token.

Default resolves the scene's style, not the settings window's surface color. The
paper renderer composites tint over the warm base and then draws the original fibers.
Changing color does not invalidate text layout or alter the editor's content undo stack.

The scene adapter keeps migrated natural rope material when its explicit selection is
unchanged. Choosing a custom color from Default uses dyed material, even if its RGB
equals the resolved default; choosing Default resolves natural material. Material identity is part of the rope
bitmap cache key, including every shade and the deterministic seed.

Card previews use a temporary entry-ID override, without reloading the viewport or
persisting its anchor. A moving card bypasses its cached face while previewing. Open
RichEdit fields suspend formatting undo and retain their selection and modification
flag. Durable saves compare the original color field and, for cards, the snapshot
and revision; they do not replace an entire style record. Native input ownership
includes the parent menu while the picker is active.

Incomplete HEX or RGB text remains visible across theme changes.
Only a complete legal color enables Apply; Enter in a field validates it. The footer
stays outside the scroll viewport. Color axes expose UI Automation values, and
selected preset radio controls expose their checked state independently of RGB.
Opaque colors display six-digit HEX, while translucent colors include trailing alpha.
The current swatch has no dropdown affordance; the original swatch restores the
original draft when clicked. Full contrast details, including the desktop-background
exclusion, are exposed on the current swatch tooltip and accessibility description.
Stored recent-color history and v11 migration remain compatible, but this panel no
longer queries or displays that history. The attached critique was context; its
suggestion to remove Apply/Cancel was superseded by the explicit screenshot request.

[Implementation and evidence](../../Tasks/COLOR-picker.md)
