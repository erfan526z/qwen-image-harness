<#
.SYNOPSIS
    Build Qwen Image Harness on Windows with qmake + MinGW.

.DESCRIPTION
    Same application as build-linux.sh: it only needs a Qt 5.12+ MinGW 64-bit
    kit. The script locates qmake and the matching g++, builds a release
    binary, then runs windeployqt so the result is a standalone folder.

    Output: harness\dist\QwenImageHarness\QwenImageHarness.exe

.PARAMETER Version
    Stamp this version into the binary. Defaults to the APP_VERSION environment
    variable, then to the fallback in QImageHarness.pro.

.PARAMETER QtDir
    Qt kit prefix, e.g. C:\Qt\Qt5.12.12\5.12.12\mingw73_64. When omitted the
    script uses QMAKE, then qmake on PATH, then the newest MinGW 64-bit kit
    under C:\Qt.

.PARAMETER BuildDir
    Build directory. Defaults to harness\Build-Windows.

.PARAMETER Config
    release (default) or debug.

.PARAMETER NoDeploy
    Skip windeployqt and the dist folder: leave the bare executable in the
    build directory.

.PARAMETER NoClean
    Keep existing object files. By default the script cleans first, because
    qmake does not rebuild objects when only the APP_VERSION define changes.

.PARAMETER ZipPath
    Also package the deployed folder into this .zip, named after the version.

.EXAMPLE
    .\harness\build-windows.ps1

.EXAMPLE
    .\harness\build-windows.ps1 -Version 0.2.0 -ZipPath dist\windows.zip
#>
[CmdletBinding()]
param(
    [string]$Version = $env:APP_VERSION,
    [string]$QtDir,
    [string]$BuildDir,
    [ValidateSet('release', 'debug')][string]$Config = 'release',
    [switch]$NoDeploy,
    [switch]$NoClean,
    [string]$ZipPath
)

$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
$srcDir = Join-Path $root 'Src'
if (-not $BuildDir) { $BuildDir = Join-Path $root 'Build-Windows' }
$distDir = Join-Path $root 'dist\QwenImageHarness'

function Find-QMake {
    param([string]$Explicit)

    if ($Explicit) {
        $q = Join-Path $Explicit 'bin\qmake.exe'
        if (Test-Path $q) { return (Resolve-Path $q).Path }
        throw "QtDir '$Explicit' does not contain bin\qmake.exe"
    }
    if ($env:QMAKE) {
        if (Test-Path $env:QMAKE) { return (Resolve-Path $env:QMAKE).Path }
        throw "QMAKE is set to '$($env:QMAKE)', which does not exist"
    }
    $onPath = Get-Command qmake -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    $found = @()
    foreach ($pattern in @('C:\Qt\*\*\*\bin\qmake.exe', 'C:\Qt\*\*\bin\qmake.exe', 'C:\Qt\*\bin\qmake.exe')) {
        $found += @(Get-ChildItem $pattern -ErrorAction SilentlyContinue)
    }
    $found = @($found | Where-Object { $_.FullName -match '\\mingw[0-9_]*_64\\bin\\qmake\.exe$' })
    if ($found.Count -gt 0) {
        $ranked = $found | ForEach-Object {
            $v = [version]'0.0.0'
            if ($_.FullName -match 'Qt(\d+\.\d+\.\d+)') { $v = [version]$Matches[1] }
            [pscustomobject]@{ Path = $_.FullName; Version = $v }
        } | Sort-Object -Property @{ Expression = 'Version'; Descending = $true },
                                 @{ Expression = 'Path'; Descending = $true }
        return $ranked[0].Path
    }

    throw "qmake not found. Install a Qt MinGW 64-bit kit, pass -QtDir or set QMAKE, or put qmake on PATH."
}

$qmake = Find-QMake $QtDir
$qtBin = Split-Path $qmake -Parent
$kitDir = Split-Path $qtBin -Parent
$qtVersionDir = Split-Path $kitDir -Parent
$qtRoot = Split-Path $qtVersionDir -Parent
$toolsDir = Join-Path $qtRoot 'Tools'

# Prefer the MinGW that ships next to this Qt (Qt 5.12 -> mingw730_64,
# Qt 5.15.2 -> mingw810_64). A different GCC on PATH can produce a binary that
# does not match Qt's C++ runtime.
$mingwBin = $null
if (Test-Path $toolsDir) {
    $toolKits = @(Get-ChildItem $toolsDir -Directory -Filter 'mingw*_64' -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName 'bin\g++.exe') } |
        Sort-Object LastWriteTime -Descending)
    if ($toolKits.Count -gt 0) { $mingwBin = Join-Path $toolKits[0].FullName 'bin' }
}
if (-not $mingwBin) {
    $gxx = Get-Command g++ -ErrorAction SilentlyContinue
    if ($gxx) { $mingwBin = Split-Path $gxx.Source -Parent }
}
if (-not $mingwBin) {
    throw "No MinGW g++ found next to Qt ($toolsDir) or on PATH."
}

$make = Join-Path $mingwBin 'mingw32-make.exe'
if (-not (Test-Path $make)) {
    $onPathMake = Get-Command mingw32-make -ErrorAction SilentlyContinue
    if ($onPathMake) {
        $make = $onPathMake.Source
    } else {
        throw "mingw32-make.exe not found in $mingwBin"
    }
}

