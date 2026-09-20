# AGENTS.md – Projektkontext Autofrontcam

> Diese Datei wird automatisch gelesen, wenn dieser Ordner als Arbeitsbereich
> geoeffnet ist. Sie enthaelt die geprueften Fakten, damit eine frische Sitzung
> ohne Memory-Zugriff sofort weiterarbeiten kann. (Angelegt 2026-09-20.)

## Was ist das?

Eine **WiFi-Seitenkamera fuer ein Fahrzeug**: Ein **AI-Thinker ESP32-CAM** mit
OV2640 macht einen eigenen WLAN-Zugangspunkt auf und liefert laufend Bilder.
Im Fahrzeug zeigt ein **Display** das Bild an. Es gibt drei Display-Varianten,
die alle dasselbe Bild holen.

Das Repo ist ein **Mehrgeraete-Projekt**: vier eigenstaendige ESP-IDF-Projekte
in Unterordnern, alle auf dem Branch `main` (es gibt nur `main`).

| Ordner | Geraet | Rolle |
|---|---|---|
| `esp32cam/` | AI-Thinker ESP32-CAM (ESP32-D0WD-V3 + OV2640) | Kamera + Webserver + OTA |
| `cyd/` | CYD ESP32-2432S028R, ILI9341 320x240 | Display-Client (Touch deaktiviert) |
| `tft/` | ESP32-D0WD-V3 + 2.8" SPI ILI9341 240x320 | Display-Client (zur Zeit Testmodus) |
| `s3lcd/` | LCDWIKI ES3C40P, ESP32-S3, ST7796S 480x320 + FT6336U | Display-Client (Hauptgeraet) |

Gemeinsame Bausteine: `components/nvs_config/`, `tools/increment_build.py`.
Der Build-Zaehler ist **global** (`.build_number` im Repo-Root), die Version
`v0.1.xx` zaehlt also ueber alle vier Projekte gemeinsam.

## Netzwerk und Adressen (geprueft)

- Kamera: SoftAP **`Cam-AP`, offen (kein Passwort)**, IP **10.1.1.1**,
  Name im Netz **`autocam`**. Client-Adressen: `s3lcd` = **10.1.1.3**,
  `cyd` und `tft` = **10.1.1.2**.
- **Achtung:** `cyd` und `tft` haben dieselbe Adresse -> nie gleichzeitig betreiben.
- Kamera-Tueren: **Port 80** = Webseite, `/capture`, `/api/config`, OTA,
  Captive Portal. **Port 8080** = laufender Bildstrom (4 Byte Laenge + JPEG,
  dauerhafte Verbindung) - **das ist der Weg, den alle Displays nutzen**.
  `MJPEG_PORT 81` in `esp32cam/include/config.h` ist nur ein Rest ohne Code.
- `/capture` bedient nur einen Client; andere bekommen **503** (Zeitlimit 10 s).
- Die Kamera liefert real **CIF 400x296** (`CAM_FRAME_SIZE=FRAMESIZE_QVGA`,
  das ist das JPEG-Minimum des OV2640) und etwa **3 Bilder/s**. Die Kamera ist
  der Bremsklotz, nicht das Display.

## Bauen und flashen (Windows)

```powershell
. "C:\Users\win4g\Downloads\GitHub\VS-Projekte\CascadeProjects\esp-idf\export.ps1" *> $null
$env:PATH = (($env:PATH -split ';' | Where-Object { $_ -ne '' }) | Select-Object -Unique) -join ';'
cd <projektordner>; idf.py -p COMx flash
```

- **COM5** = ESP32-CAM (CH340) - dort kommt **keine Konsolenausgabe** an, nur
  flashen. Zum Pruefen immer das Display-Log ansehen.
- **COM7** = s3lcd (USB-Serial/JTAG), **COM4** = cyd, **COM3** = tft.
  Die Nummern wechseln beim Umstecken.
- Vor dem Flashen verwaiste `idf_monitor.py`-Prozesse beenden (belegen den Port).
- **PATH immer deduplizieren** (siehe oben). Ohne das waechst der PATH ueber
  32 KB und Windows startet keine Prozesse mehr - Build und Flash laufen dann
  still ins Leere, mit leerer Ausgabe.

