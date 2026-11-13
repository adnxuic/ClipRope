# ADR 0026 · Preview and drag worker ownership

Accepted for the architecture plan, AR2-02, 03 and 11–14. The existing desktop, capture worker and preview worker ship as a matched set. Schema v11, the thumbnail filename grammar, saved clipboard formats and the sign-in preference retain their existing meanings.

## Decision

* Screenshot hover owns one byte-only AppContainer request. The child inherits bounded input and output mappings and has no profile or arbitrary source-path argument.
* An ordinary thumbnail child runs with the current user token in a kill-on-close Job. It inherits only the request, response and metadata notification handles. The request identifies an absolute profile, a Blob hash, adapter, width, generation and deadline. Its restricted SQLite connection checks the application ID and live representation. File operations resolve paths from the saved descriptor. Explicit reads bind to that file-list identity. Automatic requests respect the existing network/reparse read policy. A transaction checks reachability again before installing a derived file.
* A DIB-only outward drag uses the same supervised executable in a separate single-job lane. Its distinct operation accepts only packed pixels, serial and byte length. The encoder branch has no profile argument or database lookup. The parent validates output size, PNG header and source dimensions. This user-token Job provides process termination and resource bounds. The screenshot-hover mode retains its separate AppContainer policy.
* Each ordinary or drag operation has a three-second deadline, one-child Job and 256 MiB process commit limit. A canceled child must exit before reuse of that lane. The termination path retains a pending-exit handle if immediate confirmation fails. A pool callback for drag encoding owns bytes and a guarded notification address. The desktop retires that address on cancellation before destroying its listener. The STA retains the source lease, verifies the gesture and performs the OLE operation.
* The cache returns bounded dirty Blob identities to all visible consumers of the affected source. The host retains an explicit full-refresh path. Hover entries expire after their existing idle interval and the background service then sleeps until signaled.

## Evidence and trade-offs

In the original in-process ordinary thumbnail path, blocking file inspection, codec initialization or derivative installation held desktop exit beyond two seconds. Moving only codec work would still leave the file-open wait in the desktop. The supervised candidate passed the held-operation exit gate. A new process adds first-read latency and transient memory. See the paired cold file trials, aggregate process counts and fixed budgets. [Phase B](../../Tasks/architecture-evidence/AR2-B.md)

A 1080p synthetic drag spent more time encoding its PNG than loading its saved DIB. Staging just that conversion lets the STA receive release, cancellation and exit while preparing the offer. Existing PNG/file offers retain their direct path. Real referenced-file matching and final Shell calls can still wait on a slow provider. Startup and repeat timing, the Shell receiver and the separate physical-input qualifications are recorded with the measurements. [Phase D](../../Tasks/architecture-evidence/AR2-D.md)

The current renderer retains its shared backing allocation across shape changes and creates Floating HWNDs for saved cards at startup. The measured populations and alternating shapes stayed within the fixed resource budgets. A different population, provider or device can justify repeating those tests. The six extension checks and final environment coverage are in the [qualification handoff](../../Tasks/architecture-evidence/AR2-E.md).
