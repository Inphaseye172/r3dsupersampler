<#
.SYNOPSIS
  One-time setup of the Windows build dependencies (vcpkg + Qt) into .deps\ next to the repo.
  Requires: Visual Studio 2022 (Desktop C++ workload), CMake >= 3.25, Ninja, Python 3, git.
  CUDA Toolkit 12.x is optional; when nvcc is on PATH the CUDA resampler is built.
#>
param(
  [string]$QtVersion = "6.8.3",
  [string]$QtArch = "win64_msvc2022_64"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$deps = Join-Path $root ".deps"
New-Item -ItemType Directory -Force $deps | Out-Null

if (-not $env:VCPKG_ROOT) {
  $vcpkg = Join-Path $deps "vcpkg"
  if (-not (Test-Path (Join-Path $vcpkg "vcpkg.exe"))) {
    Write-Host "== cloning vcpkg into $vcpkg"
    git clone https://github.com/microsoft/vcpkg $vcpkg
    git -C $vcpkg checkout 2026.07.29
    & (Join-Path $vcpkg "bootstrap-vcpkg.bat") -disableMetrics
  }
  $env:VCPKG_ROOT = $vcpkg
}
Write-Host "VCPKG_ROOT=$env:VCPKG_ROOT"

if (-not $env:QT_ROOT) {
  $qt = Join-Path $deps "qt"
  $qtDir = Join-Path $qt "$QtVersion\msvc2022_64"
  if (-not (Test-Path (Join-Path $qtDir "bin\Qt6Core.dll"))) {
    Write-Host "== installing Qt $QtVersion ($QtArch) into $qt via aqtinstall"
    python -m pip install --user --upgrade aqtinstall
    python -m aqt install-qt windows desktop $QtVersion $QtArch -O $qt
  }
  $env:QT_ROOT = $qtDir
}
Write-Host "QT_ROOT=$env:QT_ROOT"

[Environment]::SetEnvironmentVariable("VCPKG_ROOT", $env:VCPKG_ROOT, "User")
[Environment]::SetEnvironmentVariable("QT_ROOT", $env:QT_ROOT, "User")
Write-Host "Dependencies ready. Next: scripts\build.ps1"
