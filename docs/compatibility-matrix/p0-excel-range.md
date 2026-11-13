# P0-06 · Excel worksheet range

Two consecutive runs on 2026-11-01 used Windows 11 25H2 x64 build 26200.9711, Excel Home and Student 2021 x64 file version 16.0.20527.20112 and the Debug x64 probe. Both inventories and all cell checks agreed. The fixture opened a new Excel process and disposable workbook on a noninteractive test desktop.

The source was a two-cell row with a bold, yellow formula cell (`=2+3`) and adjacent text. Excel copied the range twice. The first copy went directly to D3 via Excel's native worksheet Paste. For the second copy, the probe bound the source PID, captured its OLE data, verified an intervening clipboard overwrite, republished the saved formats and flushed them. Excel pasted into D5 after the publisher process exited.

| Observation in the destination workbook | Direct Excel paste | Probe restore |
| --- | --- | --- |
| Value 5 and adjacent text in separate cells | Preserved | Preserved |
| Source formula | Preserved | Lost, only the value survived |
| Bold face and yellow cell fill | Preserved | Preserved |
| Replace pasted cell with a new formula, evaluate to 7 | Passed | Passed |

All 24 offered descriptors reported content aspect and index -1. The probe saved two HGLOBAL formats, `HTML Format` and `CF_UNICODETEXT`. The following inventory groups the skipped descriptors by their actual medium.

| Medium | Skipped formats |
| --- | --- |
| HGLOBAL | Biff12, Biff8, Biff5, CF_SYLK, CF_DIF, XML Spreadsheet, CF_TEXT, Csv, ShadowWorkbook, Web Custom Format Map, Rich Text Format, Object Descriptor, Link Source Descriptor, Link, unnamed format 129 (CF_DSPTEXT), ExcludeClipboardContentFromMonitorProcessing |
| IStream | Hyperlink, Link Source |
| IStorage | Embed Source |
| GDI, MFPICT, ENHMF | CF_BITMAP, CF_METAFILEPICT, CF_ENHMETAFILE respectively |

The private workbook exercised an explicit adapter probe. The offered exclusion marker belongs to the policy checks for passive history capture. P0 measured the worksheet-to-worksheet paths above. Source exit before restore, durable application restart, other Office objects and other destinations are untested in this run.

## Reproduce

Use the [build setup](../developer/build.md) and a standard-user Windows desktop with Excel installed. Enable the opt-in CTest and run it twice. For the native route and restore checks, see the [fixture](../../tests/compatibility/office_range.ps1) and the [capture/overwrite probe](../../tests/compatibility/office_restore.cpp).

```powershell
cmake -S . -B build/p0-baseline -DCLIPROPE_TEST_OFFICE=ON -G "Visual Studio 17 2022" -A x64
cmake --build build/p0-baseline --config Debug
ctest --test-dir build/p0-baseline -C Debug -R P0-06 --repeat until-fail:2 -V
```
