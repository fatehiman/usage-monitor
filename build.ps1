# Build Usage Monitor with MinGW-w64 GCC (WinLibs).
#   .\build.ps1          build to build\usage-monitor.exe (+ copy services\ samples if missing)
#   .\build.ps1 -Run     stop the running copy, build, start the new exe
param([switch]$Run)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

# Find g++ / windres: PATH first, then the WinLibs winget folder.
$gpp = (Get-Command g++ -ErrorAction SilentlyContinue).Source
if (-not $gpp) {
    $gpp = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\BrechtSanders.WinLibs*\mingw64\bin\g++.exe" -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $gpp) { throw 'g++ not found. Install: winget install BrechtSanders.WinLibs.POSIX.UCRT' }
$bin = Split-Path $gpp
$windres = Join-Path $bin 'windres.exe'
$env:PATH = "$bin;$env:PATH"

$exe = Join-Path $PSScriptRoot 'build\usage-monitor.exe'
if ($Run) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File "$HOME\.claude\skills\desktop-app-rules\stop-app.ps1" -Path $exe
}

New-Item -ItemType Directory -Force build | Out-Null
& $windres res\app.rc -O coff -o build\app.res.o
if ($LASTEXITCODE) { throw 'windres failed' }

$src = Get-ChildItem src\*.cpp | ForEach-Object { $_.FullName }
& $gpp -std=c++17 -Os -s -municode -mwindows -static `
    -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 `
    -ffunction-sections -fdata-sections '-Wl,--gc-sections' -Wall `
    $src build\app.res.o `
    -lwinhttp -lbcrypt -lmsimg32 -lcomctl32 -lshell32 -ladvapi32 -luser32 -lgdi32 `
    -o $exe
if ($LASTEXITCODE) { throw 'compile failed' }

# Copy sample services only when the target file does not exist (never overwrite user edits).
New-Item -ItemType Directory -Force build\services | Out-Null
Get-ChildItem services -File | ForEach-Object {
    $dst = Join-Path 'build\services' $_.Name
    if (-not (Test-Path $dst)) { Copy-Item $_.FullName $dst }
}

'{0:N0} KB  {1}' -f ((Get-Item $exe).Length / 1KB), $exe
if ($Run) { Start-Process $exe }
