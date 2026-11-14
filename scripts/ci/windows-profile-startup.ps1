param(
    [Parameter(Mandatory)][string]$PortableDirectory,
    [string]$ReportDirectory = 'artifacts/startup-profile',
    [ValidateRange(1, 100)][int]$Launches = 11,
    [string]$Commit = $env:GITHUB_SHA,
    [switch]$VerifyDiagnostics
)

$ErrorActionPreference = 'Stop'
$portable = (Resolve-Path -LiteralPath $PortableDirectory).Path
$report = (New-Item -ItemType Directory -Force -Path $ReportDirectory).FullName
$variables = @{
    PATH = "$portable;$env:SystemRoot\System32;$env:SystemRoot"
    QT_QPA_PLATFORM = 'windows'
    QT_PLUGIN_PATH = ''
    QML2_IMPORT_PATH = ''
    UTSURE_CRASH_DUMP_DIR = (Join-Path $report 'crash-dumps')
    QT_FORCE_STDERR_LOGGING = '1'
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
    [ordered]@{
        OS = $os.Caption
        OSVersion = $os.Version
        CPU = $cpu.Name
        LogicalProcessors = $machine.NumberOfLogicalProcessors
        MemoryBytes = $machine.TotalPhysicalMemory
        ExecutableSha256 = (Get-FileHash -LiteralPath $executable).Hash
        Commit = $Commit
        QtCoreSha256 = (Get-FileHash -LiteralPath (Join-Path $portable 'Qt6Core.dll')).Hash
        QtCoreVersion = (Get-Item -LiteralPath (Join-Path $portable 'Qt6Core.dll')).VersionInfo.FileVersion
        Build = $env:UTSURE_CMAKE_BUILD_TYPE
        PlatformPlugin = 'windows'
        Method = 'Sequential launches of the same directory. The first sample has uncontrolled cache state.'
    } | ConvertTo-Json | Set-Content (Join-Path $report 'environment.json')

    $samples = foreach ($index in 1..$Launches) {
        $log = Join-Path $report "launch-$index.stderr.txt"
        $process = Start-Process -FilePath $executable -ArgumentList '--startup-profile' `
            -WorkingDirectory $portable -PassThru -RedirectStandardError $log `
            -RedirectStandardOutput (Join-Path $report "launch-$index.stdout.txt")

        try {
            $ready = '^\[startup\]\s+([\d.]+)\s+[\d.]+\s+Event loop turn after first paint$'
            $deadline = [Diagnostics.Stopwatch]::StartNew()
            do {
                if ($process.HasExited) {
                    throw "Portable application exited before it was ready (code $($process.ExitCode))."
                }
                Start-Sleep -Milliseconds 25
                $lines = @(Get-Content -LiteralPath $log)
                $readyMatch = $lines | Select-String -Pattern $ready | Select-Object -First 1
            } until ($readyMatch -or $deadline.Elapsed.TotalSeconds -ge 30)
            if (!$readyMatch) {
                throw "Timed out waiting for first paint and event-loop readiness. See $log"
            }
            $creationMatch = $lines | Select-String -SimpleMatch '[startup] Process creation to main (ms): ' |
                Select-Object -First 1
            if (!$creationMatch) {
                throw "Missing process creation timing. See $log"
            }
            $culture = [Globalization.CultureInfo]::InvariantCulture
            $preMain = [double]::Parse(($creationMatch.Line -split ': ')[-1], $culture)
            $fromMain = [double]::Parse($readyMatch.Matches[0].Groups[1].Value, $culture)

            $idleCpu = $null
            if ($index -eq $Launches) {
                $process.Refresh()
                $process.Modules | ForEach-Object ModuleName | Sort-Object |
                    Set-Content (Join-Path $report 'loaded-modules-at-idle.txt')
                Start-Sleep -Milliseconds 500
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
                CacheState = $(if ($index -eq 1) { 'uncontrolled' } else { 'warm' })
                PreMainMs = $preMain
                MainToReadyMs = $fromMain
                ProcessToReadyMs = $preMain + $fromMain
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
