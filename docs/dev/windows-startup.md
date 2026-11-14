# Windows portable startup profiling

## Results

[Windows CI run 43616220926](https://github.com/amanosatosi/utsure/actions/runs/43616220926) compared portable commits `a24d069` and `b873b6f` on the same Azure runner. The host ran Windows Server 2025 Datacenter 10.0.26100 with four AMD EPYC 9V74 logical CPUs and 16 GiB RAM. Both packages used MSYS2 UCRT64, x64 RelWithDebInfo, Qt 6.12.0, and the native Windows platform plugin. The Qt6Core file hashes differed despite the matching version. The packages ran in sequence with ten warm samples each.

| Metric | Baseline | Optimized |
| --- | ---: | ---: |
| Warm process creation to event-loop readiness, median | 204.8 ms | 183.3 ms |
| Warm range | 186.5–218.2 ms | 168.0–200.5 ms |
| Warm process creation to first Qt window paint event, median | 186.9 ms | 165.3 ms |
| Warm process creation to `main()`, median | 30.1 ms | 37.1 ms |
| Idle process CPU time over one second, one sample | 0 ms | 0 ms |
| First launch with uncontrolled cache state | 1,268.0 ms | 153.9 ms |
| Reboot-controlled cold launch | Unmeasured | Unmeasured |

The warm readiness median fell 21.5 ms (10.5%). An earlier [baseline run](https://github.com/amanosatosi/utsure/actions/runs/43612049502) recorded 248.5 ms on different hardware. The first launches in the final comparison ran in a fixed order after different extraction and smoke-test histories. The cold target remains unverified.

| Warm stage median | Baseline | Optimized |
| --- | ---: | ---: |
| Crash handler installation | 0.91 ms | 0.98 ms |
| `QApplication` construction | 16.65 ms | 18.75 ms |
| Resource registration and metadata | 0.01 ms | 0.01 ms |
| Application icon initialization | 1.64 ms | 1.57 ms |
| Settings load in `main()` | 0.40 ms | 0.40 ms |
| Settings load in `MainWindow` | 0.28 ms | Removed |
| Font resolution and application | 11.06 ms | 12.54 ms |
| Bundled Pyidaungsu registration, included in font resolution | 1.48 ms | 1.74 ms |
| Window shell, toolbar, queue, and output widgets | 10.92 ms | 12.46 ms |
| Main tab | 11.22 ms | 11.76 ms |
| Encode tab | 11.12 ms | Deferred until first job selection |
| Special and Logs tabs | 3.64 ms | 0.77 ms for placeholders and Logs |
| Preview and Task Log layout | 6.55 ms with drop overlay | 5.94 ms |
| Stylesheet assignment | 0.74 ms | 1.01 ms |
| Preview audio and frame controller initialization | 0.07 ms and 0.04 ms | 0.01 ms and less than 0.01 ms |
| First encode runner slot | 0.09 ms | Deferred until encoding |
| Notification setup | 23.16 ms | Deferred until a terminal job |
| Initial view refresh | 4.10 ms | 2.45 ms |
| `MainWindow::show()` | 37.38 ms | 47.98 ms |
| Event-loop turn after first paint | 17.25 ms | 17.18 ms |

Notification audio accounted for 22.72 ms of baseline notification setup. Creating the notification and its selected sound on demand removed that work from startup. Both `ffmpegmediaplugin.dll` and `MMDevApi.dll` disappeared from the optimized idle module snapshot. Qt6Multimedia, FFmpeg, FFMS2, and Mangetsu DLLs remained loaded. The warm pre-main measurement and idle module snapshots cannot apportion loader time per DLL, so the portable dependency closure stayed intact.

The Encode and Special editors are built on first job selection, before their controls become usable. Preview workers start on the first frame request or viable audio playback, and the encode runner uses the existing queue start path. Icon reuse, fewer explicit repolishes, one settings snapshot, and reuse of the crash directory check remove repeated work. The installed font family lookup still initializes Qt's font database, and the measured font stage was slightly slower. Early crash handlers remain installed before Qt and before the crash directory write check. The recent crash scan runs after the window is shown.

Windows CI passed 123 tests, the app and extracted portable smoke tests, and all 22 native launch/close cycles. The optimized portable executable wrote a diagnostic dump and JSON sidecar. The automated suite covers missing and invalid settings, default Pyidaungsu and Myanmar text in Qt widgets, notification routing, and encode lifecycle, parallelism, and cancellation. An audio-enabled Windows client is still needed to verify audible notifications, the first audiovisual preview, and interactive editor and first-encode latency.

Qt meets the warm target on this runner. The evidence supports keeping Qt Widgets. The largest optimized stage is now `show()`, which includes synchronous widget polish and layout. Cold client traces should establish whether that stage, the font database, or the portable loader dominates the reported desktop delay before further work on styling or delay-loading.

Build and package with the repository's Windows MSYS2 UCRT64 workflow. Use the same Qt version, build configuration, and dependency pins for both builds. Keep both extracted bundles on the same volume.

## Measurements

Run `scripts/ci/windows-profile-startup.ps1 -PortableDirectory <extracted-portable-directory>` for each build on the same host. The script launches the normal Windows platform plugin with a portable-only dependency path, and closes the actual window after the first paint and a queued event-loop callback.

* `environment.json` records the Windows version, hardware, executable hash, and build metadata.
* `launch-*.stderr.txt` records the stage timings. The cumulative main-thread column starts at entry to C++ `main()`. The stage column measures time since the prior checkpoint or the duration of a named nested stage. Nested timings already contribute to their enclosing stage.
* `samples.csv` records Windows process creation to `main()`, GUI readiness, and an idle CPU sample. Compare the median and range of launches 2–11 on the same host. Launch 1 has uncontrolled cache state.
* `loaded-modules-at-idle.txt` identifies modules still loaded at idle. Use ETW image-load events to identify when they loaded or detect modules unloaded before the snapshot.

The paint mark is a Qt paint event for the actual main window. The next queued callback establishes that the event loop serviced work after painting. Desktop composition and input latency can be measured independently in Windows Performance Analyzer.

## Cold launches and loader attribution

Use a dedicated Windows test host with a consistent power profile, storage, and Windows Defender configuration. Stage both builds and prepare identical initialized settings before measurement. Reboot before each cold sample and launch only one build after each reboot. Alternate builds over multiple reboots, use a consistent delay after sign-in, and record cache-control procedure and disk type. Report the median and range separately from the warm runs.

For attribution, capture a separate WPR General Profile trace around each launch. In WPA filter process lifetime, image loads, sampled CPU, thread scheduling, hard faults, and file I/O to the matching Utsure PID. Use process creation and the logged pre-main duration to bound loader and runtime startup. Identify the libraries and files on the critical path before proposing delay-loading. Record trace runs separately because tracing can affect launch timing.

## Functional verification

Run the existing Windows build, CTest suite, and packaged smoke check. For a desktop check, repeat the launch and close sequence, test first launch with missing settings and recovery from a corrupt settings file, and verify Burmese glyphs with default and custom fonts. Import a source with audio, open the preview, start and cancel a parallel batch, then close during an active job. Complete a successful and a failed job with a working audio output device to verify the two notification sounds. Run `utsure.exe --write-diagnostic-dump` and check the dump and sidecar.
