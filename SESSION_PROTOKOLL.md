# Session-Protokoll — Erstellung des Projekts „autofrontcam"

Datum: 2026-08-06

## Ausgangslage / Auftrag

Der Nutzer beauftragte, ein neues Projekt **„autofrontcam“** (Autofrontkamera) auf Basis des
bestehenden LoRa-Projekts (`lora`) zu erstellen. Das `lora`-Projekt diente als ESP-IDF-Vorlage.
Zusätzlich sollte der angeschlossene Chip auf **COM4** ausgelesen und das Projekt an die
Hardware angepasst werden.

## Schritt 1 — Chip auf COM4 identifiziert

Über `esptool` (ESP-IDF) wurden folgende Kerndaten vom Gerät auf COM4 ausgelesen:

| Eigenschaft | Wert |
|---|---|
| **Chip** | ESP32-D0WD-V3 (Rev. v3.0) — klassischer ESP32 (kein S3) |
| **Features** | Wi-Fi, BT, Dual Core, 240MHz |
| **Crystal** | 40MHz |
| **MAC** | B0:A7:32:DD:B7:F4 |
| **Flash** | 4MB |
| **Port** | COM4 (USB-SERIAL CH340) |

**Ergebnis:** Es handelt sich um das **AI-Thinker ESP32-CAM** Modul (OV2640-Kamera + 4MB PSRAM + MicroSD).
Dies war die Grundlage für alle weiteren Anpassungen (klassischer ESP32 statt ESP32-S3!).

## Schritt 2 — Projektstruktur angelegt

- Neues Verzeichnis `C:\Users\win4g\Downloads\GitHub\VS-Projekte\CascadeProjects\autofrontcam`
- Basis-Struktur vom `lora`-Projekt kopiert (`CMakeLists.txt`, `sdkconfig.defaults`,
  `partitions.csv`, `components/main/`, `include/`, `tools/`)

## Schritt 3 — Dateien an die ESP32-CAM angepasst

- **`include/config.h`** — Board auf AI-Thinker ESP32-CAM umgestellt:
  - Chip: ESP32-D0WD-V3
  - Kamera: OV2640 (SCCB/I2C, DVP parallel), vollständige Pinbelegung (GPIO0–39)
  - LED: Status GPIO33 (active-low), Flash-LED GPIO4
  - Kamera-Parameter: 20MHz XCLK, JPEG, SVGA (800x600), Qualität 12
- **`include/camera.h` + `components/main/camera.c`** — neuer OV2640-Kameratreiber
  (Init, JPEG-Capture, Info-Ausgabe)
- **`components/main/main.c`** — komplett neu geschrieben:
  - MJPEG-Stream-Server auf Port 81 (`/stream`, `/capture`)
  - Web-UI (Root-Seite mit Live-Stream + Steuerlinks)
  - LED-Task, Monitore, WiFi, OTA
- **`sdkconfig.defaults`** — auf ESP32-Target umgestellt:
  - `CONFIG_IDF_TARGET="esp32"` (statt esp32s3)
  - Flash: 4MB (statt 16MB), QIO, 80MHz
  - PSRAM aktiviert (4MB, wichtig für JPEG-Puffer)
  - Console: UART (CH340) statt USB-Serial/JTAG
  - Watchdogs deaktiviert (Kamera + Stream sind zeitkritisch)
  - Compiler-Optimierung: `-Os` (Größenoptimierung)
- **`partitions.csv`** — 4MB-OTA-Layout:
  - `ota_0` + `ota_1` je 1792KB
  - `camera_data` 384KB (NVS)
- **`CMakeLists.txt`** — Projektname `autofrontcam`
- **`components/main/idf_component.yml`** — Abhängigkeit `espressif/esp32-camera` ergänzt
- **`components/main/ota.c` / `wifi.c`** — LoRa-Referenzen (Node-ID) entfernt, Texte auf Kamera angepasst

## Schritt 4 — Aufgetretene Build-Probleme und Lösungen