## Stolperfallen (alle schon einmal aufgetreten)

- `include/version.h` wird beim Bauen erzeugt und ist per `.gitignore`
  ausgeschlossen. Aendert sich nur diese Datei, wird `main.c` nicht neu
  uebersetzt und das Geraet meldet die **alte** Version. Vor dem Flash:
  `(Get-Item main\main.c).LastWriteTime = Get-Date`.
- Ein Watchdog, der nur prueft "laeuft der Leerlauf-Task", erkennt haengende
  Dienste **nicht**. Dafuer braucht es Dienst-Pruefungen (Loopback-Test oder
  eine Gesundheits-Ampel wie im Kamera-Projekt).
- lwIP-Sockets brauchen immer Sende-/Empfangs-Zeitlimits. Ohne
  `SO_SNDTIMEO` blockiert `send()` dauerhaft, wenn der Client weg ist.
- `esp_http_client_perform()` liest den Antwort-Text selbst weg. Wer ihn
  braucht, sammelt ihn im Event-Handler (`HTTP_EVENT_ON_DATA`).
- `/api/config` der Kamera antwortet auf **jeden** POST mit 200 "OK".
  Der Statuscode allein beweist also nichts - den Text pruefen.
- Farben: `s3lcd` dekodiert mit `swap_color_bytes = 0`, `cyd` mit `1`.
- NVS: `nvs_config_get_str(key, NULL)` stuerzt ab -> immer `""` als Vorgabe.

## Bedienung des s3lcd (Hauptgeraet)

- Bild antippen und **2 Sekunden halten** -> Funktionsraster erscheint
  (kurzes Antippen macht absichtlich nichts).
- Raster 5x3: Linienposition/-winkel, Liniendicke (3x), `DREH-BILD`,
  `STRECKEN`, `VERSCHIEBEN`, `HELLIGKEIT`, `KONTRAST`, `VERBINDEN`,
  `CAM-RESET`, `ENDE`.
- Waagerechte Fingerbewegung = erster Wert (z. B. Streckung X),
  senkrechte = zweiter Wert (z. B. Streckung Y).
- Nach **3 Sekunden** ohne Beruehrung wird gespeichert (nur bei Aenderung)
  und das normale Videobild ist wieder aktiv.
- Helligkeit und Kontrast regelt der Client selbst (LUT in `stream.c`);
  die Kamera-Werte werden einmalig auf 0 gesetzt.

## Wichtige Dateien

- `README.md` (Repo-Uebersicht) - **teilweise veraltet**: nennt noch den
  Branch `tft` und beschreibt den Bildstrom nur ueber `/capture`.
- `s3lcd/main/ui.c` - Touch-Bedienung, Raster, NVS-Speicherung
- `s3lcd/main/stream.c` - Bild holen, JPEG dekodieren, anzeigen
- `s3lcd/main/display.c` - ST7796S-Treiber, Drehen/Strecken/Verschieben
- `esp32cam/main/main.c` - Webserver, Bildstrom, Selbstueberwachung (Watchdog)
- `esp32cam/main/camera.c` - Kamera-Start und Wiederherstellung
- `esp32cam/include/config.h`, `s3lcd/include/config.h` - alle Pins/Parameter

## Stand (2026-09-20)

- `s3lcd` laeuft mit **v0.1.313** (Raster 5x3, Strecken, Verschieben,
  2-Sekunden-Halten, Startbild).
- `esp32cam` ist mit **v0.1.303** geflasht (Selbstueberwachung,
  Kamera-Wiederherstellung, Socket-Haerten, Fernneustart).
- Letzter Commit: `22eef4f` auf `main`, gepusht nach
  `github.com:softrider70/autofrontcam.git`.
- `tft` ist im Testmodus (`TFT_TEST_MODE=1`) und zeigt noch nichts an -
  vermutete Ursache Wackelkontakt, seither nicht erneut geprueft.
