# Autofrontcam — Rechte-Seiten-Kamera für den Škoda (ESP32-CAM + OV2640 + CYD-Display)

Eine **WiFi-Seitenkamera** für ein Škoda-Fahrzeug auf Basis des **AI-Thinker ESP32-CAM**
(ESP32-D0WD-V3 + OV2640). Die Kamera liefert **Einzelbilder (JPEG, `/capture`)** über ein
eigenes WLAN (Access Point) an einen **Display-Client im Fahrzeuginnenraum** und unterstützt
**OTA-Firmware-Updates** über einen eingebetteten Webserver.

Zusätzlich gibt es eine **CYD-Variante** (Cheap Yellow Display, ESP32-2432S028R): ein
eigenständiger **Display-Client**, der sich per WLAN mit dem Kamera-SoftAP verbindet, das
Kamerabild auf dem ILI9341-Display (**320×240 Landscape**) anzeigt und per **Touch-Menü**
Helligkeit/Rotation/Kalibrierung steuert.

> **Zwei Geräte = zwei eigenständige ESP-IDF-Projekte im selben Repo** (`esp32cam/` + `cyd/`),
> keine Branches. Details siehe unten unter „Zwei Geräte – Projektaufbau“.
>
> **Aktueller Stand des Streams:** Die Kamera liefert **keinen echten MJPEG-Stream** mehr.
> Es wird ein einzelnes JPEG pro HTTP-Request (`GET /capture`) auf **Port 80** geliefert; der
> CYD holt Frames in einer Poll-Schleife ab. (Der frühere `MJPEG_PORT 81`-Verweis ist ein
> veralteter Kommentar, kein aktiver Server.)

## Anwendungsfall

- **Montageort:** Rechte Frontseite des Škoda, vor dem Seitenspiegel, unten auf
  Reifenhöhe am Stoßfänger.
- **Zweck:** Sicht auf die **rechte Fahrzeugseite** — zur Überwachung beim Einparken,
  Rangieren und beim Beobachten des toten Winkels entlang der Fahrzeugflanke.
- **Bedienung:** Der **CYD-Display-Client** im Innenraum verbindet sich mit dem Access Point
  und zeigt das Kamerabild dauerhaft an. (Alternativ war ursprünglich ein iPhone 6 per
  Captive Portal auf der Web-UI geplant — der CYD hat diese Rolle übernommen.)

## Stromversorgung & Batterieüberwachung

- Das Modul wird über einen **Spannungswandler** an die **Fahrzeugbatterie im Motorraum**
  angeschlossen (Bordnetz 12V → 5V für das Board).
- Die **Bordspannung wird vom ESP überwacht** (Messpin am ADC des ESP32).
- **Fahrzeug abgestellt** (Spannung fällt unter den Grenzwert): Der ESP schaltet WiFi
  ab und geht in den **bestmöglichen Sleep-Modus**. Im Sleep überwacht er ausschließlich
  den Spannungs-Messpin.
- **Fahrzeug gestartet** (Spannung steigt über den Grenzwert): Der ESP **erwacht aus dem
  Sleep** und startet Access Point, Kamera und Stream neu.
- **Konfigurierbarer Grenzwert** (Werkseinstellung 12,8V), einstellbar über die Web-UI.
- **Betriebsmodus per Radiobutton** in der Web-UI: **„Dauerbetrieb“** (default) oder
  **„Geregelt“** (Sleep/Wake-up über Spannung).
- **Default = Dauerbetrieb:** Der ESP läuft **immer aktiv** — wichtig, solange der
  Spannungsteiler noch nicht montiert ist (sonst wären die Messwerte am offenen Pin
  sinnlos und der ESP würde ungewollt in den Sleep fallen). Erst nach Montage des
  Teilers wird in der UI auf **„Geregelt“** umgestellt.

## Features

**Kamera (`esp32cam/`):**
- **Einzelbild-Abruf (`GET /capture`)** mit maximaler Bildrate (Frame so aktuell wie möglich)
- **Vollbild-Web-UI** für das Smartphone (iPhone 6), optimierte Bilddarstellung (Standard-/Fallback-Anzeige)
- **Kalibrierungslinie (rot):** verschiebbare Fahrzeugkante — vertikal über das ganze
  Bild, links/rechts verschiebbar, Winkel einstellbar, Dicke einstellbar
- **Zweite Kalibrierungslinie (gelb):** optional, sofern der RAM reicht
- **Spannungsanzeige** der Bordspannung im Bild + einstellbarer Schwellwert
  + **Betriebsmodus** „Geregelt“/„Dauerbetrieb“ (Radiobutton, default Dauerbetrieb)
