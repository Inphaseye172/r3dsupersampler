# Builds the Inno Setup installer from dist\ (run scripts\build.ps1 first).
param([string]$Version = "0.2.0")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$iscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
if (-not (Test-Path $iscc)) { throw "Inno Setup 6 not found at $iscc (https://jrsoftware.org/isinfo.php)" }
& $iscc "/DSourceDir=$root\dist" "/DAppVersion=$Version" (Join-Path $root "installer\supersampler.iss")
if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit code $LASTEXITCODE" }
Write-Host "Installer written to installer\Output"
