[![Quadux IT Logo](https://quadux.it/Logo.png)](https://quadux.it/)

Copyright © 2026 Quadux IT GmbH

License: Apache 2.0 (see LICENSE)
Author: Walter Hoffmann

# qboost

[![Release](https://img.shields.io/github/v/release/QuaduxIT/qboost)](https://github.com/QuaduxIT/qboost/releases)
[![License: Apache 2.0](https://img.shields.io/badge/license-Apache%202.0-blue.svg)](LICENSE)
[![GitHub](https://img.shields.io/badge/github-QuaduxIT%2Fqboost-181717?logo=github)](https://github.com/QuaduxIT/qboost)
[![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-0078D6?logo=windows)](#)

Systemweiter Lautstärke-Verstärker für Windows. Hebt den Wiedergabepegel eines
Audio-Endpoints über die 100-%-Grenze, die Windows sonst setzt.

Die Verstärkung passiert in einer **APO** (Audio Processing Object) — einem
Audio-Plugin, das die Windows-Audio-Engine in ihre eigene Effektkette lädt.
Gesteuert wird über ein Kommandozeilenwerkzeug; ein Hintergrundprozess läuft
nicht mit.

Kein Treiber: Die APO ist eine gewöhnliche User-Mode-COM-DLL. Kein Kernelcode,
keine Signaturpflicht, kein Bluescreen-Risiko.

Installiert wird das Paket unter dem Namen **Quadux Boost**; das
Kommandozeilenwerkzeug heißt `qboost-cli`.

## Installieren

Das fertige `QuaduxBoost.msi` hängt an jedem [Release](../../releases); es wird
dort automatisch aus den Quellen gebaut und liegt deshalb nicht im Repository.

```powershell
msiexec /i QuaduxBoost.msi        # mit Oberfläche
msiexec /i QuaduxBoost.msi /qn    # ohne Rückfragen
msiexec /x QuaduxBoost.msi        # entfernen
```

Das Paket legt die Dateien nach `%ProgramFiles%\QuaduxBoost\` und nimmt das
Verzeichnis in den System-PATH auf. Die Einrichtung übernimmt das Programm
selbst: registrieren, Effektkette umhängen, Originalzustand sichern und
**messen, ob tatsächlich verstärkt wird**. Schlägt das fehl, baut es alles
zurück. Nach der Installation steht die Lautstärke auf 200 %.

Beim Entfernen wird die ursprüngliche Effektkette wiederhergestellt.

## Steuern

```powershell
qboost-cli set 250    # 250 % Lautstärke
qboost-cli set 100    # neutral
qboost-cli off        # Boost aus
qboost-cli status     # Gerät, Effektslots, aktueller Gain
qboost-cli selftest   # misst per Testton, ob die Verstärkung greift
```

Die Einstellung liegt in `%ProgramData%\QuaduxBoost\state.bin` und übersteht
Neustarts. Änderungen greifen sofort, ohne Neustart des Audiodienstes.

Ein harter Begrenzer bei 0 dBFS verhindert Überläufe; oberhalb von etwa 400 %
verzerrt es je nach Material hörbar.

## Bauen

Voraussetzungen: Visual Studio Build Tools (MSVC x64) und Windows SDK.
WiX wird beim ersten Lauf nach `msi\wix\` geladen und ist nicht Teil des
Repositorys.

```powershell
.\msi\build-msi.ps1     # baut alles und schnürt dist\QuaduxBoost.msi
```

Einzeln:

```powershell
.\apo\build.ps1         # APO-DLL
.\apo\build.ps1 -Diag   # Diagnosebau (fester Gain, Meldungen über Named Pipe)
.\cli\build.ps1         # Kommandozeilenwerkzeug
.\test\build.ps1        # APO isoliert prüfen, ohne Registrierung und ohne Admin
```

## Aufbau

```
apo/     APO-DLL (C++/COM) — die eigentliche Verstärkung
cli/     qboost-cli: Steuerung und Einrichtungslogik (setup.h)
msi/     Paketdefinition (WiX)
test/    apotest.exe — fährt die APO isoliert durch
dist/    Bauergebnis (nicht im Repository)
docs/    Notizen zur Windows-Audiokette
```

Der Gain wird im Echtzeit-Audio-Thread gelesen (lock-frei, ohne Allokation).
Wie die APO in die Audiokette eingehängt wird und welche Fallstricke dabei
lauern, steht in [docs/windows-apo.md](docs/windows-apo.md).

## Lizenz

Apache-2.0 — siehe [LICENSE](LICENSE). © 2026 Quadux IT.
