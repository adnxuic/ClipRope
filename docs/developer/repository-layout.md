# Repository layout

Directory names below are relative to the repository root. They follow section 9 of the [implementation plan](../../ClipRope_Long_Term_Implementation_Plan.md). The source and test directories reserve ownership boundaries for future CMake targets.

```text
ClipRope/
|-- AGENTS.md                         AI contributor entry point
|-- .agent/                           Task, verification and handoff procedures
|-- Tasks/                            P0–P6 task plans, Git workflow and completed-task ledger
|-- ClipRope_Long_Term_Implementation_Plan.md
|-- temp.md                           Original product notes
|-- rope_hanger_card.png              Supplied visual reference
|-- src/
|   |-- domain/                       Entities, values, ordering and capacity rules
|   |-- application/                  Use cases, ports, command scheduling and results
|   |-- localization/                 Pure language manager and catalog contracts
|   |-- persistence/                  SQLite, schema, blob store, leases and recovery
|   |-- platform_win/                 Windows clipboard, shell, window and DPI adapters
|   |-- rendering/                    Shared graphics device, text, images and caches
|   |-- presentation/                 Workspace, Floating views, gestures and view models
|   |-- host/                         Process entry, wiring, startup and shutdown
|   `-- capture_worker/               External data-object capture and serialization
|-- resources/                        Shipped skin, icon and other application assets
|   `-- i18n/                         Maintained English and Simplified Chinese JSON
|-- tools/                            Build-time catalog validation and code generation
|-- tests/
|   |-- unit/                         Pure rules and command orchestration with fakes
|   |-- integration/                  Real component boundaries, storage and restart
|   |-- clipboard_sources/            Controlled Windows/OLE test producers
|   |-- compatibility/                Reproducible Windows and Office cases and fixtures
|   |-- performance/                  Benchmarks, workloads and measurement harnesses
|   `-- fault_injection/              Crash, blocked source, I/O and transaction failures
`-- docs/
    |-- developer/                    English implementation handbook
    |-- decisions/                    Dated architectural decision records (ADRs)
    |-- schemas/                      Reviewed storage and interchange specifications
    `-- compatibility-matrix/         Versioned observations from compatibility runs
```

## Placing a change

- Keep headers and implementation together under their owning source module. The public boundary of an internal module should expose only what its consumers need. See [Architecture](architecture.md) for the allowed dependencies.
- Keep the current schema with its storage implementation in `src/persistence/`. Put the corresponding schema description in `docs/schemas/` and a version rejection or rollback case in `tests/integration/` or `tests/fault_injection/`.
- To add a clipboard representation, put the saved descriptor in the data model, external OLE extraction in `src/capture_worker/`, and Windows publication in `src/platform_win/`. Put a synthetic producer in `tests/clipboard_sources/` and verify the round trip in `tests/integration/`. A measured Office result belongs in `docs/compatibility-matrix/`.
- Put interaction routing in `src/presentation/`, pixel and text rendering in `src/rendering/`, and HWND creation, hit regions and DPI notifications in `src/platform_win/`. Keep durable card styles on the entry. A skin resource or a screenshot does not define clipboard payload bytes.
- Keep the source tree separate from CMake output and the per-user history store. The latter contains `history.db`, external blobs, thumbnails, staging files and logs. See [Build](build.md) and [Data model](data-model.md) for their respective locations.

The empty directories contain zero-byte `.gitkeep` files so they can travel with the scaffold in version control. Remove a marker when its directory gains a real tracked file.
