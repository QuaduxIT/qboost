<#
  @license Apache-2.0
  Copyright © 2026 Quadux IT GmbH
     ____                  __              __________   ______          __    __  __
    / __ \__  ______ _____/ /_  ___  __   /  _/_  __/  / ____/___ ___  / /_  / / / /
   / / / / / / / __ `/ __  / / / / |/_/   / /  / /    / / __/ __ `__ \/ __ \/ /_/ /
  / /_/ / /_/ / /_/ / /_/ / /_/ />  <   _/ /  / /    / /_/ / / / / / / /_/ / __  /
  \___\_\__,_/\__,_/\__,_/\__,_/_/|_|  /___/ /_/     \____/_/ /_/ /_/_.___/_/ /_/

  qboost
  Author: Walter Hoffmann
#>
# build.ps1 - kompiliert qboost-cli.exe mit MSVC (x64).
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath  = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if (-not $vsPath) { throw "Keine VC++-Build-Tools gefunden." }
$devcmd = Join-Path $vsPath 'Common7\Tools\VsDevCmd.bat'

$out = Join-Path $here 'build'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$cmd = @"
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "$devcmd" -arch=amd64 -host_arch=amd64 >nul
cd /d "$out"
cl /nologo /O2 /EHsc /std:c++17 /DUNICODE /D_UNICODE /W3 ^
   "$here\qboost-cli.cpp" ^
   /Fe:qboost-cli.exe ^
   /link ole32.lib propsys.lib advapi32.lib legacy_stdio_definitions.lib
"@
$bat = Join-Path $out '_build.bat'
Set-Content -Path $bat -Value $cmd -Encoding Ascii
$prev = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
cmd.exe /c "`"$bat`"" 2>&1 | ForEach-Object { "$_" }
$ErrorActionPreference = $prev
if ($LASTEXITCODE -ne 0) { throw "Build fehlgeschlagen (Exit $LASTEXITCODE)." }
Write-Host "OK -> $out\qboost-cli.exe"