1. **`esp_hal_clock` fehlte** in der verwendeten IDF v6.1-dev-Installation.
   - Ursache: `esp32-camera` 2.1.7 verlangt diese Komponente ab IDF ≥ 6.0.
   - Lösung: `esp_hal_clock` als **lokale Projektkomponente** (`components/esp_hal_clock`)
     aus der IDF v6.0-Installation (`C:\esp\v6.0\esp-idf`) übernommen (nur ESP32-Anteil behalten).
   - Alternative (verworfen): esp32-camera auf 2.0.16 pinnen → scheiterte an refaktorierten
     Driver-Strukturen (`driver/ledc.h` → `esp_driver_ledc`).

2. **IRAM-Überlauf** (~26KB) beim Linken.
   - Ursache: aus dem LoRa-Projekt übernommene `CONFIG_COMPILER_OPTIMIZATION_NONE=y` (`-O0`)
     erzeugt riesige Code-Segmente im IRAM.
   - Lösung: auf `CONFIG_COMPILER_OPTIMIZATION_SIZE=y` (`-Os`) umgestellt.

3. **`-Werror`-Format-Warnungen** in `main.c`.
   - `body[512]` zu klein für HTML-Seite (~900 Bytes) → auf `body[2048]` vergrößert.
   - `part_hdr[64]` zu klein für Stream-Header → auf `part_hdr[96]` vergrößert.

4. **Python-Env-Konflikt** bei IDF v6.0 (verworfen): Die v6.1-Env passte nicht zu den
   v6.0-Constraints — deshalb weiter mit IDF v6.1-dev gebaut.

## Schritt 5 — Erfolgreicher Build

```
autofrontcam.bin  binary size 0xd4100 bytes (≈ 868 KB)
Smallest app partition is 0x1c0000 bytes. 0xebf00 bytes (53%) free.
Project build complete.
```

## Abschluss

- Alle 7 Planungs-Schritte abgeschlossen.
- Projekt baut fehlerfrei mit IDF v6.1-dev + `esp32-camera` 2.1.7 + lokaler `esp_hal_clock`.
- README.md für das neue Projekt erstellt (ohne LoRa-Anteile).
- Nächster sinnvoller Schritt: `idf.py -p COM4 flash` und Stream-Test unter `http://<ip>:81/stream`.

## Wichtige Erkenntnisse / Merkhinweise

- Das ESP32-CAM ist ein **klassischer ESP32** (LX6), nicht ESP32-S3 — Target in `sdkconfig`
  ist `esp32`, Partitionen 4MB.
- IDF v6.1-dev im Workspace **enthält kein `esp_hal_clock`** — für Kamera-Projekte lokale
  Komponente mitführen oder IDF aktualisieren.
- Für das ESP32-CAM ist **PSRAM zwingend nötig** (JPEG-Puffer), sonst läuft die Kamera nicht.
- `-O0` (Debug-Optimierung) führt bei diesem Projekt zum IRAM-Überlauf — `-Os` verwenden.

---

## Nachtrag (2026-08-06) — Zweite Session: Anwendungsfall + Implementierung

### Auftrag
Die Kamera wird an einem **Škoda an der rechten Frontseite** montiert (vor dem Seitenspiegel,
Reifenhöhe am Fänger), sicht auf die rechte Fahrzeugseite. Stromversorgung über Wandler an der
Fahrzeugbatterie im Motorraum. Bordspannung wird vom ESP überwacht; unter Grenzwert (12,8V)
geht der ESP in den bestmöglichen Sleep (nur Messpin aktiv), bei Spannungsanstieg erwacht er.

### Abgestimmte Design-Entscheidungen (in README dokumentiert)
- **Spannungsmessung:** GPIO35 (ADC1_CH7), Spannungsteiler **100k/22k** (max. 14,5V → 2,62V am
  ADC), Faktor **5,55**, Mittelwertfilter (16 Samples).
- **Betriebsmodus per Radiobutton:** „Dauerbetrieb“ (default, bis Spannungsteiler montiert) /
  „Geregelt“ (Sleep/Wake-up). Kein 0V-Trick am Grenzwert.