- **Access Point + Captive Portal** (zur Einrichtung / für die Web-UI)
- **OTA-Update-Button** (links unten im Bild überlappend) – Firmware wird direkt vom
  iPhone hochgeladen (Datei aus der Files-App, kein Ausbau/Internet nötig)
- **Sleep-/Wake-up-Steuerung** über die Fahrzeugspannung
- **NVS-Konfigurationsspeicher** (WiFi, Spannungsgrenzwert, Linieneinstellungen)
- **Stack- und Heap-Monitoring**
- **ESP-IDF 6.x, FreeRTOS (Dual-Core)**

**Display-Client (`cyd/`, CYD):**
- **Kamerabild-Liveanzeige** (JPEG-Abruf + Dekodierung + `display_blit` auf ILI9341)
- **OSD-Leiste oben** (Version, fps, Status — statt dauerhafter Buttons)
- **Touch-Menü** (per Tippen ein-/ausblendbar): **BRI+/BRI−/ROT** (Helligkeit sendet per
  HTTP-POST an die CAM-Config-API, ROT dreht die Anzeige) und **KALIB/DIAG/ZU**
- **Touch-Kalibrierungsmodus** (loggt XPT2046-Rohwerte der 4 Ecken + Mitte)
- **Panel-Geometrie-Diagnose** (4-Quadranten-Test für das „1/4-Screen“-Problem)
- **Robuster Stream-Loop** (Overlay/OSD-Timer unabhängig von Frames, kein Einfrieren)

## System-Übersicht

```mermaid
flowchart LR
    BAT[Fahrzeugbatterie 12V<br/>Motorraum] -->|Wandler| ESP[ESP32-CAM<br/>OV2640 + PSRAM]
    BAT -->|Messpin ADC| ESP
    ESP -->|AP 2.4GHz "Cam-AP"<br/>10.1.1.1| CYD[CYD Display-Client<br/>ESP32-2432S028R]
    CYD -->|"GET /capture (JPEG)"| ESP
    CYD -->|"POST /api/config (BRI)"| ESP
```

## Sleep-/Wake-up-Logik

| Zustand | Bedingung | Verhalten |
|---|---|---|
| **Aktiv** | Spannung ≥ Grenzwert (z.B. 12,8V) | Kamera + Stream + AP laufen |
| **Sleep** | Spannung < Grenzwert | WiFi ab, Deep-/Light-Sleep, nur Spannungs-Messpin aktiv |
| **Wake-up** | Spannung > Grenzwert | Aufwachen, System komplett neu starten |
| **Dauerbetrieb** | Modus „Dauerbetrieb“ (default) | Immer aktiv, kein Sleep |

*(Konkrete Realisierung siehe Abschnitt „Design-Entscheidungen“ unten.)*

## Design-Entscheidungen (verbindliche Zielvorgaben)

> Diese Entscheidungen wurden mit dem Auftraggeber abgestimmt und sind verbindlich
> für die Implementierung. Sie dienen als „Single Source of Truth“ für das Zielbild.

### 1. Spannungsmessung
- **Messpin:** `GPIO35` (ADC1_CH7, nur Eingang — kollidiert nicht mit Kamera/PSRAM/SD).
- **Wandler (max. 14,5V):** Spannungsteiler **100kΩ (oben) / 22kΩ (unten)** →
  Teilerverhältnis 0,1803, Umrechnungsfaktor **≈5,55** (Batterie→ADC).
  Bei 14,5V ≈ 2,62V am ADC (unter der 3,1V-Grenze, 11dB), bei 12,8V ≈ 2,31V.
  Laststrom ≈0,12mA. (Nicht 30kΩ verwenden → würde bei 14,5V clippen.)
- **Messung:** Mittelwertfilter (≈16 Samples), Kalibrierungsfaktor in NVS.
- **Betriebsmodus per Radiobutton** in der Web-UI: **„Geregelt“** oder **„Dauerbetrieb“**
  (kein „0V-Trick“ am Grenzwert).
- **Default = „Dauerbetrieb“:** ESP läuft immer aktiv — gilt, bis der Spannungsteiler
  montiert ist. Erst dann auf „Geregelt“ umstellen.

### 2. Sleep-/Wake-up-Steuerung
- Nur im Modus **„Geregelt“** aktiv. Im Modus „Dauerbetrieb“ findet kein Sleep statt.
- Spannung < Grenzwert (Voreinstellung **12,8V**): WiFi + Kamera aus, **bestmöglicher Sleep**,
  nur der Messpin bleibt aktiv.
