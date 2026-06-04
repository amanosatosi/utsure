# Windows crash dumps

On Windows, utsure installs a crash handler early in app startup. If the process hard-crashes, it attempts to write:

- `<folder containing utsure.exe>\crash-dumps\utsure-crash-YYYYMMDD-HHMMSS-pid-<pid>.dmp`
- `<folder containing utsure.exe>\crash-dumps\utsure-crash-YYYYMMDD-HHMMSS-pid-<pid>.json`

The `.dmp` is a Windows minidump. The `.json` sidecar contains the last-known encode context, including stage, paths, codecs, frame position, thread counts, queue context, memory counters when available, and build metadata.
The crash handler attempts to write the `.dmp` before writing this sidecar, so a sidecar failure should not prevent dump creation.

Startup resolves and creates the crash dump directory before crash handlers are needed. Directory priority is:

1. `UTSURE_CRASH_DUMP_DIR`
2. `crash-dumps` beside `utsure.exe`, which is the portable-build default
3. `%LOCALAPPDATA%\Utsure\crash-dumps`

By default, dumps use a compact type intended for upload:

- `MiniDumpNormal`
- `MiniDumpWithThreadInfo`
- `MiniDumpWithUnloadedModules`
- `MiniDumpWithProcessThreadData`

For deeper debugging, set:

```powershell
$env:UTSURE_FULL_CRASH_DUMP = "1"
```

Full dumps also include full process memory, handle data, and indirectly referenced memory. Treat full dumps as sensitive: they can contain file paths, media metadata, and process memory contents.

To write dumps somewhere else for a local repro or test run, set:

```powershell
$env:UTSURE_CRASH_DUMP_DIR = "C:\temp\utsure-crash-dumps"
```

If unset, portable builds use `crash-dumps` beside `utsure.exe`. If that cannot be created or written, utsure falls back to `%LOCALAPPDATA%\Utsure\crash-dumps`.

For parallel encode crashes, the sidecar contains the crashing thread id when available, the last-updated runner slot, the active job count, and a `runner_contexts` array with per-runner-slot state so one worker's progress does not erase the others.

When reporting a crash, include both the `.dmp` and `.json` sidecar. On the next startup, utsure logs recent crash dump paths and asks the user to include them in crash reports.

CI uploads a separate `utsure-windows-symbols-<commit>` artifact. Match the dump to the artifact from the same commit SHA. Use the matching `utsure.exe`, utsure-built DLLs/libs, and `build-metadata.txt` from that artifact when resolving stack traces.