- **Sleep:** Deep-Sleep mit Timer-Wakeup (10s), Early-Check nach Boot, Spannungs-Monitor-Task (30s).
- **OTA:** Upload vom iPhone. OTA-Button (links unten) öffnet den Datei-Picker der
  Files-App, die `.bin` wird per `POST /update` (raw octet-stream) in die freie
  OTA-Partition geschrieben. Ablauf: `idf.py build` → AirDrop/iCloud/Mail aufs iPhone →
  in der Web-UI auswählen und hochladen. *(Zwischenlösung mit fw_store-Storage-Partition
  wurde nach Rücksprache mit dem Nutzer wieder verworfen – wäre nur sinnvoll, wenn das
  Board eh am PC wäre.)*
- **UI:** Vollbild-Stream + Canvas-Overlay, Touch-Menü (ein/aus), Linien-Buttons links,
  OTA-Button links unten, Spannung rechts oben, Linien per **Drag + 4 Buttons** (X, Winkel, Dicke),
  rote + gelbe Linie, NVS-Persistenz.
- **Stream:** `CAMERA_GRAB_LATEST` für minimale Latenz / maximale Aktualität.

### Neue Dateien
- `include/voltage.h`, `components/main/voltage.c` — Spannungsmessung
- `include/sleep.h`, `components/main/sleep.c` — Deep-Sleep/Wake-up
- `include/lines.h`, `components/main/lines.c` — Kalibrierungslinien + NVS
- `components/main/index.html` — eingebettete Touch-Web-UI (EMBED_FILES)

### Geänderte Dateien
- `include/config.h` — Volt-Pins/Faktoren, Modus-Default, Intervalle
- `components/main/main.c` — voltage/sleep/lines-Init, Config-API (`/api/config` GET/POST)
- `components/main/ota.c` — Web-OTA (`POST /update`), OTA-Button lädt vom iPhone hoch
- `components/main/camera.c` — `CAMERA_GRAB_LATEST`
- `components/main/CMakeLists.txt` — neue Quellen + `esp_adc` + `EMBED_FILES`
- `partitions.csv` — ota_0/ota_1 je 1,75MB (Upload-OTA, keine Storage-Partition)

### Build
- Fehlerfrei. App ~905 KB (0xdcfa0), 34% frei in der 1,3MB-OTA-Partition.
- Hürden: `ADC_ATTEN_DB_11` heißt in IDF v6.x `ADC_ATTEN_DB_12`; Partitions-Subtyp
  `unknown` heißt `undefined` (`ESP_PARTITION_SUBTYPE_DATA_UNDEFINED`).

### Nächster Schritt (Nutzer)
- Flash auf COM4: `idf.py -p COM4 flash`
- Firmware via AirDrop/iCloud aufs iPhone legen, dann OTA-Button in der Web-UI testen.
- Spannungsteiler (100k/22k) montieren, dann in der UI auf „Geregelt“ umstellen.

### Zusatz (gleiche Session) — OTA-Rollback-Schutz
- **Problem:** Ohne Rollback war `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` deaktiviert → eine
  technisch valide, aber abstürzende Firmware hätte keinen automatischen Rückfall gehabt.