- Spannung > Grenzwert: **Aufwachen** und System neu starten (AP + Kamera + Stream).
- Da der ESP32 kein echtes ADC-Wakeup hat: Wake-up über periodisches ADC-Pollen
  im Light-Sleep (Designentscheidung, kann bei Bedarf angepasst werden).

### 3. OTA (Upload vom iPhone, „nur ein Button“)
- Der **OTA-Button** (links unten) öffnet den **Datei-Picker der Files-App**.
- Die Firmware-Datei wird per **Upload** vom iPhone direkt in die freie OTA-Partition
  geschrieben (klassischer Web-OTA, `POST /update`, raw `application/octet-stream`).
- **Wie kommt die `.bin` aufs iPhone?** Per AirDrop, iCloud/Dropbox oder Mail – danach
  steht sie in der Files-App und kann beim Update ausgewählt werden.
- Kein Ausbau des Boards, kein USB, kein Internet nötig – nur AP + iPhone.
- Im Browser wird der Upload-Fortschritt am OTA-Button angezeigt.

**Rollback-Schutz (automatischer Rückfall):**
- Aktiviert über `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`.
- Eine neue Firmware startet als **„PENDING_VERIFY“** (noch nicht bestätigt).
- In `app_main` wird nach erfolgreichem Boot `esp_ota_mark_app_valid_cancel_rollback()`
  aufgerufen (vor dem Deep-Sleep-Check!) – erst dann gilt das Update als gültig.
- **Konsequenz:**
  - *Korrupte/unvollständige Firmware* → Bootloader-Validierung schlägt fehl → startet die andere Partition.
  - *Abstürzende Firmware* (Panic vor der Bestätigung) → beim nächsten Boot rollt der
    Bootloader eigenständig auf die letzte stabile Partition zurück.
  - Hinweis: Ein logischer „Hänger“ ohne Reset wird nicht erkannt (Watchdogs bewusst
    deaktiviert für Kamera/Stream).

**OTA-Ablauf:**
```
idf.py build  →  build/autofrontcam.bin
      ↓ (AirDrop/iCloud/Mail)
autofrontcam.bin in der Files-App des iPhones
      ↓
Web-UI: OTA-Button → Datei auswählen → Upload → Neustart
```

### 4. Web-UI (iPhone 6, Touch — Einrichtungs-/Fallback-Weg)
> **Hinweis:** Primärer Anzeigeweg ist heute der **CYD-Display-Client** (siehe Abschnitt
> „Zwei Geräte – Projektaufbau“). Die Web-UI existiert weiterhin auf Port 80 und dient der
> Einrichtung, für Kalibrierungslinien, OTA und Spannungs-/Modus-Konfiguration.
- **Vollbild-Bild** (aus `/capture`), bestmöglich ausgenutzt.
- **Immer sichtbar:** Bild, Kalibrierungslinien (Overlay), Spannungsanzeige.
- **Touch-Menü (nur nach Bildschirm-Berührung):** Linien-Buttons, Dicke, OTA-Button,
  Spannungsgrenzwert + **Betriebsmodus (Radiobutton „Geregelt“/„Dauerbetrieb“)**.
  Ausblenden durch Tipp auf freie Stelle.
- **OTA-Button:** links unten, überlappend auf dem Bild.
- **Spannungsanzeige:** rechts oben (z.B. `12,9V`); Grenzwert-Einstellung nur im
  Modus „Geregelt“ relevant.

### 5. Kalibrierungslinien (Overlay)
- **Canvas-Overlay** über dem Einzelbild (`/capture`, per Poll-Loop in der Web-UI), Linien werden darauf gezeichnet.
- **Rote Linie** = Fahrzeugkante: immer vertikal über das ganze Bild, X-Position
  (links/rechts), **Winkel** um den Mittelpunkt, **Dicke** — alles einstellbar.
- **Gelbe Linie:** zweite Linie, aktivierbar, sofern der RAM reicht.
- **Bedienung = Drag + Buttons:**
  - *Drag auf der Linie* → X-Position verschieben; *Griff oben/unten* → Winkel drehen.
  - *4 Buttons* (links eingeblendet): `←` `→` (fein verschieben) + `↺` `↻` (fein drehen).
  - Drag nur aktiv bei geöffnetem Menü (normales Tippen verschiebt nichts).
- **Persistenz:** X, Winkel, Dicke, aktive Linien in **NVS** (überleben Sleep/Neustart).

### 6. Stream / Bildabruf
- **Einzelbild pro Request (`GET /capture`)** statt Endlos-MJPEG — blockiert den httpd nicht
  und ist über den langsamen SoftAP zuverlässiger.
