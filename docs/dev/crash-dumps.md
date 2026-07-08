# Windows crash dumps

On Windows, utsure installs a crash handler early in app startup. If the process hard-crashes, it attempts to write:

- `<folder containing utsure.exe>\crash-dumps\utsure-crash-YYYYMMDD-HHMMSS-mmm-pid-<pid>-tid-<tid>-seq-<nnnn>.handler-entered.txt`
- `<folder containing utsure.exe>\crash-dumps\utsure-crash-YYYYMMDD-HHMMSS-mmm-pid-<pid>-tid-<tid>-seq-<nnnn>.dmp`
- `<folder containing utsure.exe>\crash-dumps\utsure-crash-YYYYMMDD-HHMMSS-mmm-pid-<pid>-tid-<tid>-seq-<nnnn>.json`
- `<folder containing utsure.exe>\crash-dumps\utsure-crash-YYYYMMDD-HHMMSS-mmm-pid-<pid>-tid-<tid>-seq-<nnnn>.dump-failed.txt` when minidump writing fails

The `.dmp` is a Windows minidump. The `.json` sidecar contains the last-known encode context, including stage, paths, codecs, frame position, thread counts, queue context, memory counters when available, build metadata, and active C++ exception type/message when the dump is written from `std::terminate`.
For SEH crashes, the sidecar also maps `exception_address` to `module+RVA`, records the faulting module name/path/base/size/checksum/timestamp, decodes access violations as read/write/execute plus fault address, records crashing-thread registers when a `CONTEXT` is available, and records up to 16 stack qwords that map to loaded modules as `module+RVA`.
The crash handler writes the handler-entered marker before attempting `MiniDumpWriteDump`, then writes the `.json` sidecar whether minidump writing succeeds or fails. The early marker uses `dump_write_success=pending` because the dump has not been attempted yet; use the final JSON or `.dump-failed.txt` for the actual result. Crash artifacts are created with no-overwrite path selection; an existing dump, marker, or sidecar is never silently replaced.

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
- A backup first-chance vectored exception handler through `AddVectoredExceptionHandler`, limited to hard-fault SEH codes such as access violations and stack overflows
- `std::terminate`
- `SIGABRT`, `SIGILL`, and `SIGFPE`

Normal first-chance software exceptions, including C++ exception code `0xE06D7363`, are intentionally ignored by the vectored handler because they may be caught by Qt, the C++ runtime, or app code. If a C++ exception is actually uncaught, the terminate handler writes a sidecar with `cxx_exception_active`, `cxx_exception_type`, and `cxx_exception_message`.

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

For subtitle-rendering crashes, diagnostic builds should keep debug information in both `utsure.exe` and `libass-9.dll`:

```bash
export UTSURE_CMAKE_BUILD_TYPE=Debug
export UTSURE_LIBASSMOD_BUILDTYPE=debug
export UTSURE_DIAGNOSTIC_SYMBOLS=ON
export UTSURE_STRIP_PORTABLE_DEBUG=OFF
./scripts/ci/windows-msys2-build-libassmod.sh
./scripts/ci/windows-msys2-build.sh
./scripts/ci/windows-msys2-package-symbols.sh
./scripts/ci/windows-msys2-package-portable.sh
```

Resolve reported RVAs with the unstripped binaries from the symbols artifact or build tree:

```bash
llvm-addr2line -f -C -e libass-9.dll 0x44645
llvm-addr2line -f -C -e libass-9.dll 0x5A415 0x3F900 0x52182 0x557DF
llvm-addr2line -f -C -e utsure.exe 0x27B336 0x1DBB2F
```

The MinGW equivalent is `addr2line -f -C -e <binary> <rva>` if LLVM tools are unavailable.

The normal CI libassmod build applies `patches/libassmod/*.patch` after checking out the requested `UTSURE_LIBASSMOD_REF`. The mangetsu cache-corruption diagnostic patch aborts before cache ref/unref/key helpers dereference a corrupt `value - CACHE_ITEM_SIZE` header, and prints the cache operation, caller, expected cache descriptor, value pointer, first readable value bytes, and current ASS event text/style/actor/PTS when available.

Mangetsu/libassmod cache isolation build switches:

- `MANGETSU_DISABLE_MULTI_BORDER_CACHE=1`: keep rendering multi-border output, but bypass the composite cache for multi-border composite values.
- `MANGETSU_DISABLE_CUSTOM_BORDER_LAYERS=1`: collapse mangetsu border rendering to the upstream-style single border layer for comparison.
- `MANGETSU_DISABLE_DRAWING_CACHE_STRINGVIEWS=1`: deep-copy drawing text before outline-cache lookup so hash/compare/key insertion do not read directly from the event `ASS_StringView`.

Example diagnostic dependency build:

```bash
export UTSURE_LIBASSMOD_BUILDTYPE=debug
export MANGETSU_DISABLE_MULTI_BORDER_CACHE=1
./scripts/ci/windows-msys2-build-libassmod.sh
```

Subtitle/libass diagnostic switches:

- `UTSURE_SUBTITLE_STRICT_SAME_THREAD=1`: assert/log that each libass subtitle session is created, used, and destroyed on the same subtitle worker thread. The FFmpeg streaming path uses this owner-thread model by default.
- `UTSURE_LIBASS_GLOBAL_LOCK=1`: protect all libass/libassmod init, config, render, image registration, free, and teardown calls with one global mutex.
- `UTSURE_SUBTITLE_EVENT_LOG_REPORTED_FRAME=1`: log active ASS events at frame `28109` or renderer timestamp `1172380 ms`.
- `UTSURE_SUBTITLE_EVENT_LOG_FRAME=28109` or `UTSURE_SUBTITLE_EVENT_LOG_PTS_MS=1172380`: log active ASS events for an explicit frame or timestamp.
- `UTSURE_SUBTITLE_STOP_AFTER_FRAME_RANGE=28100-28120`: stop a subtitle encode after rendering the range, useful for shorter reproductions.

To compare libassmod with clean upstream libass when the ABI remains compatible, build the dependency prefix from a different source/ref and then rebuild Utsure against that prefix:

```bash
export UTSURE_LIBASSMOD_SOURCE_URL=https://github.com/libass/libass.git
export UTSURE_LIBASSMOD_REF=master
export UTSURE_LIBASSMOD_BUILDTYPE=debug
export UTSURE_LIBASSMOD_APPLY_PATCHES=OFF
./scripts/ci/windows-msys2-build-libassmod.sh
UTSURE_CMAKE_EXTRA_ARGS="-DUTSURE_ALLOW_UPSTREAM_LIBASS_DIAGNOSTIC=ON" ./scripts/ci/windows-msys2-build.sh
```

As a fallback when the process is too corrupted for utsure's in-process writer, enable Windows Error Reporting LocalDumps:

```powershell
New-Item -Path "HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\utsure.exe" -Force
New-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\utsure.exe" -Name DumpFolder -Value "C:\temp\utsure-wer-dumps" -PropertyType ExpandString -Force
New-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\utsure.exe" -Name DumpType -Value 2 -PropertyType DWord -Force
```

WER LocalDumps are not a replacement for utsure's sidecar context, but they can prove the process crashed when no utsure handler marker appears.
