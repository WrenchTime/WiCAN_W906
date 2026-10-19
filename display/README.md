# WiCAN-Display

Eigenständige Anzeige für den WiCAN im Sprinter W906: Live-Werte auf frei einteilbaren Seiten, Fehlerspeicher
lesen und löschen, alles ohne Cerbo, MQTT-Broker oder Node-RED. Das Display spricht per WLAN und HTTP direkt
mit dem WiCAN; der bisherige Weg WiCAN → MQTT → Node-RED bleibt daneben bestehen.

Dazu kann es als zweite Quelle Werte des Victron Cerbo GX zeigen – Batterie, Solar, Tanks, Temperaturen –,
auch wenn das Fahrzeug steht und der WiCAN schläft: siehe „Zweite Quelle: der Cerbo GX". Ab Werk ist sie aus,
und ohne sie verhält sich das Display in allem wie vorher.

Hardware: Elecrow CrowPanel 2.1inch-HMI ESP32 Rotary Display (ESP32-S3R8, rundes IPS-Panel 480 × 480,
Drehknopf mit Taster, Touch).

## Stand (2026-10-12)

Die Firmware läuft seit dem 2026-10-09 auf dem Board, seit dem 2026-10-10 im Fahrzeug. Gesehen oder gemessen:

- Start ohne Fehler: Expander antwortet, Touch-Controller meldet sich als CST826 (Kennung 0x11), PSRAM 8 MB,
  WLAN-Treiber startet ohne eigenen NVS-Bereich, „safe mode 0".
- Das Bild steht ruhig, Ring rot, Text richtig; Touch geht; Standby schaltet das Licht nach 60 s ab.
- Knopf: Dieser Encoder liefert zwei Zählschritte je Raste (`KNOB_COUNTS_PER_DETENT`); damit ist eine Raste
  eine Seite.
- Hotspot, Eintragen eines Netzes über die Webseite, Beitritt, echte Werte vom WiCAN.
- Update über die Webseite: hochladen, am Knopf installieren, „Update in Ordnung?" bestätigen.
- Fehlerspeicher lesen vom Display aus (2026-10-10): 35 s, 18 von 18 Steuergeräten, die Liste steht auf dem
  Display.

**Gefunden (2026-10-11): Bei voller Sendeleistung wird das Board schlecht gehört.** Zwei Tage lang fiel die
Strecke immer wieder aus, obwohl das Display den Router gut empfängt: In schlechten Phasen kamen 0 bis 50 % der
Pings an, dann scheiterte schon der Beitritt. Mit mitgelesener Konsole zeigte sich: Der Treiber meldet dabei
keine Trennung, die Info-Seite nennt −65 dBm, und von 60 Pings kommt einer an – der Hinweg steht, der Rückweg
nicht. Eine Versuchs-Firmware hat die Sendeleistung alle zwei Minuten gewechselt (Display unbewegt, zwei Pings
je Sekunde, fünf Durchgänge):

| Sendeleistung | Pings angekommen | je Abschnitt | Laufzeit (Median je Abschnitt) |
|---|---|---|---|
| 20 dBm (Vorgabe des Treibers) | 717 von 893 | 72 / 91 / 100 / 98 / 6 % | 11 bis 38 ms |
| 13 dBm | 1051 von 1051 | 5 × 100 % | 4 bis 6 ms |
| 8,5 dBm | 1058 von 1061 | 99 bis 100 % | 4 bis 5 ms |
| 2 dBm | 1065 von 1067 | 99 bis 100 % | 4 ms |

Bei 20 dBm war die Laufzeit in jedem Durchgang ein Mehrfaches der Nachbarabschnitte, auch wo kein Ping verloren
ging: Die Pakete werden wieder und wieder gesendet. Die Sendeleistung steht jetzt fest auf 8,5 dBm
(`NET_TX_POWER` in `main/net.c`). Damit gemessen am selben Tag: am PC 89 Minuten lang 10.533 von 10.533 Pings,
dann am Platz im Fahrzeug bei −69 dBm (−79 bis −61) zweieinhalb Stunden lang 17.896 von 17.896, Laufzeit im
Median 4 ms, 10.706 Abfragen an den WiCAN ohne einen Fehlschlag. Am Tag davor gingen bei gleichem Empfang und
voller Leistung 10 bis 49 % verloren. Wo zwischen 13 und 20 dBm es kippt, ist nicht gemessen; ebenso wenig, ob
es mit voller Helligkeit hält (gemessen wurde mit dunklem Bildschirm und mit 5 %).

Der Empfangswert auf der Info-Seite sagt über diese Störung nichts: Er misst nur den Hinweg. Was vorher
versucht und für die Ursache gehalten wurde, steht hier, damit niemand es wiederholt:

- „Der Energiesparmodus des WLAN-Treibers ist schuld": Er ist abgeschaltet (das senkt die Antwortzeit von 154
  auf 14 ms), die Verluste blieben.
- „Mit eingeschaltetem Hotspot ist die Strecke gut": In zehn Minuten einer Mitschrift stimmte das (kein
  einziger Fehlschlag gegen zwei bis sieben je Minute davor und danach), in einer späteren Messung nicht.
