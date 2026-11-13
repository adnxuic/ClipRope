# External drag copy

Hold Ctrl and drag the card body past the Windows drag distance for its monitor. ClipRope chooses the operation at that point. Releasing Ctrl during the drag retains the chosen operation. Escape cancels it.

| Visible workspace at lift | Result |
| --- | --- |
| Single rope, or hidden workspace with a Floating source | Start a copy-only OLE drag. The receiving application chooses a representation. The source retains its membership, order and window position. |
| Multi-rope overview | Place a new Hanging entry in the indicated slot on any visible rope, including the source rope. Release outside a rope to cancel. A Floating source retains its window and layout. |

An external session binds the source entry and revision, leases its immutable snapshot and passes an independent data object to Windows. The shell supplies the copy cursor and drag image. A successful OLE copy reports target acceptance. Consumer tests separately inspect the text, image or file that appeared. The command has no clipboard publication, native paste, history admission or workspace collapse. [Windows OLE drag and drop](https://learn.microsoft.com/en-us/windows/win32/api/ole2/nf-ole2-dodragdrop)

Text and rich content offer their validated saved formats. File references offer the original ordered paths with copy semantics. A missing member prevents transfer of the group. Pixel-only cards offer their saved formats alongside one file representation. For a DIB-only snapshot, editors can also request a full-resolution PNG encoded from those pixels. ClipRope uses a readable original screenshot path after matching its pixels, or supplies a named virtual file backed by the same PNG bytes. A known folder on its own cannot identify a file. The drag does not create a temporary PNG. [Shell transfer formats](https://learn.microsoft.com/en-us/windows/win32/shell/clipboard)

The STA binds the gesture serial, source HWND, entry revision and clipboard sequence before waiting behind capture. For DIB-only content it stages immutable pixels for the supervised encoder and keeps the source lease on the STA. Release, Escape, a new gesture, source retirement, clipboard replacement and exit invalidate preparation. A valid completion builds the Shell object and enters the copy-only OLE loop once. [Worker roles and measured trade-offs](0026-preview-and-drag-workers.md)

In overview, copying inserts an independent entry sharing the snapshot and inheriting the source style. The destination's capacity applies to the resulting order. On a full rope containing A, B and C, copying C to slot one leaves a new C, A and B. The original C was the tail. A failed transaction retains all three original IDs in order.