- **Maximale Bildrate** — die Szene soll so aktuell wie möglich sein
  (Auflösung QVGA, JPEG-Qualität 16, GRAB_LATEST, kleiner Frame = schnellerer SoftAP-Transfer).

## Zugriff

- **AP-Modus:** SSID `Cam-AP`, IP `10.1.1.1` → Captive Portal (zur Einrichtung / Web-UI)
- **Web-UI:** `http://10.1.1.1/` (Port 80)
- **Einzelbild:** `http://10.1.1.1:80/capture`
- **Config-API:** `http://10.1.1.1:80/api/config` (GET = JSON, POST = Form)
- **OTA:** `http://<ip>:80/ota`
- **CYD:** STA am `Cam-AP`, feste IP `10.1.1.2` (kein DHCP)

> **Port-Hinweis:** `/capture` und die Web-UI liegen auf **Port 80** (ein gemeinsamer
> `esp_http_server`). `MJPEG_PORT 81` in `config.h` ist **nur noch ein Kommentar** —
> es gibt keinen separaten MJPEG-Server.

## Board: AI-Thinker ESP32-CAM

| Komponente | Spezifikation |
|---|---|
| **Chip** | ESP32-D0WD-V3 (Dual-Core Xtensa LX6, 240MHz) |
| **Kamera** | OV2640 (SCCB/I2C, DVP parallel) |
| **Flash** | 4MB (aus Chip ausgelesen) |
| **PSRAM** | 4MB (SPI-RAM, wichtig für JPEG-Puffer) |
| **Status-LED** | GPIO33 (active-low) |
| **Flash-LED** | GPIO4 |
| **Port** | COM4 (USB-Serial CH340) |
| **Spannungsmessung** | ADC an einem frei konfigurierbaren GPIO (Spannungsteiler 12V→3,3V) |

## Schnellstart

Beide Geräte sind **eigenständige ESP-IDF-Projekte** im Repo. Vor jedem Kommando in das
jeweilige Projektverzeichnis wechseln — so ist immer klar, **welches Gerät am USB-Port
angesteckt sein muss**:

| Projekt | Gerät | Verzeichnis | Firmware |
|---|---|---|---|
| `esp32cam` | AI-Thinker ESP32-CAM (COM4) | `esp32cam/` | `autofrontcam.bin` |
| `cyd` | CYD ESP32-2432S028R | `cyd/` | `autofrontcam_cyd.bin` |

```powershell
. ..\lora\activate-esp-idf.ps1          # ESP-IDF Umgebung aktivieren (einmal pro Terminal)
cd esp32cam                             # ODER: cd cyd
idf.py build
```

### Flash
> **Achtung:** Es muss genau das Gerät am USB-Port hängen, das du flashen willst!
> ESP32-CAM ↔ CYD **nicht** verwechseln — die Firmware ist nicht kompatibel.

```powershell
cd esp32cam; idf.py -p COM4 flash      # ESP32-CAM einstecken
cd cyd;     idf.py -p COM4 flash      # CYD einstecken
```

### Monitor
```powershell
cd esp32cam; idf.py -p COM4 monitor    # ESP32-CAM
cd cyd;     idf.py -p COM4 monitor    # CYD
```

## Projektstruktur

```
autofrontcam/
├── esp32cam/                  ESP-IDF-Projekt 1: Kamera (AI-Thinker ESP32-CAM)
│   ├── CMakeLists.txt         Projekt-Root (verweist auf ../components)
│   ├── sdkconfig.defaults     Board-Konfiguration (4MB, PSRAM, OTA)
│   ├── partitions.csv         OTA-Partitionen (4MB)
│   ├── main/                  Quellcode des Kameramoduls
│   │   ├── main.c             Hauptprogramm (Stream-Server, Web-UI, Config-API)
│   │   ├── index.html         Eingebettete Touch-Web-UI
│   │   ├── camera.c           OV2640 Kameratreiber
│   │   ├── wifi.c             WiFi-Manager (AP + Captive Portal)
│   │   ├── ota.c              OTA-Webserver (Upload)
│   │   ├── voltage.c          Spannungsmessung (ADC GPIO35)
│   │   ├── sleep.c            Sleep-/Wake-up-Steuerung
│   │   ├── lines.c            Kalibrierungslinien (NVS-Persistenz)
│   │   └── ...                (stack_monitor.c, heap_monitor.c, dns_server.c)
│   └── include/               Header des Kameramoduls (config.h, camera.h, ...)
├── cyd/                       ESP-IDF-Projekt 2: Display-Client (CYD)
│   ├── CMakeLists.txt         Projekt-Root (verweist auf ../components)
│   ├── sdkconfig.defaults     Board-Konfiguration (4MB, kein PSRAM)
│   ├── partitions.csv         Einfaches Layout ohne OTA
│   ├── main/
│   │   ├── main.c             Einstieg (NVS, Display, Touch, Tasks, Log-Level)
│   │   ├── display.c          ILI9341-Treiber (320x240 Landscape, roher SPI + Mutex) + 5x7-Font
│   │   ├── stream.c           WiFi-STA + JPEG-Abruf (/capture) + Dekodierung + Anzeige
│   │   ├── touch.c            XPT2046-Touch (SPI3, Druckschwelle 300, Kalibrier-Modus)
│   │   └── ui.c               OSD + Touch-Menü (BRI/ROT/KALIB/DIAG/ZU -> CAM-API)
│   └── include/               Header des CYD (config.h, version.h.in)
├── components/
│   └── nvs_config/            GEMEINSAME Komponente (NVS-Helfer, beide Projekte)
├── tools/
│   └── increment_build.py     Build-Nummer erhöhen (--project-dir)
└── README.md
```

