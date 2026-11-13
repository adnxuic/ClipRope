# ADR 0002 · Isolate foreign clipboard reads

**Accepted · 2026-11-01.** Install `ClipRopeWorker.exe` beside the desktop and headless executables. Each eligible clipboard capture runs on an OLE STA in a fresh worker process. The worker uses the Windows GUI subsystem, so starting it requires no console host. The desktop holds no idle worker. The historical format and Office inventory tools may use a synchronous STA against their controlled fixtures. [P6-01](../../Tasks/P6-release-readiness.md)

## Failure boundary

A source can block indefinitely while enumerating formats, checking a marker, rendering a delayed format or reading a stream. The P0 configuration placed these calls on an in-process STA. That kept the UI responsive but required the thread to return during shutdown. A timeout on a future could drop the result while leaving the thread and its COM state alive.

The listener freezes the version, serial, source, target rope and three-second deadline before dispatch. At most two workers and the latest waiting ticket are retained. A newer event replaces the pending ticket, including when the newer event is excluded. Each worker starts on the same window station and desktop as its parent. Windows attaches it to a one-process, kill-on-close job during creation, with a 256 MiB process commit limit. Exit, cancellation or expiry closes the job without waiting on foreign COM.

The parent passes one inherited anonymous section handle and the ticket serial and version. The section reserves address space for 256 descriptors and 64 MiB of combined bytes. The worker commits only the pages its response requires. It runs the local-history marker policy and existing format adapters, serializes metadata and bytes, then exits. The supervisor requires a successful exit, committed regions, the expected protocol version and ticket, bounded counts and lengths, supported saved media and codec, consistent indices, and an exact payload length. It rechecks the clipboard version and live ticket before using the bytes. Only the host can submit admission to the history store. No staging files or Blob leases cross the process boundary.

Format identifiers cross the section within the same Windows session and station. The host translates registered identifiers to stable names in the durable manifest. The SQLite schema and snapshot codec remain at their existing versions. The worker has the same account privileges as the host. The job confines the lifetime and resource use of capture.

## Release x64 qualification

Windows 11 build 26200.9711, MSVC 19.40, Windows SDK 10.0.22621.0, no debugger. A synthetic external OLE source held a `GetData` call but kept dispatching COM and window messages. Independent processes posted real clipboard updates to the production desktop listener on a noninteractive window station.

Two workers entered different blocked sources. A third copy waited and a fourth replaced it. The fixture killed the exact first worker. The fourth copy entered history once and the third source received zero payload reads. The second worker expired. The fixture killed a separate current worker and let another current ticket expire against an unchanged clipboard version. The final build measured 2,969 ms between the source reaching its read barrier and worker exit. The ticket deadline starts when the listener receives the new version. Releasing obsolete sources produced zero extra admissions. A final eligible copy entered once. On reopen the store contained exactly the two expected Unicode payloads in order. The staging directory was empty and zero worker processes remained.

The comparison uses the shipped P5 host as the P0-style baseline, keeping the same resident desktop and storage workload on both sides. Each process opened an empty profile, reached listener readiness and received one blocked source. Private Bytes sums the host and every descendant. The fixture additionally checks that the desktop has only the expected worker children and each worker has zero children. The separate synthetic producer and test controller are excluded. These are paired point samples on the noninteractive station. The desktop performance suite measures distributions under the interactive workload.

| Configuration | Idle Private Bytes | Blocked Private Bytes | WM_NULL round trip | Exit after WM_CLOSE |
| --- | ---: | ---: | ---: | ---: |
| P5 in-process capture STAs | 2,174,976 | 2,416,640 | 122 µs | 763,346 µs. Still running after 750 ms, then the fixture released the source. |
| P6 capture worker | 2,342,912 | 3,903,488 | 69 µs | 6,135 µs with the source still blocked. |

