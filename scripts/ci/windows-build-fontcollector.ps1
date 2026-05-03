$ErrorActionPreference = 'Stop'

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$thirdPartyRoot = if ($env:UTSURE_THIRD_PARTY_ROOT) {
    $env:UTSURE_THIRD_PARTY_ROOT
} else {
    Join-Path $projectRoot '.deps'
}
$fontCollectorVersion = if ($env:UTSURE_FONTCOLLECTOR_VERSION) {
    $env:UTSURE_FONTCOLLECTOR_VERSION
} else {
    '4.0.2'
}
$pyInstallerVersion = if ($env:UTSURE_PYINSTALLER_VERSION) {
    $env:UTSURE_PYINSTALLER_VERSION
} else {
    '6.19.0'
}

$fontCollectorRoot = if ($env:UTSURE_FONTCOLLECTOR_ROOT) {
    $env:UTSURE_FONTCOLLECTOR_ROOT
} else {
    Join-Path $thirdPartyRoot 'fontcollector'
}
$buildRoot = Join-Path $fontCollectorRoot 'build'
$distRoot = Join-Path $fontCollectorRoot 'dist'
$prefixRoot = Join-Path $fontCollectorRoot 'prefix'
$bundleToolRoot = Join-Path $prefixRoot 'tools\fontcollector'
$venvRoot = Join-Path $fontCollectorRoot 'venv'
$entryPoint = Join-Path $fontCollectorRoot 'fontcollector_entry.py'

Remove-Item -Recurse -Force $buildRoot, $distRoot, $prefixRoot, $entryPoint -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $fontCollectorRoot, $bundleToolRoot | Out-Null

python -m venv $venvRoot
$python = Join-Path $venvRoot 'Scripts\python.exe'

& $python -m pip install --upgrade pip
& $python -m pip install `
    "FontCollector==$fontCollectorVersion" `
    "pyinstaller==$pyInstallerVersion"

@'
from font_collector.__main__ import main

if __name__ == "__main__":
    main()
'@ | Set-Content -Path $entryPoint -Encoding UTF8

& $python -m PyInstaller `
    --clean `
    --noconfirm `
    --onefile `
    --name fontcollector `
    --distpath $distRoot `
    --workpath $buildRoot `
    --specpath $fontCollectorRoot `
    --collect-all font_collector `
    --collect-all fontTools `
    --collect-all freetype `
    --collect-all find_system_fonts_filename `
    --collect-all ass `
    --collect-all ass_tag_analyzer `
    --collect-all langcodes `
    $entryPoint

$builtExe = Join-Path $distRoot 'fontcollector.exe'
if (-not (Test-Path $builtExe)) {
    throw "FontCollector PyInstaller build did not produce '$builtExe'."
}

Copy-Item -Force $builtExe (Join-Path $bundleToolRoot 'fontcollector.exe')

$sitePackages = & $python -c "import site; print(site.getsitepackages()[0])"
$licenseFile = Get-ChildItem -Path $sitePackages -Recurse -File -Include 'LICENSE', 'LICENSE.txt' |
    Where-Object {
        $_.FullName -match 'fontcollector|FontCollector|font_collector'
    } |
    Select-Object -First 1
if ($licenseFile) {
    Copy-Item -Force $licenseFile.FullName (Join-Path $bundleToolRoot 'FontCollector-LICENSE.txt')
}

@"
FontCollector $fontCollectorVersion
Built for the utsure Windows portable bundle with PyInstaller $pyInstallerVersion.
Source: https://github.com/moi15moi/FontCollector
Package: https://pypi.org/project/FontCollector/
"@ | Set-Content -Path (Join-Path $bundleToolRoot 'README.txt') -Encoding UTF8

& (Join-Path $bundleToolRoot 'fontcollector.exe') --help | Out-Null

Write-Host "FontCollector portable tool staged at $bundleToolRoot"
