<#
.SYNOPSIS
  One-command Windows build from a clean clone: bootstrap deps, configure, build, test, install, package.
  Usage: powershell -ExecutionPolicy Bypass -File scripts\build.ps1 [-Config Release|Debug] [-SkipTests] [-Installer] [-FetchSamples]
#>
param(
  [string]$Config = "Release",
  [switch]$SkipTests,
  [switch]$Installer,
  [switch]$FetchSamples
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root
& (Join-Path $root "scripts\bootstrap.ps1")
if ($LASTEXITCODE -ne 0) { throw "bootstrap failed" }
$preset = if ($Config -eq "Debug") { "windows-debug" } else { "windows-release" }

function Invoke-Native {
  param([Parameter(Mandatory)][string]$Exe, [Parameter(ValueFromRemainingArguments)][string[]]$Args)
  & $Exe @Args
  if ($LASTEXITCODE -ne 0) { throw "$Exe $($Args -join ' ') failed with exit code $LASTEXITCODE" }
}

# Use the VS developer environment if cl.exe is not already on PATH.
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if (-not $vs) { throw "Visual Studio 2022 with the C++ workload was not found (run scripts\bootstrap.ps1)" }
  Import-Module (Join-Path $vs "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
  Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64"
}

if ($FetchSamples) {
  $py = if (Get-Command py -ErrorAction SilentlyContinue) { @("py", "-3") } else { @("python") }
  Invoke-Native $py[0] @($py[1..($py.Length - 1)] | Where-Object { $_ }) (Join-Path $root "scripts\fetch_samples.py")
  $env:R3DSS_SAMPLES = Join-Path $root ".deps\samples"
}

Invoke-Native cmake --preset $preset
Invoke-Native cmake --build --preset $preset
if (-not $SkipTests) { Invoke-Native ctest --preset $preset }
Invoke-Native cmake --install "build\$preset" --prefix dist
if (-not (Test-Path "dist\bin\Supersampler.exe")) {
  throw "GUI was not built: Qt6 Widgets was not found at configure time (QT_ROOT='$env:QT_ROOT'). Run scripts\bootstrap.ps1 or set QT_ROOT and rebuild."
}
if (-not (Test-Path "dist\bin\r3dss-cli.exe")) { throw "r3dss-cli.exe missing from dist\bin: the build is incomplete" }
Compress-Archive -Path dist\* -DestinationPath "Supersampler-win64.zip" -Force
Write-Host "Portable build: Supersampler-win64.zip (dist\bin\Supersampler.exe)"
if ($Installer) { & (Join-Path $root "scripts\make-installer.ps1"); if ($LASTEXITCODE -ne 0) { throw "installer failed" } }