- „Das Display braucht mindestens −55 dBm": Am besten Platz (−51 dBm) blieben 6 % Verlust, an einem anderen
  Tag gingen bei −55 dBm 20 bis 45 % verloren und bei −67 dBm 1,4 %.
- „Die Leitungen zum Bildschirm stören den eigenen Empfang": Mit der schwächsten Treiberstufe sank der Verlust
  in 70 Minuten von 15,5 % (284 von 1830) auf 3,5 % (62 von 1780), in 15 von 16 Nachbarpaaren. Die Stufe ist
  geblieben (`components/board/board.c`), aber die Ursache war sie nicht: Ein zweiter Versuch, der die
  Leitungen und das Hintergrundlicht zeitweise ganz abschaltete, fand keinen Unterschied mehr.

Ohne Beleg geblieben und trotzdem noch drin: Das Display spricht nur 802.11b/g (`main/net.c`).

Damit ein Aussetzer kein Lesen mehr kostet: Ein Lesen des Fehlerspeichers, das der WiCAN angenommen hat,
übersteht eine Verbindungspause; das Display holt die Liste, wenn er wieder antwortet, und gibt erst nach
180 s Stille auf. Das Löschen bleibt dabei, wie es war: Geht die Verbindung währenddessen verloren, ist der
Ausgang unbekannt, und es muss neu gelesen werden.

**Zweite Quelle, der Cerbo GX: Sie läuft seit dem 2026-10-12 auf dem Gerät im Fahrzeug** – mit dem Stand von
Commit `cf30cfe`: die Quelle, ihr Task, die fünf Seiten, der lange Druck. Was davon bekannt ist, stammt aus
`GET /api/info` des Displays (nur gelesen):

- 11:17, 11.160 s nach dem Start: Stand `live`, 41 Werte, 9284 Bündel genommen, keines verworfen, eine Sitzung,
  Verzug 0 ms. Interner Speicher für `malloc()`: 64 KB frei, 33 KB im Minimum seit dem Start, größter Block
  53 KB.