**Wichtig:** `esp32cam` und `cyd` sind getrennte Builds mit getrennten `build/`-Ordnern
und getrennten `sdkconfig`-Dateien. Gemeinsamer Code liegt einmal in `components/nvs_config/`.

## Zwei Geräte – Projektaufbau

**Warum zwei Unterprojekte und keine Branches?**
- Die beiden Geräte sind hardwaremäßig völlig verschieden: ESP32-CAM (Kamera-Treiber,
  PSRAM, OTA) vs. CYD (Display-Treiber, Touch, WiFi-Client, kein PSRAM). Sie brauchen
  unterschiedliche `sdkconfig`, unterschiedliche Partitionstabellen und eigene Builds.
- **Branches** würden `sdkconfig`, `build/`-Artefakte und das generierte `version.h`
  vermischen → bei jedem Wechsel müsste komplett neu gebaut werden, und beim abwechselnden
  Flashen an einem USB-Port bestünde die Gefahr, die falsche Firmware zu flashen.
- **Zwei Unterprojekte im selben Repo** sind die sauberste Lösung: getrennte `build/`,
  getrennte `sdkconfig`, getrennte Flash-Befehle, gemeinsamer Code in `components/`.

**Netzwerk-Topologie (CYD als Anzeige):**
- Der ESP32-CAM eröffnet den SoftAP `Cam-AP` (offen, IP `10.1.1.1`, max. **1** Client).
- Der CYD verbindet sich als WiFi-Station mit `Cam-AP` (feste IP `10.1.1.2`) und holt
  JPEG-Frames per `GET http://10.1.1.1:80/capture` in einer Poll-Schleife ab
  (`STREAM_POLL_MS 50` → ~20 fps, de facto durch CAM/SoftAP begrenzt).
- **Hinweis:** Durch `max_connection = 1` am CAM ist immer nur **ein** Gerät gleichzeitig
  verbunden (entweder das iPhone ODER der CYD). iPhone und CYD gleichzeitig wären erst
  möglich, wenn am CAM die Client-Anzahl erhöht würde.
- **Owner-Logik am CAM:** Nur **ein** Client wird bedient. Der erste Client (per
  Quell-IP) behält den Stream; andere bekommen **503**. Nach **10 s** ohne Anfrage
  von der aktiven IP kann ein anderes Gerät übernehmen (`STREAM_OWNER_TIMEOUT_MS`).

### CYD — Hardware-Erkenntnisse (verifiziert)

> Diese Erkenntnisse wurden am echten Board verifiziert und sind verbindlich.

