param(
    [ValidateSet('optimized', 'baseline', 'both')][string]$Variant = 'both',
    [ValidateRange(1, 100)][int]$Launches = 11,
    [string]$VideoOrFolder,
    [ValidateRange(1, 100000)][int]$ExpectedJobs = 1,
    [string]$SettingsFile,
    [switch]$Cold,
    [switch]$Trace
)

$ErrorActionPreference = 'Stop'
if ($Cold -and $Variant -eq 'both') {
    throw 'Choose -Variant baseline or -Variant optimized for a single post-reboot launch.'
}
$folder = Join-Path $PSScriptRoot ('Utsure-startup-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $folder | Out-Null
Write-Host "Saving benchmark results to $folder"
$variants = if ($Variant -eq 'both') { 'baseline', 'optimized' } else { $Variant }
foreach ($build in $variants) {
    $portable = Join-Path $PSScriptRoot $build
    $commit = (Get-Content -LiteralPath (Join-Path $portable 'benchmark-commit.txt') -Raw).Trim()
    $common = @{
        PortableDirectory = $portable
        Commit = $commit
        SettingsFile = $SettingsFile
        Launches = $Launches
        Cold = $Cold
        Trace = $Trace
    }
    if ($VideoOrFolder) {
        $common['OpenPath'] = $VideoOrFolder
        $common['ExpectedJobs'] = $ExpectedJobs
    }
    Write-Host "Benchmarking $build"
    & (Join-Path $PSScriptRoot 'windows-profile-startup.ps1') @common -ReportDirectory (Join-Path $folder $build)
}
