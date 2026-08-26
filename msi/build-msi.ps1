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
# build-msi.ps1 - baut das Installationspaket dist\QuaduxBoost.msi
#
# Baut vorher die beiden Binaerdateien neu, damit im MSI garantiert der
# aktuelle Stand landet. WiX liegt als Binaerpaket unter msi\wix und muss
# nicht installiert sein.
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here

$candle = Join-Path $here 'wix\candle.exe'
$light  = Join-Path $here 'wix\light.exe'

# WiX ist ein reines Binaerpaket - kein Setup noetig. Fehlt es, einmalig holen.
if (-not (Test-Path $candle)) {
    Write-Host "Hole WiX-Toolset ..." -ForegroundColor Cyan
    $zip = Join-Path $env:TEMP 'wix314-binaries.zip'
    $url = 'https://github.com/wixtoolset/wix3/releases/download/wix3141rtm/wix314-binaries.zip'
    curl.exe -sSL -o $zip $url
    if (-not (Test-Path $zip)) { throw "WiX-Download fehlgeschlagen: $url" }
    Expand-Archive $zip -DestinationPath (Join-Path $here 'wix') -Force
    Remove-Item $zip -Force -ErrorAction SilentlyContinue
}
if (-not (Test-Path $candle)) { throw "WiX fehlt weiterhin: $candle" }

# 1) Binaerdateien bauen
Write-Host "Baue APO-DLL und CLI ..." -ForegroundColor Cyan
& (Join-Path $root 'apo\build.ps1') | Where-Object { $_ -match 'OK ->' }
& (Join-Path $root 'cli\build.ps1') | Where-Object { $_ -match 'OK ->' }

$apoDll = Join-Path $root 'apo\build\QuaduxBoostApo.dll'
$cliExe = Join-Path $root 'cli\build\qboost-cli.exe'
foreach ($f in @($apoDll, $cliExe)) {
    if (-not (Test-Path $f)) { throw "Datei fehlt: $f" }
}

# 2) Paket erzeugen
$out = Join-Path $root 'dist'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$obj = Join-Path $here 'obj'
New-Item -ItemType Directory -Force -Path $obj | Out-Null

Write-Host "Erzeuge MSI ..." -ForegroundColor Cyan
& $candle -nologo -arch x64 `
    "-dApoDll=$apoDll" "-dCliExe=$cliExe" `
    -out "$obj\QuaduxBoost.wixobj" (Join-Path $here 'QuaduxBoost.wxs')
if ($LASTEXITCODE -ne 0) { throw "candle fehlgeschlagen ($LASTEXITCODE)" }

$msi = Join-Path $out 'QuaduxBoost.msi'
# -spdb: keine .wixpdb erzeugen. Die braeuchte man nur fuer MSP-Patches
# gegen genau dieses Build - das Paket ersetzt sich per MajorUpgrade ohnehin.
& $light -nologo -sval -spdb -out $msi "$obj\QuaduxBoost.wixobj"
if ($LASTEXITCODE -ne 0) { throw "light fehlgeschlagen ($LASTEXITCODE)" }

$size = [math]::Round((Get-Item $msi).Length / 1KB, 1)
Write-Host "OK -> $msi  ($size KB)" -ForegroundColor Green
