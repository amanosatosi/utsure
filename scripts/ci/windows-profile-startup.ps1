param(
    [Parameter(Mandatory)][string]$PortableDirectory,
    [string]$ReportDirectory = (Join-Path $PWD ('Utsure-startup-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))),
    [ValidateRange(1, 100)][int]$Launches = 11,
    [string]$Commit = $env:GITHUB_SHA,
    [string]$OpenPath,
    [int]$ExpectedJobs = 1,
    [switch]$UseExistingSettings,
    [switch]$Cold,
    [switch]$VerifyDiagnostics
)

$ErrorActionPreference = 'Stop'
$portable = (Resolve-Path -LiteralPath $PortableDirectory).Path
$report = (New-Item -ItemType Directory -Force -Path $ReportDirectory).FullName
if ($Cold) { $Launches = 1 }
$appArguments = '--startup-profile'
if ($OpenPath) {
    $resolvedSource = (Resolve-Path -LiteralPath $OpenPath).Path.TrimEnd('\')
    $appArguments += ' --startup-profile-open "' + $resolvedSource + '"'
}
$variables = @{
    PATH = "$portable;$env:SystemRoot\System32;$env:SystemRoot"
    QT_QPA_PLATFORM = 'windows'
    QT_PLUGIN_PATH = ''
    QML2_IMPORT_PATH = ''
    UTSURE_CRASH_DUMP_DIR = (Join-Path $report 'crash-dumps')
    UTSURE_GIT_COMMIT = $Commit
    QT_FORCE_STDERR_LOGGING = '1'
}
if (!$UseExistingSettings) {
    $variables['LOCALAPPDATA'] = (New-Item -ItemType Directory -Force -Path (Join-Path $report 'settings')).FullName
}
$previous = @{}
foreach ($name in $variables.Keys) {
    $previous[$name] = [Environment]::GetEnvironmentVariable($name)
    [Environment]::SetEnvironmentVariable($name, $variables[$name])
}

try {
    $os = Get-CimInstance Win32_OperatingSystem
    $machine = Get-CimInstance Win32_ComputerSystem
    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    $executable = Join-Path $portable 'utsure.exe'
    $environment = [ordered]@{
        OS = $os.Caption
        OSVersion = $os.Version
        CPU = $cpu.Name
        LogicalProcessors = $machine.NumberOfLogicalProcessors
        MemoryBytes = $machine.TotalPhysicalMemory
        Commit = $Commit
        Build = $env:UTSURE_CMAKE_BUILD_TYPE
        PlatformPlugin = 'windows'
        CacheMethod = $(if ($Cold) { 'Operator-reported first launch after reboot' } else { 'Sequential launches' })
        Settings = $(if ($UseExistingSettings) { 'Existing user profile' } else { 'Isolated profile' })
    }

    $samples = foreach ($index in 1..$Launches) {
        Write-Progress -Activity 'Utsure startup benchmark' -Status "Launch $index of $Launches" -PercentComplete (100 * ($index - 1) / $Launches)
        $log = Join-Path $report "launch-$index.stderr.txt"
        $process = Start-Process -FilePath $executable -ArgumentList $appArguments `
            -WorkingDirectory $portable -PassThru -RedirectStandardError $log `
            -RedirectStandardOutput (Join-Path $report "launch-$index.stdout.txt")

        try {
            $readyPattern = '(?m)^\[startup\][ \t]+([0-9]+\.[0-9]+)[ \t]+[0-9.]+[ \t]+Event loop turn after first paint\r?\n'
            $creationPattern = '(?m)^\[startup\] Process creation to main \(ms\): ([0-9]+\.[0-9]+)\r?\n'
            $stallPattern = '(?m)^\[startup\] Maximum UI timer lateness \(ms\): ([0-9.]+)\r?\n'
            $importPattern = '(?m)^\[startup\] Video import \(ms\): ([0-9.]+) \(([0-9]+) jobs\)\r?\n'
            $paintPattern = '(?m)^\[startup\][ \t]+([0-9.]+)[ \t]+[0-9.]+[ \t]+First window paint event\r?\n'
            $deadline = [Diagnostics.Stopwatch]::StartNew()
            do {
                if ($process.HasExited) {
                    throw "Portable application exited before it was ready (code $($process.ExitCode))."
                }
                Start-Sleep -Milliseconds 25
                $logText = [string](Get-Content -LiteralPath $log -Raw)
                $readyMatch = [regex]::Match($logText, $readyPattern)
                $creationMatch = [regex]::Match($logText, $creationPattern)
                $stallMatch = [regex]::Match($logText, $stallPattern)
                $importMatch = [regex]::Match($logText, $importPattern)
                $complete = $readyMatch.Success -and $creationMatch.Success -and $stallMatch.Success -and (!$OpenPath -or $importMatch.Success)
            } until ($complete -or $deadline.Elapsed.TotalSeconds -ge 60)
            if (!$readyMatch.Success) {
                throw "Timed out waiting for first paint and event-loop readiness. See $log"
            }
            if (!$creationMatch.Success) {
                throw "Missing process creation timing. See $log"
            }
            $culture = [Globalization.CultureInfo]::InvariantCulture
            $preMain = [double]::Parse($creationMatch.Groups[1].Value, $culture)
            $fromMain = [double]::Parse($readyMatch.Groups[1].Value, $culture)
            if (!$complete) { throw "Incomplete startup profile. See $log" }
            if ($OpenPath -and [int]$importMatch.Groups[2].Value -ne $ExpectedJobs) {
                throw "Import did not produce $ExpectedJobs jobs. See $log"
            }
            $paintMatch = [regex]::Match($logText, $paintPattern)
            $cpuMatch = [regex]::Match($logText, '(?m)^\[startup\] Ready CPU \(ms\): ([0-9.]+)\r?\n')
            $workingSet = [regex]::Match($logText, '(?m)^\[startup\] Ready working set \(bytes\): ([0-9]+)\r?\n')
            $privateMemory = [regex]::Match($logText, '(?m)^\[startup\] Ready private memory \(bytes\): ([0-9]+)\r?\n')

            $idleCpu = $null
            if ($index -eq $Launches) {
                $process.Refresh()
                $process.Modules | ForEach-Object ModuleName | Sort-Object |
                    Set-Content (Join-Path $report 'loaded-modules-at-idle.txt')
                $process.Refresh()
                $previousCpu = $process.TotalProcessorTime.TotalMilliseconds
                Start-Sleep -Seconds 1
                $process.Refresh()
                $idleCpu = $process.TotalProcessorTime.TotalMilliseconds - $previousCpu
            }

            if (!$process.CloseMainWindow() -or !$process.WaitForExit(10000) -or $process.ExitCode -ne 0) {
                throw "Portable application did not close cleanly after launch $index."
            }
            [pscustomobject]@{
                Launch = $index
                CacheState = $(if ($Cold) { 'after reboot' } elseif ($index -eq 1) { 'uncontrolled' } else { 'warm' })
                PreMainMs = $preMain
                MainToReadyMs = $fromMain
                ProcessToReadyMs = $preMain + $fromMain
                ProcessToFirstPaintMs = $preMain + [double]::Parse($paintMatch.Groups[1].Value, $culture)
                StartupCpuMs = [double]::Parse($cpuMatch.Groups[1].Value, $culture)
                ReadyWorkingSetBytes = [long]$workingSet.Groups[1].Value
                ReadyPrivateMemoryBytes = [long]$privateMemory.Groups[1].Value
                MaxUiTimerLatenessMs = [double]::Parse($stallMatch.Groups[1].Value, $culture)
                ImportMs = $(if ($OpenPath) { [double]::Parse($importMatch.Groups[1].Value, $culture) })
                IdleCpuMsOverOneSecond = $idleCpu
            }
        } finally {
            if (!$process.HasExited) {
                $process.Kill()
                $process.WaitForExit()
            }
            $process.Dispose()
        }
    }
    Write-Progress -Activity 'Utsure startup benchmark' -Completed
    $environment['ExecutableSha256'] = (Get-FileHash -LiteralPath $executable).Hash
    $environment['QtCoreSha256'] = (Get-FileHash -LiteralPath (Join-Path $portable 'Qt6Core.dll')).Hash
    $environment['QtCoreVersion'] = (Get-Item -LiteralPath (Join-Path $portable 'Qt6Core.dll')).VersionInfo.FileVersion
    $environment | ConvertTo-Json | Set-Content (Join-Path $report 'environment.json')
    $samples | Export-Csv -NoTypeInformation (Join-Path $report 'samples.csv')
    $samples | Format-Table | Out-Host

    if ($VerifyDiagnostics) {
        $diagnostic = Start-Process -FilePath $executable -ArgumentList '--write-diagnostic-dump' -WorkingDirectory $portable `
            -PassThru -RedirectStandardError (Join-Path $report 'diagnostic.stderr.txt')
        try {
            if (!$diagnostic.WaitForExit(30000) -or $diagnostic.ExitCode -ne 0) {
                throw 'Portable diagnostic dump command failed.'
            }
            $dumps = @(Get-ChildItem -LiteralPath $env:UTSURE_CRASH_DUMP_DIR -Filter '*.dmp' -File)
            if (!$dumps -or !($dumps | Where-Object { $_.Length -gt 0 -and (Test-Path -LiteralPath ($_.FullName -replace '\.dmp$', '.json')) })) {
                throw 'Portable diagnostic command did not produce a dump and JSON sidecar.'
            }
            $dumps | Select-Object Name, Length | Format-Table | Out-Host
        } finally {
            if (!$diagnostic.HasExited) {
                $diagnostic.Kill()
                $diagnostic.WaitForExit()
            }
            $diagnostic.Dispose()
        }
    }
} finally {
    foreach ($name in $previous.Keys) {
        [Environment]::SetEnvironmentVariable($name, $previous[$name])
    }
}
