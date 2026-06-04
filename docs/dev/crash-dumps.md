# Windows crash dumps

On Windows, utsure installs a crash handler early in app startup. If the process hard-crashes, it attempts to write:

- `%LOCALAPPDATA%\Utsure\crash-dumps\utsure-crash-YYYYMMDD-HHMMSS-pid-<pid>.dmp`
- `%LOCALAPPDATA%\Utsure\crash-dumps\utsure-crash-YYYYMMDD-HHMMSS-pid-<pid>.json`

The `.dmp` is a Windows minidump. The `.json` sidecar contains the last-known encode context, including stage, paths, codecs, frame position, thread counts, queue context, memory counters when available, and build metadata.

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

When reporting a crash, include both the `.dmp` and `.json` sidecar. On the next startup, utsure logs recent crash dump paths and asks the user to include them in crash reports.

CI uploads a separate `utsure-windows-symbols-<commit>` artifact. Match the dump to the artifact from the same commit SHA. Use the matching `utsure.exe`, utsure-built DLLs/libs, and `build-metadata.txt` from that artifact when resolving stack traces.
