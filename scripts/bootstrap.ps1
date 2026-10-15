<#
.SYNOPSIS
  One-time setup of the Windows build prerequisites and dependencies.
  Installs missing tools with winget (Git, CMake, Ninja, Python 3.12, VS 2022 Build Tools with the
  C++ workload), then vcpkg and Qt into .deps\ next to the repository.
  CUDA Toolkit 12.x is optional; when nvcc is on PATH the CUDA resampler is built.
  The RED R3D SDK is optional and proprietary: download it from red.com and set R3DSDK_ROOT.
#>
param(
  [string]$QtVersion = "6.8.3",
  [string]$QtArch = "win64_msvc2022_64",
  [string]$VcpkgTag = "2026.07.29",
  [switch]$NoWinget
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$deps = Join-Path $root ".deps"
New-Item -ItemType Directory -Force $deps | Out-Null

function Invoke-Native {
  # Runs a native command and fails the script if it returns a non-zero exit code.
  param([Parameter(Mandatory)][string]$Exe, [Parameter(ValueFromRemainingArguments)][string[]]$Args)
  & $Exe @Args
  if ($LASTEXITCODE -ne 0) { throw "$Exe $($Args -join ' ') failed with exit code $LASTEXITCODE" }
}

function Install-IfMissing {
  param([string]$Command, [string]$WingetId, [string]$Extra = "")
  if (Get-Command $Command -ErrorAction SilentlyContinue) { return }
  if ($NoWinget) { throw "$Command is missing; install it or run without -NoWinget" }
  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) { throw "$Command is missing and winget is unavailable; install $WingetId manually" }
  Write-Host "== installing $WingetId with winget"
  $argsList = @("install", "--id", $WingetId, "-e", "--accept-source-agreements", "--accept-package-agreements", "--silent")
  if ($Extra) { $argsList += @("--override", $Extra) }
  Invoke-Native winget @argsList
  $env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [Environment]::GetEnvironmentVariable("Path", "User")
}

Install-IfMissing -Command git -WingetId "Git.Git"
Install-IfMissing -Command cmake -WingetId "Kitware.CMake"
Install-IfMissing -Command ninja -WingetId "Ninja-build.Ninja"

# Python: prefer the py launcher, then python3 / python.
$python = $null
foreach ($candidate in @(@("py", "-3"), @("python3"), @("python"))) {
  if (Get-Command $candidate[0] -ErrorAction SilentlyContinue) {
    & $candidate[0] @($candidate[1..($candidate.Length - 1)]) -c "import sys; sys.exit(0 if sys.version_info >= (3, 9) else 1)" 2>$null
    if ($LASTEXITCODE -eq 0) { $python = $candidate; break }
  }
}
if (-not $python) {
  Install-IfMissing -Command "py" -WingetId "Python.Python.3.12"
  $python = @("py", "-3")
}
Write-Host "Python: $($python -join ' ')"

# Visual Studio 2022 C++ tools.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = $null
if (Test-Path $vswhere) {
  $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $vs) {
  if ($NoWinget) { throw "Visual Studio 2022 with the C++ workload is missing" }
  Write-Host "== installing Visual Studio 2022 Build Tools (C++ workload) with winget"
  Invoke-Native winget install --id Microsoft.VisualStudio.2022.BuildTools -e --accept-source-agreements --accept-package-agreements --silent --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
}

# vcpkg
if (-not $env:VCPKG_ROOT) {
  $vcpkg = Join-Path $deps "vcpkg"
  if (-not (Test-Path (Join-Path $vcpkg "vcpkg.exe"))) {
    Write-Host "== cloning vcpkg $VcpkgTag into $vcpkg"
    if (-not (Test-Path $vcpkg)) { Invoke-Native git clone https://github.com/microsoft/vcpkg $vcpkg }
    Invoke-Native git -C $vcpkg checkout $VcpkgTag
    Invoke-Native (Join-Path $vcpkg "bootstrap-vcpkg.bat") -disableMetrics
  }
  $env:VCPKG_ROOT = $vcpkg
}
Write-Host "VCPKG_ROOT=$env:VCPKG_ROOT"

# Qt
if (-not $env:QT_ROOT) {
  $qt = Join-Path $deps "qt"
  $qtDir = Join-Path $qt "$QtVersion\msvc2022_64"
  if (-not (Test-Path (Join-Path $qtDir "bin\Qt6Core.dll"))) {
    Write-Host "== installing Qt $QtVersion ($QtArch) into $qt via aqtinstall"
    $py = $python[0]; $pyArgs = @($python[1..($python.Length - 1)]) | Where-Object { $_ }
    Invoke-Native $py @($pyArgs + @("-m", "pip", "install", "--user", "--upgrade", "aqtinstall"))
    Invoke-Native $py @($pyArgs + @("-m", "aqt", "install-qt", "windows", "desktop", $QtVersion, $QtArch, "-O", $qt))
  }
  $env:QT_ROOT = $qtDir
}
Write-Host "QT_ROOT=$env:QT_ROOT"
if ($env:R3DSDK_ROOT) { Write-Host "R3DSDK_ROOT=$env:R3DSDK_ROOT" } else { Write-Host "R3DSDK_ROOT not set: REDCODE decoding will be unavailable in this build" }

[Environment]::SetEnvironmentVariable("VCPKG_ROOT", $env:VCPKG_ROOT, "User")
[Environment]::SetEnvironmentVariable("QT_ROOT", $env:QT_ROOT, "User")
Write-Host "Dependencies ready. Next: scripts\build.ps1"