$env:Path = "$mingwBin;$qtBin;$env:Path"
if ($Version) { $env:APP_VERSION = $Version } else { Remove-Item Env:APP_VERSION -ErrorAction SilentlyContinue }

Write-Host "[INFO] qmake:        $qmake"
& $qmake -v | Select-Object -First 3
Write-Host "[INFO] compiler:     $(Join-Path $mingwBin 'g++.exe')"
Write-Host "[INFO] build dir:    $BuildDir"
Write-Host "[INFO] configuration: $Config"
if ($Version) { Write-Host "[INFO] version:      $Version" }

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
Push-Location $BuildDir
try {
    if (-not $NoClean -and (Test-Path 'Makefile')) {
        & $make clean
        if ($LASTEXITCODE -ne 0) { throw "make clean failed with exit code $LASTEXITCODE" }
    }

    & $qmake (Join-Path $srcDir 'QImageHarness.pro') "CONFIG+=$Config"
    if ($LASTEXITCODE -ne 0) { throw "qmake failed with exit code $LASTEXITCODE" }

    & $make -j $env:NUMBER_OF_PROCESSORS
    if ($LASTEXITCODE -ne 0) { throw "build failed with exit code $LASTEXITCODE" }
} finally {
    Pop-Location
}

$outDir = if ($Config -eq 'debug') { 'debug' } else { 'release' }
$exe = Join-Path $BuildDir "$outDir\QwenImageHarness.exe"
if (-not (Test-Path $exe)) { throw "Build finished but $exe is missing" }

function Get-ExeVersion {
    param([string]$ExePath)
    $tmp = [System.IO.Path]::GetTempFileName()
    try {
        $p = Start-Process -FilePath $ExePath -ArgumentList '--version' -Wait -PassThru `
            -NoNewWindow -RedirectStandardOutput $tmp
        if ($p.ExitCode -ne 0) { return $null }
        $text = (Get-Content $tmp -Raw).Trim()
        if ($text -match '^Qwen Image Harness\s+(\S+)$') { return $Matches[1] }
        return $text
    } finally {
        Remove-Item $tmp -Force -ErrorAction SilentlyContinue
    }
}

$effectiveVersion = if ($Version) { $Version } else { Get-ExeVersion $exe }
if (-not $effectiveVersion) { $effectiveVersion = '0.0.0' }

if (-not $NoDeploy) {
    if (Test-Path $distDir) { Remove-Item $distDir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $distDir | Out-Null
    Copy-Item $exe $distDir

    $deploy = Join-Path $qtBin 'windeployqt.exe'
    if (-not (Test-Path $deploy)) { throw "windeployqt.exe not found in $qtBin" }
    & $deploy '--release' '--no-translations' (Join-Path $distDir 'QwenImageHarness.exe')
    if ($LASTEXITCODE -ne 0) { throw "windeployqt failed with exit code $LASTEXITCODE" }

    # windeployqt ships qwindows only. The headless plugins let CI (and servers
    # without an interactive desktop) smoke-test the binary with QT_QPA_PLATFORM.
    $platforms = Join-Path $qtBin '..\plugins\platforms'
    foreach ($plugin in @('qoffscreen.dll', 'qminimal.dll')) {
        $src = Join-Path $platforms $plugin
        if (Test-Path $src) {
            Copy-Item $src (Join-Path $distDir 'platforms')
        }
    }

    $repoRoot = Split-Path $root -Parent
    foreach ($doc in @('LICENSE', 'NOTICE', 'README.md')) {
        $src = Join-Path $repoRoot $doc
        if (Test-Path $src) { Copy-Item $src $distDir }
    }
    $examples = Join-Path $repoRoot 'examples'
    if (Test-Path $examples) { Copy-Item $examples $distDir -Recurse }
    Set-Content -Path (Join-Path $distDir 'version.txt') -Value $effectiveVersion -Encoding ASCII
    Write-Host "[INFO] deployed:    $distDir"
}

if ($ZipPath) {
    if ($NoDeploy) { throw "-ZipPath needs the deployed folder; drop -NoDeploy" }
    $zipFull = [System.IO.Path]::GetFullPath($ZipPath)
    $zipDir = Split-Path $zipFull -Parent
    New-Item -ItemType Directory -Force -Path $zipDir | Out-Null
    $stageName = "qwen-image-harness-$effectiveVersion-windows-x64"
    $stage = Join-Path $zipDir $stageName
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
    Copy-Item $distDir $stage -Recurse
    Set-Content -Path (Join-Path $stage 'version.txt') -Value $effectiveVersion -Encoding ASCII
    if (Test-Path $zipFull) { Remove-Item $zipFull -Force }
    Compress-Archive -Path $stage -DestinationPath $zipFull
    Remove-Item $stage -Recurse -Force
    Write-Host "[INFO] archive:     $zipFull"
}

Write-Host ""
Write-Host "[ok] $effectiveVersion -> $exe"
if (-not $NoDeploy) {
    Write-Host "[ok] standalone folder: $distDir"
}
