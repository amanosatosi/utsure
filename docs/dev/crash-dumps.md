# Windows crash dumps

On Windows, utsure installs a crash handler early in app startup. If the process hard-crashes, it attempts to write:

- `<folder containing utsure.exe>\crash-dumps\utsure-crash-YYYYMMDD-HHMMSS-mmm-pid-<pid>-tid-<tid>-seq-<nnnn>.handler-entered.txt`
- `<folder containing utsure.exe>\crash-dumps\utsure-crash-YYYYMMDD-HHMMSS-mmm-pid-<pid>-tid-<tid>-seq-<nnnn>.dmp`
- `<folder containing utsure.exe>\crash-dumps\utsure-crash-YYYYMMDD-HHMMSS-mmm-pid-<pid>-tid-<tid>-seq-<nnnn>.json`
- `<folder containing utsure.exe>\crash-dumps\utsure-crash-YYYYMMDD-HHMMSS-mmm-pid-<pid>-tid-<tid>-seq-<nnnn>.dump-failed.txt` when minidump writing fails

The `.dmp` is a Windows minidump. The `.json` sidecar contains the last-known encode context, including stage, paths, codecs, frame position, thread counts, queue context, memory counters when available, and build metadata.
The crash handler writes the handler-entered marker before attempting `MiniDumpWriteDump`, then writes the `.json` sidecar whether minidump writing succeeds or fails. Crash artifacts are created with no-overwrite path selection; an existing dump, marker, or sidecar is never silently replaced.

Interpret missing or partial artifacts this way:

- No `.handler-entered.txt`: the utsure handler probably did not run, or the process was terminated too abruptly for in-process handling.
- `.handler-entered.txt` plus `.dump-failed.txt` plus `.json`: the handler ran, but `MiniDumpWriteDump` or dump-file creation failed. The sidecar contains `dump_write_success`, `dump_write_error_code`, and `dump_path_attempted`.
- `.handler-entered.txt` plus `.dmp` plus `.json`: the handler ran and dump writing succeeded.

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

At startup, utsure logs whether the crash dump writer is enabled, the resolved directory, directory existence/writability, installed handler state, process id, build version, and git commit when available.

Covered paths:

- Top-level unhandled SEH exceptions through `SetUnhandledExceptionFilter`
- A backup first-chance vectored exception handler through `AddVectoredExceptionHandler`
- `std::terminate`
- `SIGABRT`, `SIGILL`, and `SIGFPE`

Not covered reliably:

- Forced process termination such as Task Manager kill, power loss, or `TerminateProcess`
- Crashes after another component replaces all process exception handling and prevents both utsure handlers from running
- Heap or loader corruption severe enough to prevent in-process file creation
- CRT invalid-parameter and pure-virtual-call hooks are not separately installed yet; WER LocalDumps are the recommended fallback for those paths if utsure artifacts are absent

To test the dump path without crashing the app, run:

```powershell
.\utsure.exe --write-diagnostic-dump
```

For parallel encode crashes, the sidecar contains the crashing thread id when available, the last-updated runner slot, the active job count, and a `runner_contexts` array with per-runner-slot state so one worker's progress does not erase the others.

When reporting a crash, include both the `.dmp` and `.json` sidecar. On the next startup, utsure logs recent crash dump paths and asks the user to include them in crash reports.

CI uploads a separate `utsure-windows-symbols-<commit>` artifact. Match the dump to the artifact from the same commit SHA. Use the matching `utsure.exe`, utsure-built DLLs/libs, and `build-metadata.txt` from that artifact when resolving stack traces.

As a fallback when the process is too corrupted for utsure's in-process writer, enable Windows Error Reporting LocalDumps:

```powershell
New-Item -Path "HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\utsure.exe" -Force
New-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\utsure.exe" -Name DumpFolder -Value "C:\temp\utsure-wer-dumps" -PropertyType ExpandString -Force
New-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\utsure.exe" -Name DumpType -Value 2 -PropertyType DWord -Force
```

WER LocalDumps are not a replacement for utsure's sidecar context, but they can prove the process crashed when no utsure handler marker appears.
