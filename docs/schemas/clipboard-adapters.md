# Clipboard adapter contract, codec version 1

A retained representation identifies a standard clipboard ID or an exact registered UTF-8 name, a single concrete OLE medium, content aspect, an index, a codec version and an immutable Blob. The ordinal preserves source order where possible. Skipped and failed observations have no Blob. The existing [history schema](history.md) already stores these fields. Publication re-registers names on the current window station and validates the saved media before replacing the clipboard.

| Family | Durable bytes and publication medium |
| --- | --- |
| Unicode text and HTML Format | Validated text including its terminating null. HTML uses UTF-8 byte offsets. Both publish as `TYMED_HGLOBAL` with index -1. |
| Formatted text and pixels | The media adapter handles the retained RTF, PNG and packed DIB subsets. PNG can be captured as an HGLOBAL or as a seekable IStream at index -1. The exact saved medium is served on restore. An image preview is derived separately from the retained representation. |
| Disk files and directories | `CF_HDROP` stores ordered path references. History restoration requires available paths and prepares a copy drop effect. |
| Virtual files | `FileGroupDescriptorW` stores the bounded descriptor array as an HGLOBAL. Each `FileContents` representation stores the complete stream bytes and its zero-based index. Publication allocates an independent `TYMED_ISTREAM` for each request. |

## Virtual-file subset

The adapter accepts one to sixteen flat files with unique, case-insensitive UTF-16 names. It rejects directories, reparse points, path separators, device names, invalid characters and unterminated names. The descriptor array is trimmed to its actual records. The adapter preserves those records, including supported names and size information. A descriptor naming a file does not create a disk file or a `CF_HDROP` path.

Each content stream must support seeking to the beginning. Reads use 32 KiB blocks, up to 8 MiB per file and 32 MiB per group. The reader checks advertised sizes when present, bounded progress and the actual bytes read. A generic source format listing at index -1 can describe a virtual group. Its descriptor count determines the concrete indexed requests. The stored manifest records those actual requests.

If one file cannot be read or exceeds a bound, the entire virtual group is withdrawn. A surviving independent format such as Unicode text can still be admitted with partial status. Metadata names the failed indexed content and the withdrawn descriptor. Without any usable representation, capture creates no history entry.

When a source also supplies a valid `CF_HDROP`, the path group takes precedence. Companion virtual descriptors are marked as skipped and their content streams are never requested.

Keep ClipRope running when pasting virtual files. Windows can store one native handle per clipboard format. In the measured shutdown probe, a normal OLE flush reported success but left an unreadable indexed stream. On explicit exit ClipRope now removes its current virtual group under an ownership- and version-checked native clipboard lock. Independent scalar formats survive, including text supplied alongside the files. A virtual-only publication leaves no file descriptors on the clipboard after exit. Reopen ClipRope and activate the saved card to serve the complete group again.

The desktop debugger output and headless exit report record the native clipboard effect, number of installed companions, HRESULT and Windows error code when available. A contended native open reports the error returned by Windows. A failing OLE call without an extended error reports `win32=unavailable`. A failed first installation after emptying the clipboard reports `cleared`. A failure after one or more successful installations reports `partial`. An intentional virtual-only flatten reports `portable-formats` with `installed=0/0`. The saved snapshot retains the descriptor and every saved indexed stream in each case.

## Shared capture and publication rules

* Capture bounds enumeration and the manifest at 256 descriptors and retained payloads at 64 MiB per copy. Each scalar adapter also checks its own byte and structural limits. Sequence and owner checks cover indexed reads as well as scalar reads.
* An ordinary history ticket checks clipboard privacy markers before payload extraction. The explicit full-format compatibility probe is for controlled synthetic inputs. The P0 probe keeps its original Unicode/HTML selection for its historical matrix.
* If Windows OLE format enumeration returns `E_OUTOFMEMORY` for an eligible local copy, the worker can recover native plain Unicode. Both privacy-marker checks must have a conclusive result. With the native clipboard locked, it verifies the ticket version and owner, the entire format inventory, the byte limit and the UTF-16 payload. Unicode must be the first format and only the Windows text, OEM text and locale companions are permitted. The worker checks the owner and version again before returning the bytes. This path records partial capture and retains the OLE error. A registered format, unknown marker response, stale ticket or invalid text prevents recovery. [Worker recovery evidence](../decisions/0002-external-capture-worker.md)
* Publication checks every retained type, codec version, aspect, index and medium. It rejects duplicate format/index pairs or an incomplete virtual group before setting a new OLE object. Presentation can use the same validation without publishing. The serving object retains the publication lease. A fresh stream starts at offset zero and can outlive the external producer.
* A native clipboard reader may hold the station lock while asking the publication STA to render. On shutdown the publisher services pending messages during a bounded wait and rechecks ownership before each flush attempt.
* An OLE enumeration allocation error with no saved formats can trigger up to two fresh object acquisitions within the original capture deadline. Each checks the original owner and sequence again. The acquisition stops before reading data if either privacy result is inconclusive. A later successful full capture retains the failure and re-acquisition counts in the content-free diagnostic receipt.
* The first local-history acquisition applies the same privacy check. For either marker, DV_E_CLIPFORMAT and DV_E_FORMATETC indicate absence. Recognized contention follows the bounded retry path. Any other inconclusive QueryGetData result yields `privacy-unconfirmed` with the original HRESULT, before enumeration, payload reads or admission. A present monitor exclusion or a history flag that denies collection still yields `excluded-marker`.

## Windows evidence

`P5-01.adapter_bounds` enumerated indices 0 and 1 directly from the provider and rejected incomplete groups, duplicate indices, invalid paths, incorrect lengths and unknown codec versions. `P5-01.flush_native_reader` held the clipboard open from a native reader and checked both the delivered text and the successful shutdown flush. `P5-01.adapter_contract` captured delayed Unicode and HTML, closed the producer and capture host, reopened the store, and verified both HGLOBAL types and bytes from another process after the publisher exited. A failing HTML read left one saved, one failed and one skipped descriptor. Total extraction failure created no entry.

On Windows 11 25H2 x64 build 26200.9711, `P5-04.virtual_files` restored a Unicode descriptor with two names and both streams after restarting from storage. The system OLE proxy listed `FileContents` with indices -1 and 1. The external consumer requested concrete indices 0 and 1 and read the two distinct payloads of four and seven bytes in the original order. Index 2 was rejected. An unreadable second stream and an oversized second stream each left just the independently saved text available. The virtual-only failure admitted nothing.

Two Release x64 runs checked actual process exit for the mixed and virtual-only groups. The mixed case left the original Unicode text available. Neither case advertised any virtual descriptors after exit. A newer external copy stayed current. Run all four cases with `ctest --test-dir build/p5-capture -C Release -R "^P5-(01|04)" -V --repeat until-fail:2`.

See [the Shell clipboard format definitions](https://learn.microsoft.com/en-us/windows/win32/shell/clipboard) for the Windows data-transfer types and the [Blob contract](content.md) for storage integrity and reclamation.