| Thema | Erkenntnis |
|---|---|
| **Panel** | Das CYD-Panel ist ein **320×240-Landscape-Controller** (ST7796-artig), **NICHT** 240×320. |
| **MADCTL** | `0x40` (MX=1, **kein MV**). Mit MV (`0x60`) wird das Bild gekippt/gesplittet (untere Hälfte oben). `0x40` + `TFT_WIDTH=320` füllt das ganze Panel (behebt das „1/4-Screen“-Problem). |
| **Display-Pins** | CLK=14, MOSI=13, MISO=12, CS=15, DC=2, RST=-1 (hängt am ESP32-Reset), BL=21 (active-high). |
| **Display-Treiber** | **Roher SPI-Treiber** (`spi_device_transmit`, DC manuell, SPI2_HOST) statt `esp_lcd_panel_io` — die esp_lcd-Pixel-Schreibpfade panikten am CYD. Ein **SPI-Mutex** (`s_lcd_mutex`) schützt parallelen Zugriff (UI + Stream) vor SPI-Asserts. |
| **Touch-IC** | **XPT2046** (resistiv) auf einem **separaten SPI-Bus `SPI3`** (SCLK=25, MOSI=32, MISO=39, CS=33, IRQ=36). Der frühere Versuch mit FT6236/I2C auf GPIO 6 crashte — **GPIO 6 ist beim ESP32-WROOM ein Flash-SPI-Pin** und als I2C nicht nutzbar. |
| **Touch-Pins** | SCLK=25, MOSI=32, MISO=39, CS=33, IRQ=36 (2,5 MHz, kein DMA). |
| **Druckschwelle** | **300** (Rauschen 0–50, echter Stiftdruck ~2000). Verhindert Geister-Touches ohne Berührung. |
| **Touch-Kalibrierung** | Basis-Umrechnung `dx = x_raw*320/4096`, `dy = y_raw*240/4096` ist **korrekt** (4 Eckpunkte + Mitte verifiziert: keine Achsen-Vertauschung, keine Invertierung). Die Kalibrierung wurde **mit dem Fingernagel** erfolgreich durchgeführt (Druck deutlich über der 300er-Schwelle); weiche Fingerkuppe liefert weniger Druck und kann unzuverlässig sein. |
| **Kein PSRAM** | Klassischer CYD (ESP32-2432S028R) hat **kein PSRAM** → JPEG-Dekodierung läuft adaptiv (1:2 solange Puffer reicht, sonst 1:4), `MAX_DECODED_BUF 60000`. |
| **Anzeige-Rotation** | Kamerabild ist quer, Display quer (320×240) → `DISPLAY_ROTATION 0`. ROT-Button im Menü togglet CW/CCW. |

### CYD — Bedienung

- **Tippen** auf das Video öffnet das **Touch-Menü** (temporär, über dem Video — kein
  fester Button-Bereich, dadurch bleibt das Bild fast vollflächig).
- **Menü-Buttons** (2 Reihen unten):
  - Reihe 1: **BRI+** / **BRI−** (Helligkeit, POST an CAM-`/api/config`) · **ROT** (Anzeige drehen)
  - Reihe 2: **KALIB** (Touch-Rohwerte loggen) · **DIAG** (Panel-Geometrie-Test) · **ZU** (Menü schließen)
- **OSD oben:** Version (links), fps (rechts), Status darunter (z.B. `T:x,y` bei Touch-Diagnose).
- **DIAG:** zeigt den 4-Quadranten-Geometrie-Test (Rot/Grün/Blau/Gelb + Eckmarker 1–4);
  Tippen beendet den Test und öffnet das Menü wieder.

### CYD — Tasten-/Kalibrierungs-Workflow

1. **Kalibrierung (KALIB):** Menü öffnen → **KALIB** → die 4 Ecken +
   die Mitte drücken (je ~1 s; **Fingernagel oder Stift**, weiche Fingerkuppe reicht evtl.
   nicht für die Druckschwelle). Die Rohwerte erscheinen im seriellen Monitor als
   `touch: KALIB: x_raw=... y_raw=...` und die umgerechneten Koordinaten im OSD als `T:x,y`.
2. **Auswertung:** Die Punktwolke zeigt, ob Achsen getauscht/invertiert werden müssen —
   aktuell ist die Basis-Umrechnung bereits korrekt (verifiziert), es ist keine Matrix nötig.
3. **Diagnose (DIAG):** Falls das Bild „1/4 fehlt“ oder die Schrift kippt → Geome­trie-Test
   starten und die Panel-Orientierung (MADCTL/`TFT_WIDTH`) prüfen (siehe Hardware-Tabelle oben).

### CAM — Stream-Architektur & bekannte Punkte

- Die Kamera liefert **kein MJPEG** mehr, sondern ein einzelnes JPEG pro `GET /capture`
  (Port 80, ein gemeinsamer `esp_http_server` mit `lru_purge_enable`, `send_wait_timeout=5`).
- Der Frame wird unter dem Kamera-Mutex erfasst, **kopiert** (PSRAM) und der Mutex sofort
  wieder freigegeben — langsames Senden blockiert die Kamera nicht.
- **Kein Keep-Alive** vom CYD: Der CAM-httpd resettet bei Keep-Alive sonst die Verbindung
  (`Connection reset by peer`, fps-Einbruch) → der CYD baut pro Frame eine neue Verbindung auf.

### CAM — Selbstheilung (Self-Healing-Watchdog)

