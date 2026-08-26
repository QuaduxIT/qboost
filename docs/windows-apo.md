# Windows-APO: Aufbau und Fallstricke

Notizen zur Einbindung einer eigenen APO in die Windows-Audiokette.
Alle Angaben stammen aus Messungen auf Windows 11 (Build 26100) mit
einem HD-Audio-Endpoint. Jeder einzelne Punkt führt sonst zu einer APO, die
entweder stillschweigend ignoriert wird oder das Gerät verstummen lässt.

**1. Der richtige Registry-Zweig.**
`IMMDevice::OpenPropertyStore` schreibt nach `…\Properties`. Die Effektkette
liest Windows aber aus `…\FxProperties` des Endpoints. Einträge über den
PropertyStore bleiben deshalb wirkungslos — ohne jede Fehlermeldung.

**2. Der Slot entscheidet — und zwar SFX.**
Es gibt drei Effekt-Slots:

| Slot | Wert | Ergebnis |
|---|---|---|
| **SFX (Stream)** | `{d04e05a6-…},5` | **funktioniert** — APO verarbeitet |
| MFX (Mode) | `{d04e05a6-…},6` | APO wird erzeugt, aber nie verarbeitet → **Stille** |
| EFX (Endpoint) | `{d04e05a6-…},7` | ebenfalls **Stille** |
| Composite | `,13` / `,14` / `,15` | wird gar nicht erst instanziiert |

Dazu gehört zwingend die Modus-Liste im selben Schlüssel:

| Wert | Typ | Inhalt |
|---|---|---|
| `{d04e05a6-…},5` | `REG_SZ` | CLSID der APO |
| `{d3993a3f-…},5` | `REG_MULTI_SZ` | unterstützte Verarbeitungsmodi |

Wird nur `AUDIO_SIGNALPROCESSINGMODE_DEFAULT` eingetragen, bleibt die APO bei
Streams in anderen Modi (Media, Movie …) außen vor.

**3. Rechte auf `FxProperties`.**
Administratoren haben dort nur `SetValue` + `ReadKey`, **kein** `FullControl`.
`OpenSubKey(pfad, $true)` fordert zu viel an und scheitert; der Schlüssel muss
mit genau diesen beiden Rechten geöffnet werden.

**4. COM-Aggregation ist Pflicht.**
Windows erzeugt System-Effekt-APOs **aggregiert**, übergibt also ein äußeres
`IUnknown`. Eine Klassenfabrik, die das mit `CLASS_E_NOAGGREGATION` ablehnt,
lässt die APO nie entstehen — und die Engine liefert dann **komplette Stille**
statt auf die Standardkette zurückzufallen. Nötig ist das klassische Muster aus
delegierendem und nicht delegierendem `IUnknown`.

**5. Erwartete Schnittstellen.**
Abgefragt werden `IAudioProcessingObject`, `IAudioSystemEffects`,
`IAudioSystemEffects3` und `IAudioProcessingObjectNotifications`. Fehlt eine,
bricht der Aufbau ab.

**6. Formate streng prüfen.**
Ein blindes `S_OK` auf jede Formatanfrage führt dazu, dass die Engine
Kombinationen aushandelt, die `APOProcess` nicht bedienen kann. Nicht
unterstützte Formate mit `APOERR_FORMAT_NOT_SUPPORTED` ablehnen — dann nimmt
Windows die APO sauber aus der Kette und der Ton läuft unverstärkt weiter.

### Diagnose aus `audiodg.exe` heraus

`audiodg.exe` läuft mit einem schreibbeschränkten Token: **Dateien schreiben ist
unmöglich**, auch bei offener ACL. Telemetrie aus der APO läuft deshalb über
eine Named Pipe (`qboost-cli logserver`); der Diagnosebau (`build.ps1 -Diag`)
meldet darüber jeden Schritt der Aushandlung.

Zum Messen des tatsächlichen Ausgangspegels dient `qboost-cli meter` — es liest
`IAudioMeterInformation` am Endpoint und ist damit unabhängig davon, ob die APO
selbst etwas zurückmelden kann.

### Verifikation

Mit `qboost-cli meter` bei laufender Wiedergabe gemessen (Testton, Gain live
umgeschaltet, ohne Neustart dazwischen):

```
Gain 100 %  ->  endpoint-peak 0.1831
Gain 200 %  ->  endpoint-peak 0.3662     (exakt x2)
Gain 300 %  ->  endpoint-peak 0.5493     (exakt x3)
Gain 600 %  ->  endpoint-peak 1.0000     (Begrenzer greift)
```

Der Zaehler der APO meldet dabei `lock=1 process=3167 channels=2` — die
Verstaerkung passiert also nachweislich im Audiograph und laesst sich im
laufenden Betrieb aendern.
