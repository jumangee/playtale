Lucide and Feather icons keep their original licenses.
The licenses and copyright notices are in `freeink-sdk/libs/assets/Icons/lucide/LICENSE` at the repository root.
Keep those notices with copies of those icons, including converted bitmaps.

## Orientation

The `.h` assets here are produced by `scripts/convert_icon.py`, which rotates the
artwork 90 degrees counterclockwise before packing (1 bpp, bit 0 = ink). That
layout is what `GfxRenderer::drawIcon` expects, and it is **not** the layout a
FreeInkUI `BitmapRef` expects: `BitmapRef` pixels are row-major natural
orientation. An asset from this directory used in a list row, dialog or button
must therefore be converted first — `icon_orientation::transpose` in
`src/components/UiAppHelpers.h` — either directly, or through
`listIconFor()`/`storygameIcon32()`/`storygameIcon24()`, which do it once. Wrapping
one of these arrays in `freeink::Icon` by hand draws it on its side, and declaring
a 32x32 `Icon` over a 24x24 array additionally reads past the end of the data.