> **Problem (behoben):** Der CYD baut pro Frame eine **neue TCP-Verbindung** auf (kein
> Keep-Alive). Jede geschlossene Verbindung blieb als TIME_WAIT-PCB (`2*MSL`) bestehen.
> Mit nur 16 aktiven TCP-PCBs und `MSL=60000` (60 s) waren nach kurzer Zeit **alle PCBs
> belegt** → der CAM-Server nahm keine neuen Verbindungen mehr an → der CYD meldete
> `Failed to open a new connection` / `select() timeout` (fps 3, kein Bild).
> Da die CAM **verbaut** ist und nicht manuell resettet werden kann, heilt sie sich jetzt
> selbst.

**Umgesetzte Härtung (Firmware):**
- **LWIP:** `CONFIG_LWIP_MAX_ACTIVE_TCP=48` (statt 16), `CONFIG_LWIP_TCP_MSL=2000`
  (TIME_WAIT nur 4 s statt 120 s), `CONFIG_LWIP_MAX_SOCKETS=16` → tote PCBs blockieren
  den Server nicht mehr.
- **Self-Healing-Watchdog** (`conn_watchdog_task` in `main.c`): alle 10 s ein lokaler
  Test-Connect auf Port 80. Eskalation bei Fehlschlägen:
  1. **2 Fehlschläge:** HTTP-Server neu starten (`httpd_stop` + `httpd_start` → resettet
     alle offenen/halboffenen Verbindungen und PCBs)
  2. **4 Fehlschläge:** WiFi/AP neu starten (`esp_wifi_stop` + `esp_wifi_start`)
  3. **8 Fehlschläge:** kompletter Software-Reset (`esp_restart`)
- Damit arbeitet die CAM **eigenständig** (ohne manuellen Reset) und hat nach einem
  Selbstheilungs-Vorgang sofort wieder einen Client.

## Konfiguration

Alle wichtigen Parameter in `include/config.h` (jeweils pro Projekt):

**`esp32cam/include/config.h`:**
- **Kamera-Pins (OV2640)** und Auflösung (**QVGA 320×240**, JPEG, Qualität 16 — kleine Frames
  für hohe Bildrate über den SoftAP)
- **LED-Pins** (Status + Blitz)
- **WiFi-AP-SSID, AP-IP, Timeouts** (`Cam-AP`, `10.1.1.1`, `max_connection = 1`)
- **Server-Port** (`STREAM_PORT 80`; `MJPEG_PORT 81` ist veraltet/ungültig)
- **Spannungsmesspin + Spannungsteiler-Faktor**
- **Sleep-Grenzwert (Voreinstellung 12,8V)**
- **Task-Stacks und Prioritaeten**

**`cyd/include/config.h`:**
- **Display-Pins** (SPI2: CLK=14, MOSI=13, CS=15, DC=2, BL=21) + `TFT_WIDTH=320`, `TFT_HEIGHT=240`
- **MADCTL** (`ILI9341_MADCTL 0x40`, kein MV)
- **Touch-Pins** (SPI3: SCLK=25, MOSI=32, MISO=39, CS=33, IRQ=36) + Druckschwelle (in `touch.c`)
- **WiFi/statische IP** (`Cam-AP`, `10.1.1.2`)
- **Stream-Parameter** (`STREAM_POLL_MS 50`, `CAM_PORT_DEFAULT 80`, `/capture`, Puffergrößen)
- **UI-Layout** (`UI_OSD_H 20`, kein fester Button-Bereich)

## Implementierungsstatus

**Kamera (`esp32cam/`):**

| Feature | Status |
|---|---|
| Einzelbild-Abruf `/capture` (max. Bildrate, GRAB_LATEST, Port 80) | ✅ implementiert |
| Web-UI (Vollbild, Touch-Menü) | ✅ implementiert |
| Access Point + Captive Portal | ✅ implementiert |
| OTA (Upload vom iPhone, Button links unten) | ✅ implementiert |
| mDNS | ✅ implementiert |
| Spannungsmessung (ADC GPIO35, Teiler 5,55) | ✅ implementiert |
| Sleep-/Wake-up-Steuerung (Deep-Sleep, Timer-Wakeup) | ✅ implementiert |
| Kalibrierungslinien (rot + gelb, Drag + Buttons) | ✅ implementiert |
| Spannungsanzeige + Modus/Grenzwert (Radiobutton) | ✅ implementiert |
| NVS-Persistenz (Linien, Modus, Grenzwert) | ✅ implementiert |
| MJPEG-Stream (Port 81) | ❌ entfernt (nur `/capture`) |
| LWIP-Härtung (TCP-PCBs, kurze TIME_WAIT) | ✅ implementiert |
| Self-Healing-Watchdog (Server/WiFi/Reset) | ✅ implementiert |

