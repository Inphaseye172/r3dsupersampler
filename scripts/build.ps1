<#
.SYNOPSIS
  One-command Windows build from a clean clone: bootstrap deps, configure, build, test, install, package.
  Usage: powershell -ExecutionPolicy Bypass -File scripts\build.ps1 [-Config Release|Debug] [-SkipTests] [-Installer]
#>
param(
  [string]$Config = "Release",
  [switch]$SkipTests,
  [switch]$Installer
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root
& (Join-Path $root "scripts\bootstrap.ps1")
$preset = if ($Config -eq "Debug") { "windows-debug" } else { "windows-release" }

# Use the VS developer environment if cl.exe is not already on PATH.
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  Import-Module (Join-Path $vs "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
  Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64"
}

cmake --preset $preset
cmake --build --preset $preset
if (-not $SkipTests) { ctest --preset $preset }
cmake --install "build\$preset" --prefix dist
Compress-Archive -Path dist\* -DestinationPath "R3DSupersampler-win64.zip" -Force
Write-Host "Portable build: R3DSupersampler-win64.zip (dist\bin\R3DSupersampler.exe)"
if ($Installer) { & (Join-Path $root "scripts\make-installer.ps1") }
