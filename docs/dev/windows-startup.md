# Windows portable startup profiling

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