**Display-Client (`cyd/`):**

| Feature | Status |
|---|---|
| ILI9341-Anzeige (320×240 Landscape, roher SPI + Mutex) | ✅ implementiert |
| Stream-Abruf + JPEG-Dekodierung + Anzeige | ✅ implementiert |
| OSD-Leiste (Version/fps/Status) | ✅ implementiert |
| Touch-Menü (BRI+/BRI−/ROT/KALIB/DIAG/ZU) | ✅ implementiert |
| Touch XPT2046 (SPI3, Druckschwelle 300) | ✅ implementiert |
| Touch-Kalibrierung (Rohwerte, Basis-Matrix verifiziert) | ✅ verifiziert |
| Panel-Geometrie-Diagnose (DIAG) | ✅ implementiert |
| Overlay/OSD unabhängig von Frames (kein Einfrieren) | ✅ implementiert |
| HTTP-Log-Spam-Reduktion (CAM nicht erreichbar) | ✅ implementiert |

## Erkenntnisse & gelöste Probleme (Debug-Log)

Die wichtigsten, am realen Board verifizierten Erkenntnisse (Ausführliches siehe
„Zwei Geräte – Projektaufbau → CYD — Hardware-Erkenntnisse“):

| Problem | Ursache | Lösung |
|---|---|---|
| **Reset-Loop am CYD** beim Display-Zugriff | `esp_lcd_panel_io` panikte bei Pixel-Write | **Roher SPI-Treiber** (`spi_device_transmit`, DC manuell) |
| **SPI-Assert** bei parallelem UI+Stream-Zugriff | gleichzeitige SPI-Nutzung ohne Schutz | **SPI-Mutex** um `lcd_write` |
| **Touch-Init crashte** | FT6236/I2C auf GPIO 6 — GPIO 6 ist Flash-SPI-Pin | **XPT2046 auf SPI3** (25/32/33/39) |
| **Nur 3/4 des Screens beschrieben** („1/4 fehlt“) | Panel ist **320×240**, nicht 240×320 | `TFT_WIDTH=320`, `TFT_HEIGHT=240` |
| **Schrift/Stream gekippt oder gesplittet** | MADCTL mit MV (0x60) | **MADCTL 0x40** (kein MV), Display quer |
| **Geister-Touches** ohne Berührung | Druckschwelle 50 zu niedrig (Rauschen) | Schwelle **300**; echter Druck ~2000 |
| **Touch-Koordinaten im Log unlesbar** | HTTP-Fehler-Spam überflutete das Log | Log-Level HTTP auf `ESP_LOG_NONE`; Koordinaten zusätzlich im OSD (`T:x,y`) |
| **OSD/Menü friert ein** bei ausbleibenden Frames | Overlay hing am Frame-Fetch | **Overlay-Timer alle 500 ms unabhängig von Frames** |
| **`Connection reset by peer`/fps-Einbruch** am CYD | CAM-httpd unterstützt kein Keep-Alive | **kein Keep-Alive**, Verbindung pro Frame neu |
| **CAM nimmt keine Verbindungen an** (fps 3, `select() timeout`) | TIME_WAIT-PCB-Erschöpfung (nur 16 TCP-PCBs, MSL 60s) bei Verbindungs-pro-Frame | **LWIP härten** (48 PCBs, MSL 2s) + **Self-Healing-Watchdog** (Server/WiFi/Reset) |

## Abhängigkeiten

- **`espressif/esp32-camera`** (v2.1.7) — OV2640-Treiber, via Component Registry
- **`espressif/esp_jpeg`** — JPEG-Verarbeitung
- **`esp_hal_clock`** — lokale Komponente, da sie in der verwendeten IDF v6.1-dev-Installation fehlt (aus IDF v6.0 übernommen)

## Versionierung

`MAJOR.MINOR.BUILD` in `include/config.h` (`APP_VERSION_MAJOR`, `APP_VERSION_MINOR`).

Der **Build-Zähler ist global** für das gesamte Repo (`.build_number` im Repo-Root):
Beide Projekte (`esp32cam/` und `cyd/`) teilen sich die Nummernfolge `v0.1.xx`, damit die
Versionsnummer über alle Geräte eindeutig bleibt. `tools/increment_build.py` wird pro
Projekt mit `--project-dir <projekt>` aufgerufen und schreibt die Version in das jeweilige
`include/version.h`.