- **Lösung:** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` in `sdkconfig.defaults` + in `main.c`
  `esp_ota_mark_app_valid_cancel_rollback()` ganz am Anfang von `app_main` (VOR dem
  Deep-Sleep-Check, sonst würde eine neue Firmware nach dem ersten Wakeup fälschlich
  zurückgerollt).
- **Verhalten:** Korrupte Firmware → Bootloader startet sie nicht (Validierung).
  Abstürzende Firmware → Rollback auf die letzte stabile Partition beim nächsten Boot.
  Hänger ohne Reset → nicht erkannt (Watchdogs für Kamera/Stream bewusst deaktiviert).
- **Hürden beim Build:** Nach sdkconfig-Änderung musste die `sdkconfig` gelöscht werden
  (wird sonst nicht aus defaults neu erzeugt). Danach wiederholte **Windows-Dateilocks**
  (`GetLastError()=32`, Windows Defender Echtzeitscanner) – Lösung: Build in Schleife
  wiederholen (`do { ninja } while ($LASTEXITCODE -ne 0)`), jeder Versuch macht Fortschritt.

---

## Zusatz (gleiche Session) — CYD-Variante: Repo-Umbau auf Zwei-Projekt-Struktur

### Entscheidung: Zwei Unterprojekte statt Branches
- **Ziel:** Zweite Firmware-Variante für den **CYD** (Cheap Yellow Display, ESP32-2432S028R)
  als Display-Client (zeigt das Kamerabild vom ESP32-CAM, Touch-Steuerung).
- **Entschieden:** **Ein Repo, zwei eigenständige ESP-IDF-Projekte** (`esp32cam/` + `cyd/`),
  **keine Branches** (Branches mischen sdkconfig/build/version.h → jedes Umschalten =
  Komplett-Neubau + Verwechslungsgefahr beim abwechselnden Flashen am selben USB-Port).
- **Neue Struktur:**
  - `esp32cam/` – bisheriges Projekt verschoben (Kamera, OTA, PSRAM). Build OK, v0.1.77.
  - `cyd/` – neues Display-Client-Projekt. Build OK, v0.1.81.
  - `components/nvs_config/` – gemeinsame NVS-Komponente (beide Projekte).
  - `tools/increment_build.py` – jetzt mit `--project-dir`; Build-Zähler bleibt **global**
    (Repo-Root `.build_number`) → gemeinsame v0.1.xx-Nummerierung über alle Geräte.

### CYD-Hardware (klassisch ESP32-2432S028R, WROOM-32)
- **Display:** ILI9341 240x320 SPI (SCK18/MOSI23/CS5/DC2/RST4/BL21 active-high).
- **Touch:** XPT2046 (CS14, gleicher SPI-Bus).
- **Kein PSRAM** → JPEG-Dekodierung mit Skalierung 1:4 (160x120, 38 KB) + 2x-Upscaling
  + 90°-Rotation beim Blit → füllt 240x320 komplett.

### Wichtige Hürden / Erkenntnisse
- **ESP-IDF 6.x hat KEINEN ILI9341-Treiber mehr** in `esp_lcd` (nur ST7789/SSD1306) →
  eigener Minimal-Treiber über `esp_lcd_panel_io_spi` (`tx_param`/`tx_color`), inkl.
  Standard-Init-Sequenz. Farbdaten RGB565 **Big-Endian** (`be16()`); `esp_jpeg` mit
  `swap_color_bytes=1` liefert konsistent Big-Endian.
- **5x7-Font ist spaltenorientiert** (5 Bytes = 5 Spalten, Bits = Zeilen) – mein erster
  Code las zeilenorientiert → Out-of-Bounds (GCC `-Werror=aggressive-loop-optimizations`).
  Korrekt: `(font5x7[ch][c] >> ry) & 1`.
- **`nvs_config_get_str(key, NULL)` crasht** (macht `strdup(NULL)`) → immer `""` als
  Default übergeben + mit `strlen` absichern.
- **`${Python3_EXECUTABLE}` ist im Configure leer** (nur `_Python3_EXECUTABLE`/`PYTHON`
  im Cache) → auf **`${PYTHON}`** umgestellt.
- **Erstes Configure scheitert**, wenn `include/version.h` noch nicht existiert
  (`target_sources` verlangt die Datei) → einmal `increment_build.py --project-dir cyd`
  manuell ausführen, dann `idf.py build`.
- **Versions-Sync** (version.h vs. Binary) benötigt wie beim CAM ggf. einen zweiten Build.

### Nächster Schritt (Nutzer)
- CYD an USB stecken, flashen: `cd cyd; idf.py -p COM4 flash`.
- ESP32-CAM + CYD einschalten; CYD sollte sich mit `Cam-AP` verbinden und das Bild zeigen.
- **Hardware-Validierung nötig:** Display-Init (Farben ggf. MADCTL 0x08↔0x00), Backlight
  (active-high), Touch-Kalibrierung (TOUCH_MIN/MAX_X/Y, Z1/Z2-Schwellen), Rotation
  (CW/CCW).