An independently restarted P6 trial measured 2,363,392 idle bytes, 4,042,752 blocked bytes, a 54 µs ping and 5,086 µs to exit. SHA-256 of the comparison executables was `e53262921875ef3671fa0a19c96a361b4b11c2e6788971238150939c93210462` for the P5 desktop, `5ba7fd076af7df93a09de6b807f60965bd443194b6d4abe7f7f606162414f2f8` for the P6 desktop and `8710b4be6c9bc331fc65240fed0d46c890532bcd352ee7890630ae3bb06ceb28` for the worker.

`P6-01.worker_protocol` rejects an uncommitted response, wrong ticket, wrong clipboard version, unknown codec and excess descriptors. The existing `P1-05.capture_admission`, `P1-06.headless_restart` and `P2-01.hidden_capture_restart` passed on the external worker. [Protocol test](../../tests/fault_injection/worker_protocol.cpp) · [Desktop fault fixture](../../tests/fault_injection/worker.cpp)

### Stable reader contention

The production candidate with desktop SHA-256 `8a28fd9a795f09edd664e60f818aca77b5ca203ba62b70cf298cb8278d0e7e3c` and worker `528d49f67542a23e3ee59f357e17b278a6b4735a5f70b29b3d40c4cd5d4f5de0` lost one eligible raw Unicode copy in a deterministic private-station run. An independent reader held the clipboard for 266 ms. The sequence and source HWND stayed fixed. The old worker exhausted eight short retries, and the store still had zero entries at 3,203 ms.

The release host now passes the ticket's absolute deadline to the worker. When the owner and sequence remain current, a known clipboard locker or explicit transient OLE contention can use the remaining time. Each attempt rechecks the local history markers. After the original short retry window, attempts are spaced 32 ms apart, with 100 ms reserved for response transfer and admission. Malformed formats and unsupported media are terminal. The historic format probe retains its eight-retry window.

On the corrected Release x64 build, three independent runs held the native clipboard for 265, 250 and 265 ms. Each stored exactly one entry at 296, 297 and 312 ms respectively, and reopening its profile returned the original Unicode bytes. The hang/kill, offline restart and historical external-capture regressions passed on the same build. This fixture isolates a confirmed cause of a lost stable copy. It cannot attribute an earlier soak timeout that lacked per-ticket diagnostics. [Stable reader regression](../../tests/fault_injection/worker.cpp)

### Failed OLE enumeration of a native text copy

An accelerated run reproduced a separate lost copy on the reference desktop. The listener bound version 12596. The fixture verified its owner, version and Unicode bytes under the clipboard lock. The worker reported `EnumFormatEtc` failure `E_OUTOFMEMORY` (0x8007000E) with zero descriptors, zero saved formats and exit code zero. The bound rope was still active and no matching Blob had entered history. On a private station the same native source HWND admitted 500 consecutive versions, followed by 500 more while a second window repeatedly held the clipboard open. [Native soak failure receipt](../../tests/performance/results/ole-enumerator-failure/receipt.txt)

Local-history capture can recover this specific enumeration failure when OLE privacy queries conclusively establish eligibility. Absent markers require `DV_E_FORMATETC` or `DV_E_CLIPFORMAT`; a present local-history permission must carry a valid value of one. The worker locks the native clipboard, checks the original sequence and owner, then enumerates its complete native inventory. Only a leading `CF_UNICODETEXT` and its standard text, OEM text and locale companions qualify. Registered formats, OLE envelopes and unknown inventories leave the enumeration failure intact. The worker validates the Unicode HGLOBAL with the ordinary size and UTF-16 checks, verifies identity again and reports **partial** with the original HRESULT. The host still checks the admission deadline and bound rope. The worker's memory limit remains 256 MiB.

The bounded in-memory capture receipt records the last listener version and disposition, completed version, status, HRESULT, retries, Unicode row outcome, descriptor counts and worker exit code. For an OOM response it also reads the worker job's peak committed KiB. The failure trace above predates that last counter. A debugging tool can query the numeric receipt after a missed copy without starting a disk transcript or materializing its body.