- 11:35, 12.245 s nach dem Start: 10.207 Bündel, weiter keines verworfen und dieselbe eine Sitzung, 41 Werte,
  Verzug 0 ms; 63 KB frei, Minimum 33 KB, größter Block 49 KB. Zwischen den beiden Lesungen 923 Bündel in
  1085 s – der Takt, den auch der PC am Broker gehört hat (unten). Die Ansichten des Displays sind gespeichert
  („W906 OM651 Standard + Cerbo").

In beiden Lesungen liegen „frei" und „größter Block" über den Zahlen, bei denen `main/gx.c` anhalten lässt
(50 KB und 16 KB, solange niemand den Webserver fragt). Das Minimum von 33 KB sagt für sich nichts über die
Quelle: Unter welcher Last es entstand, ist nicht bekannt, und schon ein Laden der Webseite drückt es (ohne die
Quelle gemessen am 2026-10-11, siehe `main/gx.c`).

Vom Gerät **nicht** bekannt, weil niemand es berichtet oder gemessen hat: wie die fünf Seiten auf dem runden
Bildschirm aussehen, ob der lange Druck dorthin führt, das Verhalten über Funkpausen und über einen Neustart des
Cerbo, der Tiefstand des Stacks des fünften Tasks, die Zeit unter dem Lock je Bündel.

**Der Editor für Cerbo-Werte (die Cerbo-Schlüssel in `GET /api/catalog`, `GET /api/layout/gx`, „Cerbo-Seiten
anhängen") ist auf dem Gerät noch nicht gelaufen**: Er ist jünger als die Firmware dort. Er ist auf dem PC
getestet, und die Webseite ist am 2026-10-12 in einem Browser gegen den Mock ausprobiert, nicht gegen das Gerät.

Auf dem PC, außerhalb der Tests: Der Code des Displays hat am Broker des Besitzers mitgehört – `main/gx.c` und
der Kern, für den PC übersetzt (`host/gx_live`), vom Mac aus und nur lesend. Am 2026-10-11 300 s lang: eine
Sitzung, 250 Bündel genommen, keines verworfen, 41 bis 42 Werte je Bündel, Abstände von 0,6 bis 5,3 s. Am
2026-10-12 600 s lang: eine Sitzung, 510 Bündel genommen, keines verworfen, 41 Werte, Abstände von 472 bis
4498 ms, davon 30 über 2 s, Verzug höchstens 2 ms. Der Flow sendet also nicht gleichmäßig jede Sekunde; die
Lücken liegen in seinem eigenen Takt, nicht in der Strecke. Im letzten Bündel dieses Tages fehlte von den 30
Werten der fünf Seiten einer, `GX_AC_OUT_P`: „AC-Ausgang" auf der Seite „Energie" ist dann ein Strich.

Ebenfalls offen: Nach jedem gewollten Neustart nennt die Info-Seite `wdt` als Grund (mehrfach gesehen, Ursache
nicht geklärt). Nicht geprüft am Board: Fehlerspeicher löschen, Zurücknehmen eines Updates, Wärme im Gehäuse,
der Web-Schirm mit seinen drei Zeilen und der dauerhaft freie Web-Zugriff.

| Teil | Stand |
|---|---|
| Logik (`components/core`, 29 Module) | auf dem PC getestet: 29 Testprogramme mit gut 15.300 Prüfungen, dazu über 9.900 Mutationen, von denen jede einen Test rot machen muss (gezählt am 2026-10-12). Läuft in der CI unter gcc, lokal unter clang, jeweils auch mit `unsigned char` wie auf dem ESP32. |
| Zeichencode (`components/ui`, LVGL 9.5.0) | in der CI gebaut; ein Renderer ohne Display zeichnet 80 von Hand gebaute Bildschirme und gut 200 Szenen aus den Tests der Logik und prüft, dass nichts über den Kreis ragt, sich überlappt oder abgeschnitten wird. Die Bilder hängen als Artefakt `wican-display-screens` am CI-Lauf. |
| Plattform (`main`: Start, Bildschirm-Task, WLAN, HTTP, Flash) | übersetzt und linkt für den ESP32-S3. Gegen ESP-IDF v5.5.2 und LVGL 9.5.0 gegengelesen. Dazu laufen `main.c`, `screen.c`, `net.c`, `web.c` und `gx.c` unverändert auf dem PC gegen die echte Logik, mit Platzhaltern für ESP-IDF, FreeRTOS und LVGL (`host/platform`, in der CI). Die Platzhalter sind ein nach den Quellen geschriebenes Modell: Sie zeigen, dass die Plattform ihre eigenen Regeln hält, nicht, dass Treiber, Scheduler und Flash sich so verhalten wie gelesen. |
| Board-Schicht (`components/board`) | Pins, Zeiten und die Panel-Initialisierung sind aus dem Schaltplan und den Beispielen des Herstellers gelesen. Am Board bestätigt (2026-10-09): Bild, Farben, Hintergrundlicht, Expander, Touch, Taster, Encoder. Gemessen ist nur die Zahl der Zählschritte je Raste. |
| Webseite (`main/web/index.html`) | in Chromium gegen einen Display-Mock auf dem PC ausprobiert. Nicht auf einem Telefon, nicht in Safari oder Firefox. Die Firmware bettet sie zweimal ein, wie sie ist und vom Build mit gzip gepackt (`tools/page_gz.py`), und schickt die gepackte an jeden Browser, der gzip annimmt: aus 60189 Bytes werden 20464 (gemessen am 2026-10-10 an der Seite des Commits 863a2c9, mit der zlib 1.2.12 des Mac). Das läuft in der Simulation von `web.c` und im Mock; gebaut und am Board gesehen ist es noch nicht. |
| WiCAN-Firmware (HTTP-Weg für den Fehlerspeicher) | auf dem Adapter im Fahrzeug geflasht und gemessen (2026-10-04): Lesen über HTTP funktioniert, MQTT unverändert. Löschen über HTTP ist am Fahrzeug noch nicht getestet. Siehe `tools/w906/API.md`. |

## Aufbau

```
components/core    alles Verhalten in reinem C ohne ESP-IDF; app.h ist das ganze Gerät, app_web.h seine Weboberfläche
components/ui      zeichnet, was core als scene_t vorgibt, und entscheidet nichts
components/board   die Hardware des CrowPanel: Expander, Panel, Knopf, Touch, Hintergrundlicht
components/store   was einen Neustart überlebt (NVS)
main               die Plattform: Tasks, WLAN, HTTP in beide Richtungen, Ereignisse der App
main/web           die Webseite, eine Datei
layouts            das eingebaute Layout für den W906 (35 Werte auf 7 Seiten) und die eingebauten Seiten des
                   Cerbo (30 Werte auf 5 Seiten)
test               Tests der Logik, Mutationslisten, redproof.py
host               der Renderer ohne Display
host/platform      die Plattform auf dem PC: Simulationen von main.c, screen.c, net.c, web.c und gx.c, ihre Mutationen
host/gx_live       gx.c und der Kern gegen die Sockets des PC: hört an einem echten MQTT-Broker mit
tools              Prüfungen für Partitionstabelle, Layouts, Webseite und Build; mock_display.py; mock_flow.py
                   (Broker und Flow eines Cerbo für die Werkbank); page_gz.py packt die Webseite für den Build der
                   Firmware, die Simulation und den Mock
API.md             die Weboberfläche des Displays
```

Die Regel dahinter: Was sich entscheiden lässt, steht in `components/core` und ist getestet. `main` und
`components/board` reichen nur weiter. Wer etwas am Verhalten ändert, ändert es in `core` und im Test dazu.

## Prüfen ohne Board

Logik-Tests und Rot-Nachweis (clang oder gcc, make, python3):

```bash
cd ~/projects/WiCAN/WiCAN_W906-display/display/test && make
```

```bash
cd ~/projects/WiCAN/WiCAN_W906-display/display/test && python3 redproof.py --module hold
```

Ohne `--module` laufen alle Mutationen; das dauert auf einem Laptop über eine Stunde, die CI teilt es in acht Teile.

Die Plattform auf dem PC und ihr Rot-Nachweis (dieser läuft nicht in der CI, etwa eine Minute):

```bash
cd ~/projects/WiCAN/WiCAN_W906-display/display/host/platform && make
```

```bash
cd ~/projects/WiCAN/WiCAN_W906-display/display/host/platform && python3 redproof.py
```

Webseite gegen den Mock (danach im Browser `http://127.0.0.1:8907/`, die Steuerleiste für Freigabe und Knopf
liegt unter `/mock`):

```bash
cd ~/projects/WiCAN/WiCAN_W906-display/display/tools && python3 mock_display.py --port 8907
```

Die Firmware selbst wird nur in der CI gebaut (`.github/workflows/display.yml`, ESP-IDF v5.5.2). Das Ergebnis
ist das Artefakt `wican-display`. Derselbe Job prüft, dass das Image ein Fünftel seines Slots frei lässt und
dass LVGL mit dem Assert-Handler aus `components/ui` übersetzt wird (ein fehlgeschlagenes Assert endet in
`abort()` und damit in einem Neustart, nicht in einer Endlosschleife).

## Flashen

Erst wenn die Prüfliste unten abgearbeitet wird. Das Board hängt über das mitgelieferte Kabel am USB des
ESP32-S3 selbst (kein USB-UART-Wandler); meldet es sich nicht, BOOT halten und RESET tippen.

Artefakt holen (legt die Dateien nach `~/Downloads/wican-display`):

```bash
gh run download -R mcgyver78/WiCAN_W906 --name wican-display --dir ~/Downloads/wican-display
```

Geflasht wird mit `esptool` (auf dem Mac über Homebrew; die Befehle unten sind gegen die Hilfe von
esptool 5.4.0 geprüft, geflasht wurde damit noch nichts):

```bash
brew install esptool
```

In allen Befehlen `/dev/cu.usbmodemXXXX` durch den Port ersetzen, unter dem sich das Board meldet:

```bash
ls /dev/cu.usbmodem*
```

Vor dem ersten Flashen die Werksfirmware sichern (die ganzen 16 MB, das dauert einige Minuten). Sie ist der
einzige Beleg, dass Board und Panel in Ordnung sind, und der Vergleich, wenn unser Bild nicht kommt:

```bash
esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX read-flash 0 0x1000000 ~/Downloads/crowpanel_werksfirmware.bin
```

Zurück zur Werksfirmware geht es mit derselben Datei:

```bash
esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX write-flash 0 ~/Downloads/crowpanel_werksfirmware.bin
```

Erstes Flashen über USB. Zuerst den ganzen Flash löschen (etwa eine Minute): Die Partitionen der
Werksfirmware liegen anders, und ihre Reste lägen sonst in unseren.

```bash
esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX erase-flash
```

Die Adressen gehören zur Partitionstabelle `partitions.csv` und dürfen nicht geändert werden:

```bash
cd ~/Downloads/wican-display && esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX write-flash 0x0 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin 0x29000 ota_data_initial.bin 0x30000 wican-display.bin
```

So ist es am 2026-10-09 gelaufen (löschen 61 s, schreiben 14 s, alle Prüfsummen bestätigt).

Die serielle Ausgabe (Start, Fehlermeldungen, die Zeilen zu den `CHECK:`-Stellen) kommt über dasselbe
USB-Kabel; beenden mit Ctrl-A, dann K. Läuft sie nicht mit, geht die Meldung eines Absturzes verloren:

```bash
screen /dev/cu.usbmodemXXXX 115200
```

Spätere Versionen kommen über die Webseite (Gerät → Firmware). Das Display startet eine hochgeladene Firmware
erst nach einem Knopfdruck und fragt nach ihrem ersten Start „Update in Ordnung?". Ohne Antwort binnen fünf
Minuten, oder nach jedem Neustart davor, läuft wieder die Version davor. Eine so zurückgenommene Version
bietet das Display danach nicht als „Vorherige Version" an.

Achtung nach erneutem Flashen über USB: Das schreibt die Buchführung des Bootloaders neu. Liegt im anderen
Slot noch ein zurückgenommenes Update, steht es danach wieder unter „Vorherige Version".

## Bedienung in Kürze

- Drehen: Seite vor und zurück, harte Enden. Kurz drücken: Menü. Lang drücken: eine Ebene zurück; auf den
  Wertseiten zur ersten Seite, die gerade Werte zeigt (ohne Cerbo ist das die erste Seite, wie bisher).
- Die Punkte am unteren Rand sind die Seiten, der gefüllte ist die gezeigte. Ohne eigene Farbe ist er weiß
  und die übrigen grau. Eine Farbe je Seite gibt die Webseite: Ansichten → Seite wählen → „eigene Farbe für
  den Punkt der Seite"; in der Seitenliste steht sie als Balken unter der Nummer. Im Layout ist es
  `"color":"#rrggbb"` an der Seite.
- Helligkeit: im Uhrzeigersinn heller, dagegen dunkler – unabhängig von der Einstellung „Drehrichtung", die
  nur für Seiten und Zeilen gilt.
- Fehlerspeicher nur über das Menü. Löschen: auf „Löschen" drehen und den Knopf drei Sekunden halten.
  Touch kann nur abbrechen. Gelöscht wird nur die Liste, die das Display selbst gerade gelesen hat und zeigt,
  höchstens zehn Minuten lang, bei Zündung an und Motor aus.
- Fragen des Displays beantwortet nur der Knopf. Ein Tipp auf den Bildschirm bestätigt nichts, auch nicht
  „Update in Ordnung?" – eine Firmware, mit der der Knopf nicht geht, wird so nach fünf Minuten
  zurückgenommen.
- Solange das Display liest oder löscht, sind Neustart, Vorherige Version und Werkseinstellungen gesperrt
  (die Zeilen sind grau), und die Webseite kann kein Netz speichern oder vergessen (Antwort „busy").
- Wird das Display zu heiß (85 °C am Chip), schaltet es das Licht ab. Offene Fragen und Dialoge sind dann
  beendet, ein gerade gehaltener Knopfdruck zählt nicht, und die Webseite bekommt für alles, was eine
  Bestätigung am Gerät braucht, die Antwort „hot".
- Web-Zugriff: Ab Werk ist er dauerhaft frei. Jedes Gerät im selben WLAN kann über die Webseite Ansichten und
  Einstellungen ändern, ein Netz vergessen, das Display neu starten und eine Firmware hochladen, ohne dass
  jemand am Display war – vom Start an, auch nach jedem Neustart. WLAN speichern, Firmware installieren und
  Werkseinstellungen brauchen weiterhin einen Knopfdruck am Gerät: die Frage steht auf dem Schirm, und wer
  sie nicht gestellt hat, lehnt mit einem langen Druck ab. Aussperren oder ohne Knopfdruck umflashen kann
  ein fremdes Gerät im WLAN das Display also nicht. Stören kann es: Ein Upload überschreibt die vorherige
  Version im zweiten Speicherplatz, und solange er läuft, liegt sein Hinweis über jedem Schirm und Knopf
  und Bildschirm bleiben ohne Wirkung; ein Neustart über die Webseite, bevor „Update in Ordnung?" bestätigt
  ist, nimmt ein Update zurück. In einem WLAN mit Geräten, denen man das nicht zutraut, gehört „Dauerhaft
  frei" auf nein. Die Webseite kann den Fehlerspeicher weder lesen noch löschen.
- Wer das frühere Verhalten will, stellt unter Menü → Web-Zugriff die Zeile „Dauerhaft frei" auf nein. Dann
  schaltet die Zeile „Freigabe" darunter Änderungen frei, 10 Minuten nach der letzten Änderung, höchstens 30
  Minuten. Solange „Dauerhaft frei" auf ja steht, ist „Freigabe" grau und zeigt „dauerhaft". Der Schalter
  lässt sich nur am Display umlegen, nicht über die Webseite. Er bleibt über Neustart und Update erhalten;
  Werkseinstellungen stellen ihn zurück auf ja. Ein Display, das von einer Firmware ohne diesen Schalter
  aktualisiert wird, ist danach dauerhaft frei – auch dann, wenn er schon einmal auf nein stand und man
  zwischendurch zu einer solchen Firmware zurückgegangen ist („Vorherige Version") und dort eine Einstellung
  geändert hat: sie speichert die Einstellungen ohne ihn.
- Ohne gespeichertes WLAN öffnet das Display einen eigenen Hotspot; Name und Passwort stehen unter
  Menü → Web-Zugriff. Solange kein Netz gespeichert ist und im sicheren Modus lässt er sich nicht abschalten
  (die Zeile „Hotspot" in den Einstellungen ist dann grau).
- Knopf beim Einschalten halten: sicherer Modus (eingebaute Ansichten, Hotspot an).

## Zweite Quelle: der Cerbo GX

Das Display abonniert auf dem MQTT-Broker des Cerbo ein einziges Topic, `display/gx/values`. Dorthin
veröffentlicht ein Flow in Node-RED etwa jede Sekunde ein Bündel: die Werte des Wohnmobils in einer
JSON-Nachricht. Das Display liest nur. Es kann kein MQTT-Paket bauen, das etwas veröffentlicht
(`components/core/mqtt_sub.h`), und damit auf dem Cerbo nichts schalten. Die Werte liegen in einem eigenen
Speicher: Nichts davon geht in den Fehlerspeicher, in den Standby oder in die Wahl des WLAN ein.

Auf dem Gerät läuft die Quelle seit dem 2026-10-12, der Editor für ihre Werte noch nicht: Was gelesen wurde und
was niemand gesehen hat, steht unter „Stand".

### Einrichten

1. In Node-RED auf dem Cerbo muss der Tab „Display" laufen. Der Besitzer hat ihn importiert; **er liegt nicht
   im Repository**, weil er die Geräte dieses Wohnmobils nennt. Es darf nur einen solchen Tab geben: Zwei
   senden abwechselnd auf dasselbe Topic.
2. In der Webseite des Displays unter Gerät → Einstellungen das Feld „Cerbo (IP-Adresse des MQTT-Brokers, leer
   = aus)" ausfüllen, hier `192.168.88.2`. Es nimmt nur eine IPv4-Adresse: keinen Namen, keinen Port (1883 ist
   fest). Das braucht den Web-Zugriff, aber keinen Knopfdruck.
3. Unter dem Feld steht danach, wie es um die Quelle steht („Cerbo 192.168.88.2: liefert · Werte: 41 · …").

Das Feld leeren schaltet die Quelle wieder ab. Im sicheren Modus ist sie aus, was auch gespeichert ist, und die
Werkseinstellungen löschen die Adresse. Wer auf eine Firmware von vor dieser Einstellung zurückgeht und dort
etwas einstellt, muss die Adresse danach neu eintragen: Jene speichert die Einstellungen ohne sie.

### Der Vertrag des Topics

```json
{"v":1,"boot":1836452071,"t":86400123,"values":{"GX_BATT_SOC":99.7,"GX_SW_B2B":"on"}}
```

- `v` ist die Version des Vertrags (1), `boot` die Nummer des Laufs des Flows, `t` die Millisekunden seit dessen
  Start, `values` je Schlüssel eine Zahl, `"on"` oder `"off"`.
- Ein Schlüssel ist `GX_` und dann 1 bis 29 Zeichen aus A–Z, 0–9 und `_`. Höchstens 64 Werte und 1400 Bytes je
  Bündel, QoS 0, nicht retained.
- Ein Bündel ist eine vollständige Momentaufnahme: Was darin fehlt, hat der Cerbo gerade nicht, und das Display
  zeigt dafür sofort einen Strich.
- An `t` erkennt das Display ein Bündel, das nach einer Funkpause verspätet ankommt, und zeigt es so alt, wie es
  ist.

Aufgeschrieben ist der Vertrag mit jeder Regel des Lesers in `components/core/cerbo.h` (dort stehen auch die
Grenzen für frisch, gedimmt und Strich), Beispiele sind `test/fixtures/gx_values_*.json`, und was die
Weboberfläche davon zeigt, steht in `API.md`. Die Tabelle der 43 Werte des Flows mit ihren Quellen am Cerbo
steht im Entwurf, der wie der Tab nicht im Repository liegt.

Ein neuer Wert braucht keine neue Firmware: ein Knoten mehr im Tab „Display", und der Schlüssel steht im
nächsten Bündel. Auf eine Seite legt man ihn in der Webseite (unten).

### Die fünf Seiten

Solange eine Adresse eingetragen ist – und nicht im sicheren Modus –, enden die Ansichten, die das Display selbst
wählt (eingebaut oder aus dem Katalog erzeugt), mit fünf eingebauten Seiten (`layouts/gx_default.json`):

| Seite | Werte |
|---|---|
| Batterie | SOC (Bogen), Spannung, Strom, Leistung, Zelle min, Zelle max |
| Energie | Solar, DC-System, AC-Ausgang, MultiPlus (Betriebsart), Starterbatterie, Alternator (Strom) |
| Alternator | B2B (an/aus), Strom, Spannung, Platine, MOSFET 1, MOSFET 2 |
| Wasser | Frischwasser und Grauwasser (Balken), Boiler, Wassertank, E-Boiler, Boiler-Ventil |
| Klima | Dinette, Heckgarage, Außen, Heizung Zuluft, Höhe, LTE RSRP |

Mit den sieben Seiten des W906 sind das zwölf, mehr fasst ein Layout nicht. Warn- und Alarmgrenzen haben die
fünf Seiten keine; die trägt man im Editor ein. **Ansichten, die jemand selbst gespeichert hat, ergänzt das
Display nie** – dafür gibt es im Editor „Cerbo-Seiten anhängen" (unten).

Eine Seite nur mit Cerbo-Werten zeigt sie auch, wenn das Fahrzeug steht und der WiCAN schläft. Eine gemischte
Seite zeigt Striche für die Quelle, die fehlt. Das Licht halten Cerbo-Werte nicht an: Der Bildschirm wird nach
der Standby-Zeit dunkel, ein Dreh, Druck oder Tipp weckt ihn, und die Werte stehen sofort da.

### Langer Druck

Auf den Wertseiten führt der lange Druck zur ersten Seite, die gerade Werte zeigt: geparkt (WiCAN schläft, Cerbo
liefert) zu „Batterie", unterwegs zu „Motor". Zeigt keine Seite Werte, führt er zur ersten Seite. Von selbst
wechselt das Display die Seite nie.

### Was das Display über den Cerbo sagt

Die Info-Seite (Menü → Info) hat zwei Zeilen mehr, hinter „WiCAN-Firmware" und nur, wenn eine Adresse eingetragen
ist:

- `Cerbo: 192.168.88.2 (liefert)` – die Adresse und der Stand in einem Wort: `liefert`, `wartet`,
  `nicht erreichbar`, `keine Werte`, `Flow zu neu`, `kein WLAN`, `aus (sicherer Modus)`.
- `MQTT: 12345 ok, 7 verworfen, 31 Verb.` – genommene Bündel, verworfene Nachrichten und Sitzungen mit dem
  Broker seit dem Start. Viele Sitzungen sind auf einer Funkstrecke mit Aussetzern kein Fehlerbild.

Eine Cerbo-Seite, die keine Werte zeigen kann, zeigt stattdessen einen dieser Texte; unter Werten, die noch
gedimmt stehen, steht er als Hinweis (`text_cerbo()` in `components/core/texts.h`):

| Text | Bedeutung | Ring einer Seite ohne Werte |
|---|---|---|
| Cerbo nicht eingerichtet | Keine Adresse eingetragen. Die Seite steht in gespeicherten Ansichten. | grau |
| Cerbo aus (sicherer Modus) | Eine Adresse ist gespeichert, aber im sicheren Modus bleibt die Quelle aus. | grau |
| Kein WLAN – Cerbo nicht erreichbar | Das Display ist in keinem Netz. | rot |
| Warte auf Cerbo … | Zweierlei: Es gibt noch keine Sitzung mit dem Broker, seit weniger als 15 s. Oder die Sitzung steht, aber seit 10 s kam kein Bündel, und noch ist nicht erwiesen, dass die Verbindung lebt – so sieht eine Funkpause aus. | gelb |
| Cerbo nicht erreichbar | Seit 15 s keine Sitzung: Cerbo aus, falsche Adresse oder außer Reichweite. | rot |
| Cerbo sendet keine Werte – Flow Display aktiv? | Die Sitzung steht, der Broker hat nach 20 s Stille auf eine Nachfrage geantwortet, und trotzdem kommt kein Bündel: Der Tab „Display" fehlt, ist abgeschaltet, oder Node-RED steht. | rot |
| Cerbo-Flow ist neuer als dieses Display | Die Bündel tragen eine höhere Version als 1. Das Display rät nicht, was sie bedeuten: Firmware aktualisieren. | rot |
| (kein Text) | Die Quelle liefert: Das letzte Bündel ist jünger als 10 s. | – |

### Cerbo-Werte auf eigene Seiten legen

In der Webseite unter Ansichten, und nur, wenn eine Adresse eingetragen ist:

- Die Liste „Verfügbare Werte" nennt hinter den Werten des WiCAN die Schlüssel des Cerbo, die das Display seit
  seinem Start oder seit dem Eintragen der Adresse in einem Bündel gesehen hat – auch die, die der Cerbo gerade
  nicht sendet (ein Gerät, das aus ist, fehlt im Bündel). Bis zu 64. Gespeichert werden sie nicht: Nach einem
  Neustart sind sie mit dem ersten Bündel wieder da.
- Antippen legt den Wert auf die offene Seite. Kennt eine der fünf eingebauten Seiten den Schlüssel, bringt er
  von dort Beschriftung, Einheit, Nachkommastellen und Darstellung mit (`GX_SW_B2B` etwa „B2B" mit an/aus);
  alles lässt sich danach ändern. Sonst kommt nur der Schlüssel, und Beschriftung und Einheit sind selbst
  einzutragen: Ein Cerbo-Wert hat im Katalog keine Einheit.
- „Cerbo-Seiten anhängen" hängt die fünf Seiten an die Ansichten im Editor, wenn alle Platz haben. Sonst sagt
  die Seite, wie viele Seiten zuerst zu löschen sind. Danach wie immer: am Display ansehen oder speichern. Das
  ist der Weg zu diesen Seiten für jemanden mit eigenen gespeicherten Ansichten.

### Werkzeuge

`tools/mock_flow.py` spielt auf dem PC Broker und Flow eines Cerbo, für ein Display an der Werkbank oder für
`gx_live`: Port 1883, ein Bündel je Sekunde mit 43 erfundenen Werten unter den Namen des echten Flows, und auf
Wunsch jeden Fehler, den ein Broker oder ein Flow machen kann (`--list`). Es verbindet sich nie irgendwohin. Als
Adresse des Cerbo trägt man am Display die des PC ein.

```bash
cd ~/projects/WiCAN/WiCAN_W906-display/display/tools && python3 mock_flow.py
```

`host/gx_live` ist `main/gx.c` mit dem Kern, für den PC übersetzt: Es verbindet sich mit einem echten Broker und
schreibt mit, was der Kern daraus macht – Stand, Zähler, Abstände der Bündel, Verzug, die Schlüssel des letzten
Bündels. Es kann nur verbinden, abonnieren und nachfragen, aus demselben Grund wie das Display. Damit, und nur
damit, wird am Broker des Cerbo mitgehört (hier 300 s):

```bash
cd ~/projects/WiCAN/WiCAN_W906-display/display/host/gx_live && make && build/gx_live 192.168.88.2 300
```

## Erster Tag am Board

Im Quelltext sind 63 Stellen mit `CHECK:` markiert (21 in `components/board/board.c`, 10 in `main/main.c`,
5 in `main/screen.c`, 13 in `main/net.c`, 7 in `main/gx.c`, 3 in `main/web.c`, 2 in `main/web/index.html`, 2 in
`sdkconfig.defaults`). Jede sagt, was zu beobachten ist und was zu ändern ist, wenn es anders kommt:

```bash
grep -rn "CHECK:" ~/projects/WiCAN/WiCAN_W906-display/display/components/board ~/projects/WiCAN/WiCAN_W906-display/display/main ~/projects/WiCAN/WiCAN_W906-display/display/sdkconfig.defaults
```

Die Reihenfolge, in der sie sich stellen:

1. Vor dem ersten Flashen: Meldet sich das Board am Mac als USB-Gerät? Die Werksfirmware läuft und zeigt ein
   Bild? Das ist der Beleg, dass Board und Kabel in Ordnung sind. Dann die Werksfirmware sichern (siehe
   „Flashen").
2. Nach dem Flashen, serielle Ausgabe mitlesen: Antwortet der Expander an 0x21? Welche Kennung meldet der
   Touch-Controller? Startet der WLAN-Treiber (er läuft ohne eigenen NVS-Bereich)? Steht in der Startzeile
   „safe mode 0", ohne dass eine Hand am Knopf war? Unter Menü → Info darf „Letzter Neustart" nie `task_wdt`
   zeigen: Der Watchdog beendet jetzt einen Hänger von Bildschirm- oder Zeichen-Task mit einem Neustart.
3. Bild: steht, gerade und ganz, auch mit WLAN-Verkehr? Farben richtig (Rot, Grün, Blau), nichts gespiegelt?
   Der Timing-Satz ist der der Werksfirmware (12 MHz); der Satz aus den ESPHome-Beispielen des Herstellers
   steht in `board_pins.h` als Ausweichweg.
4. Hintergrundlicht: nach dem Start dunkel, dann stufenlos heller, kein Flackern bei 1 %.
5. Knopf: Gemessen am 2026-10-09: Solange `KNOB_COUNTS_PER_DETENT` 4 war, brauchte eine Seite zwei Rasten
   der Hand. Der Knopf gibt also zwei Zählschritte je Raste, und die Konstante in `components/core/knob.h`
   steht seitdem auf 2. Gesehen am 2026-10-10: Im Uhrzeigersinn (von vorn) zählt der Encoder abwärts – der
   Fokus im Menü ging nach oben, die Helligkeit nach unten. Seiten und Zeilen bleiben so (wer es andersherum
   will: Einstellung „Drehrichtung"); die Helligkeit nimmt ihre Richtung jetzt von der Hand: im Uhrzeigersinn
   heller, am Board gesehen am 2026-10-11. Noch zu sehen: zehn Rasten vor und zurück ergeben null; die
   Helligkeit auch mit umgekehrter „Drehrichtung" im Uhrzeigersinn heller. Taster: losgelassen und
   gedrückt werden richtig gelesen, auch nach Minuten. Gehen Tastendrücke verloren, ohne dass gerade etwas
   gespeichert wird, dauert eine Lesung länger als die angenommenen 10 ms (`READ_MS` in `main/screen.c`).
6. Touch: Ein Tipp trifft die Zeile unter dem Finger, oben und unten in einer Liste.
7. Löschdialog: Der Ring füllt sich in drei Sekunden. Er darf neu beginnen, aber nie früher als nach drei
   Sekunden Halten vollenden – auch nicht, während im Browser ein Layout gespeichert wird.
8. Speicher und Wärme: kleinster freier interner Heap nach einer Stunde Betrieb mit Webseite; Chiptemperatur
   bei voller Helligkeit im geschlossenen Gehäuse.
9. Update: eine Firmware über die Webseite hochladen, bestätigen, „Update in Ordnung?" einmal bestätigen und
   einmal nicht (dann muss nach fünf Minuten die alte Version wieder laufen).

## Bekannte offene Punkte

- Die Plattform und die Board-Schicht sind gelesen, übersetzt und (die Plattform) auf dem PC simuliert, nie
  auf dem Board gelaufen.
- Bleibt der Netz-Task 60 s stehen (eine Gegenstelle, die ihre Antwort nie beendet), startet das Display neu.
  Das kann einen laufenden Firmware-Upload abbrechen.
- Während eines Firmware-Uploads fragt das Display den WiCAN weiter ab. Ob der Upload von 2 MB dabei bei
  laufendem Motor zügig durchläuft, ist am Board zu messen.
- „Update in Ordnung?" bleibt stehen, wenn die Hitze das Licht abschaltet; ihre fünf Minuten laufen weiter.
  Ein Update kann also zurückgenommen werden, nur weil das Display in dieser Zeit zu heiß war.
- Eine Seite, auf der seit dem Verbinden noch kein Wert geliefert wurde, zeigt Striche ohne gelben Ring.
- Löschen des Fehlerspeichers über HTTP ist am Fahrzeug noch nicht getestet, ebenso der Schlaf-Aufschub des
  WiCAN nach einem Scan.
- Die Webseite erkennt einen Neustart des Displays nur daran, dass die Laufzeit rückwärts springt.
- Zweite Quelle: Am Gerät sind nur ihre Zahlen gelesen, und der Editor für Cerbo-Werte ist dort noch nicht
  gelaufen (siehe „Stand"). Die Schlüssel des Cerbo für den Editor liest die Webseite neu, wenn sich die Zahl
  seiner Werte ändert oder der Reiter „Ansichten" geöffnet wird; ein Schlüssel, der nur einen anderen ersetzt,
  erscheint also erst dann.
- Sprache der Bedienoberfläche ist Deutsch, fest eingebaut.