`P6-01.native_unicode_recovery` replays the failing HRESULT while forwarding privacy queries to Windows' real clipboard proxy. The proxy returned `DV_E_CLIPFORMAT` for both absent markers. The exact Unicode bytes survived as partial. Both marker queries were independently exercised with `E_OUTOFMEMORY` and `E_ACCESSDENIED`. Those cases saved zero bytes, as did explicit exclusions, mixed or OLE content, malformed UTF-16, a replaced clipboard and an unrelated enumeration error. [Deterministic replay](../../tests/fault_injection/native_unicode.cpp)

On 2026-11-01 the frozen Release x64 build passed the replay, stable reader contention, worker isolation and offline round-trip tests, four of four in 8.16 s. The independent reader held the native clipboard for 266 ms and one correct entry committed in 297 ms. The blocked worker expired 2,984 ms after reaching the producer barrier.

### Reacquire an offer after an empty OOM result

The first release with native recovery completed 900 accelerated soak rounds. One genuine enumeration OOM on version 15107 produced the expected partial Unicode entry. A subsequent qualification run lost version 17563 at round 29. The listener and worker both reported the fixture's unchanged version, and the worker exited normally with `E_OUTOFMEMORY`, zero formats and zero retries. That earlier receipt did not identify which native or privacy guard declined recovery. [First recovery](../../tests/performance/results/release-stress-first-recovery/run.txt) · [Remaining loss](../../tests/performance/results/release-artifact-fallback-guard/run.txt)

When enumeration still fails with this exact HRESULT, zero inventory and zero saved bytes, a live local-history worker can release its old COM interfaces and obtain a fresh clipboard object. It allows at most two further acquisition calls, with 25 ms between OOM attempts, while the original owner and clipboard version still match. Every attempt uses the original ticket deadline. The worker rechecks both privacy markers through the newly acquired object. An inconclusive marker result on a recovery acquisition stops it before enumeration or payload access. The host admits the resulting snapshot through the original one-shot ticket.

The version-two worker protocol carries the OOM guard, marker query results, first native format class, observed count, native Win32 error, number of fresh acquisitions and per-process USER, GDI and handle counts. On any such result the host queries the job's peak private commit. The job uses the same user account and desktop as the listener and caps process-private commit at 256 MiB. The prior failed receipt proves an OLE method returned `E_OUTOFMEMORY`. It contains no measurement showing that the job reached its limit or that desktop heap resources were exhausted.

During the subsequent production stress run, two natural OOM cases on versions 19684 and 21379 established the failing guard directly. Both marker queries per case returned `DV_E_CLIPFORMAT`. Native `OpenClipboard` returned Win32 error 5 before enumeration, so the worker read no fallback body. One fresh OLE acquisition per version succeeded with a full capture, and the admitted entry on each bound rope contained the original bytes. Windows measured job peaks of 1,552 and 1,548 KiB. At each failed guard the worker had three USER objects, zero GDI objects and 102 handles. Both job peaks were below one percent of the enforced private-commit limit.

The final Release qualification tree passed `P6-01.native_unicode_recovery` in 1.15 s. The real Windows proxy returned `0x8004006A` for both missing markers. A reader in another process held a non-null HWND open through the failing enumeration. The native lock returned error 5, the old object was released, and a second object restored the text. With an anonymous reader the native lock succeeded on this Windows build, so one object sufficed. The fresh-object case admitted once at the bound head and rejected a duplicate completion. A permanently failing offer stopped after three acquisitions in total. Uncertain privacy, explicit exclusion, expiry and replacement admitted zero entries. An ordinary native Unicode worker peaked at 1,516 KiB of its 262,144 KiB job limit. The combined production stress and qualification measurements verified all 1,173 synthetic byte receipts and observed four natural OOM recoveries. [Final performance qualification](../../tests/performance/qualification.md)

```powershell
ctest --test-dir build/p6-capture -C Release -R 'P6-01|P1-05|P1-06|P2-01' -V
$candidate = (Resolve-Path build/p6-capture/Release).Path
$baseline = (Resolve-Path build/p5-final/Release/ClipRope.exe).Path
& "$candidate/cliprope_station_test.exe" "$candidate/cliprope_worker_fault_test.exe" `
  "$candidate/ClipRope.exe" "$candidate/cliprope_ole_source.exe" $baseline
```
