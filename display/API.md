# Web interface of the display

The display serves one page (`GET /`) for arranging the views, storing WiFi networks, looking at the last
fault memory list and installing firmware. The page talks to the display with the requests below; the
browser never talks to the WiCAN adapter, only the display does.

The rules are implemented in plain C and tested on the host: `components/core/web_route.h` (which request
is what, and which is refused before a handler runs), `web_json.h` (the bodies), `access.h` (who may change
something), `app_web.h` (what each request does). The examples named here are hand-written files in
`test/fixtures/` that the tests compare byte for byte.

Status of this document: the logic is tested on the host. The HTTP server that carries it runs on the
device only and has not run anywhere yet.

**The web interface can neither read nor clear the fault memory of the vehicle.** That is done at the knob.

## Who may do what

- **Reading** is open to everybody in the same WiFi. No answer ever contains a WiFi password.
- **Changing** needs the *release*, and **by default the display is released for good**: everybody in the
  same WiFi may change what the release alone allows - views, settings, forgetting a network, a restart,
  the upload of a firmware - from the start of the display on, with nobody at the display, for as long as
  it runs and after every restart. (The owner asked for that on 2026-10-10: he had to walk to the display
  for every change.) `GET /api/info` tells it with `"release":{"open":true,"left_s":0,"lasting":true}`.
  What that lets a device in the WiFi do without the knob, beyond views and settings: an upload overwrites
  the previous version in the other slot, and while its blocks arrive its notice lies over every screen
  and neither knob nor touch act; a restart before "Update in Ordnung?" was confirmed takes an update back.
  It cannot install what it uploaded, store a network or reset the display: those still ask at the display.
- **Who wants the display closed** switches "Dauerhaft frei" off at the display (menu "Web-Zugriff"); the
  setting `web_open` is stored and outlasts restart and update, and a factory reset switches it on again.
  It does not outlast a firmware from before this switch that stores the settings once (the previous
  version, after going back to it): that one writes them without the member, and the next firmware with
  the switch finds none and is released for good. The same holds for a stored text that is refused as a
  whole: the defaults apply, and by default the display is open.
  A change then needs the release of ten minutes, given by hand in the same menu (row "Freigabe"). It ends
  10 minutes after it was given or after the last accepted change, at the latest 30 minutes after it was
  switched on, when it is switched off at the display, and with a restart. Without it: `403`
  `{"error":"locked","hint":"Am Display: Menü > Web-Zugriff freigeben"}`.
  "Accepted" means that the request found the release open: a request that is then refused for another
  reason (a body that cannot be read, `busy`, an unknown network, a refused firmware file) has renewed it as
  well. Only a question to the knob that is not asked (`busy`, `asking`, `hot`, `body`) renews nothing.
  The release for good has no end and nothing to renew: no change shortens or lengthens it.
- **The switch is at the display alone.** `POST /api/settings` cannot change `web_open`, in neither
  direction (`400 {"error":"body","member":"web_open"}`): a device in the WiFi must not turn a release of
  ten minutes into one for good - the 30 minutes are there so that somebody has to come back to the
  display - and must not close the display to the others either.
- Three changes also need a **press of the knob**, under both releases, because they can lock the owner
  out or replace the firmware: storing a WiFi network, installing firmware, factory reset. So a device in
  the WiFi cannot take the display from its owner, however long the web interface is open. The request is
  answered with
  `202 {"ticket":17,"hint":"Am Display bestätigen: Knopf drücken"}`, the display shows the question for 60
  seconds, and `GET /api/ticket?id=17` tells what became of it. The knob alone answers: a short press
  says yes, a long press no. A touch on the screen does nothing to the question, wherever and whenever
  it lands - as for every question the display asks (clearing the fault memory, restart, previous
  version, factory reset, "Update in Ordnung?"): none of them is confirmed by a touch.
- Every request that is not a `GET` has to carry the header `X-Display: 1`. A web page of another origin
  cannot send it (the browser asks first, and that question is never answered). The display never sends an
  `Access-Control-Allow-*` header.
- The `Host` header has to be an IPv4 address or `wican-display.local` (optionally with a port). This keeps
  out pages whose name was bent to the address of the display.
- There is no login and no TLS. Whoever is in the WiFi while the release is open can change what the
  release allows - and by default it is open for good. In a WiFi with devices that are not to be trusted
  with that, switch "Dauerhaft frei" off at the display and keep the release closed when it is not needed.

## Requests

| Request | What it does | Release | Knob |
|---|---|---|---|
| `GET /` | the page | | |
| `GET /api/info` | version, memory, network, adapter, release, settings; the Cerbo GX if the display has its address | | |
| `GET /api/catalog` | the values the adapter can deliver | | |
| `GET /api/values` | the current values, from the memory of the display; those of the Cerbo GX as well if it has its address | | |
| `GET /api/layout` | the views in use, as the layout text | | |
| `PUT /api/layout?mode=check` | checks a layout, changes nothing | | |
| `PUT /api/layout?mode=apply` | shows it on the display without storing it | yes | |
| `PUT /api/layout?mode=save` | stores it | yes | |
| `POST /api/layout/reset` | back to the built-in or generated views | yes | |
| `GET /api/dtc/last` | the last fault memory list and the one before the last clear | | |
| `GET /api/wifi` | stored and visible networks | | |
| `POST /api/wifi` | store a network | yes | yes |
| `POST /api/wifi/forget` | remove a stored network | yes | |
| `POST /api/settings` | brightness, night mode, direction of the knob, standby, address of the Cerbo GX - not `web_open` | yes | |
| `POST /api/reboot` | restart | yes | |
| `POST /api/reset` | factory reset: WiFi, binding and settings; the views stay | yes | yes |
| `POST /api/ota` | firmware upload | yes | yes, after the upload |
| `GET /api/ticket?id=N` | what became of a question to the knob | | |

Paths are exact: no trailing slash, no upper case. A query string is only allowed where the table shows one.

The page has two forms. A client whose `Accept-Encoding` names `gzip` gets it compressed, with
`Content-Encoding: gzip` - a third of the bytes, made by the build of the firmware -, every other client as
it is. Both answers carry `Vary: Accept-Encoding`. No other answer is ever compressed.
"Names" is read narrowly (`accepts_gzip()` in `main/web.c`, after RFC 9110, 12.5.3):

- The header is a list of codings separated by commas, each with an optional weight `;q=` of 0 to 1 with at
  most three decimals. One of them is `gzip`, in letters of any case, and none is `gzip` with the weight 0,
  which refuses it. `gzip, deflate, br` is a yes, `gzip;q=0.5` as well, `gzip;q=0` and `gzip, gzip;q=0` are not.
- Weights are not compared: any weight above 0 is a yes, also next to a coding with a higher one. The
  display sends the smaller form to whoever reads it.
- `*` names nothing and is no yes; neither is `x-gzip`.
- Everything else gets the page as it is: no `Accept-Encoding`, an empty one, a header that is not such a
  list from its first byte to its last (`gzip;q=2`, `gzip;level=9`), one of 63 bytes or more (browsers send
  13 to 23), and also a client that refuses the plain page (`identity;q=0`) - there is no third form and
  no `406`. Of two `Accept-Encoding` lines in one request only the first is read.

Status of the two forms (2026-10-10): run in the simulation of `main/web.c` on a PC and in the mock of
`tools/`, which follows the same rules. Not yet built for the device.

One request at a time: the server of the display has a single task. What follows from that, on the device
(`main/web.c`; the mock in `tools/` does not model it):

- A body has to arrive whole within 15 seconds of its first byte, a firmware as long as the upload runs
  (30 seconds without a block end it). A body that takes longer gets no answer; the connection is closed.
- A `Content-Length` of 15 characters or more counts as the largest length there is, whatever its value: the
  request is refused (`413` where nothing else refuses it first) and the connection is closed. No client
  sends leading zeros.
- The page is sent with `X-Frame-Options: DENY` and `Content-Security-Policy: frame-ancestors 'none'`: it
  cannot be shown inside the page of another site. With `Vary` and, where it is compressed,
  `Content-Encoding` (above) these are all the headers it has. No other answer carries a header beyond
  status, type and length, and none an `Access-Control-Allow-*` header.
- Before a request is served, everything the requests before it asked to store is stored. A request that
  arrives after one that restarts the display (`POST /api/reboot`, a confirmed reset or installation) gets no
  answer any more: the display is gone half a second after it has answered that one.

## Refusals

Checked in this order; the first that applies is the answer. The body is `{"error":"<word>"}`; `locked`
comes with the hint shown above, the `body` of the settings with the name of the member.

| Status | Word | When |
|---|---|---|
| 404 | `not_found` | unknown path |
| 405 | `method` | known path, other method |
| 403 | `host` | `Host` missing or not allowed |
| 403 | `header` | not a `GET` and `X-Display: 1` missing |
| 403 | `locked` | the request changes something and the release is closed: "Dauerhaft frei" is switched off at the display, and no release of ten minutes is given |
| 400 | `query` | query string too long (63 bytes), `mode` or `id` missing, doubled or malformed, or a query where none belongs |
| 411 | `length` | not a `GET` and no `Content-Length` |
| 413 | `too_large` | body above 16384 bytes (layout), above the app slot (firmware), above 512 bytes (everything else); an empty firmware |
| 409 | `busy` | upload, restart or factory reset while the display reads or clears the fault memory, shows the clear dialog, or receives a firmware; a question to the knob while a firmware is received; storing or forgetting a WiFi network while the display reads or clears the fault memory |
| 409 | `asking` | a question to the knob, or the begin of a firmware upload, while a question still waits |
| 409 | `hot` | a question to the knob, or the begin of a firmware upload, while the heat keeps the backlight of the display off (`heat` is `off` in `GET /api/info`): nobody could see the question |
| 400 | `body` | the body is not what the request expects; for settings with `"member":"<name>"` (empty if the body is no JSON object; `web_open` also for a value that is good and is not the one in use) |
| 404 | `not_found` | `POST /api/wifi/forget` for a network that is not stored |
| 422 | see below | the firmware file is refused |
| 500 | `too_large` | the answer has no room in the 20480 bytes the display has for it (a catalogue whose names are control characters), or the views in use have no text |
| 500 | `upload` | the firmware upload broke, or the display had ended it |

Between the first checks and the request itself the release can end and the display can become busy: `403
locked` and `409 busy` are then answered by the request, in the order `locked`, `busy`, `asking`, `hot`, the
rest.

"Reads or clears the fault memory" means: a request of the display has been sent to the adapter or accepted
by it and has not ended. For as long, nothing takes the adapter away from under it: at the knob the rows
Neustart, Vorherige Version and Werkseinstellungen of the settings do nothing, and the web interface answers
`409 busy` to everything that restarts the display or makes it leave its network - restart, factory reset,
firmware upload, storing a network, forgetting one. A read would be left without its list, a clear without
its outcome.

A read does not end because the adapter is out of reach for a while: the display waits for the adapter to
answer again and then goes on by what its state shows - the adapter scans on without the display. For that
time the display stays busy. An adapter that stays away ends the read as failed 180 s after it accepted the
read; a read whose acceptance never reached the display, 180 s after it was sent. A clear does end when the
adapter is out of reach: its outcome is unknown from then on.

## Bodies

Numbers, texts and their order are fixed; texts are UTF-8.

### `GET /api/info` — `web_info.json`, `web_info_lasting.json`

The example is a display whose owner switched "Dauerhaft frei" off, 60 s into a release of ten minutes:

```json
{"project":"wican-display","version":"0.1.0","git":"display-v0.1.0-3-g1a2b3c4","slot":"ota_0",
 "reset":"poweron","up":4711,"safe_mode":false,"rolled_back":false,"update_pending":true,
 "heap":182340,"heap_min":151200,"psram":7340032,"psram_min":7100416,
 "heap_malloc":149572,"heap_malloc_min":118432,"heap_block":94208,"temp_c":47,"heat":"normal",
 "release":{"open":true,"left_s":540,"lasting":false},
 "wifi":{"ssid":"Werkstatt","ip":"192.168.1.77","rssi":-61,"ap":false,"ap_ssid":"WiCAN-Display"},
 "wican":{"host":"192.168.1.50","id":"a1b2c3d4e5f6","fw":"4.21","view":"live"},
 "layout":{"name":"W906 OM651 Standard","source":"stored"},
 "http":{"ok":12345,"failed":7,"reconnects":2},
 "settings":{"brightness":80,"night":25,"night_mode":false,"reverse":false,"standby_s":60,"web_open":false}}
```

**As it comes** — `web_info_lasting.json` — the display is released for good, and the same answer reads

```json
{...,"release":{"open":true,"left_s":0,"lasting":true},...,
 "settings":{"brightness":80,"night":25,"night_mode":false,"reverse":false,"standby_s":60,"web_open":true}}
```

- `view`: what the display shows about the adapter: `live`, `no_wifi`, `connecting`, `no_answer`,
  `foreign`, `no_api`, `autopid_off`, `starting`, `scan`, `ecu_offline`.
- `layout.source`: `stored`, `builtin`, `generated` (made from the catalogue for a vehicle nobody wrote
  views for) or `preview` (applied but not stored).
- `heat`: `normal`, `dim`, `off` (backlight limited by the chip temperature). While it is `off` nobody can
  see the screen: a question that waits is refused, the display asks none and begins no firmware upload
  (`409 hot`).
- `up`: seconds since the start. `temp_c`: the last reading that succeeded, 0 before the first.
- The memory, in bytes, read about once a second, 0 before the first reading. `heap` is what is free of
  the internal RAM and `heap_min` the lowest that was since the start, `psram` and `psram_min` the same of
  the external RAM. `heap` counts 32 KB the firmware keeps back for memory that has to be internal
  (`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL`): `malloc()` gets nothing of them, so what is allocated with
  `malloc()` - the buffers of lwIP among it - has less than `heap` says. What it has are the three numbers
  behind `psram_min`: `heap_malloc` is what is free of the internal memory `malloc()` hands out,
  `heap_malloc_min` the lowest that was since the start, `heap_block` its largest free block - the largest
  piece `malloc()` could get from the internal RAM at that moment. Each lowest stand is the sum of the
  lowest stands of the parts the memory is made of, which need not have been at their lowest together: it
  can be lower than what was free at any one moment, never higher.
- `release.open`: a change is accepted. `release.lasting`: the release is the one for good, which has no
  end - the last member of the object; a firmware from before this member does not name it. `release.left_s`:
  seconds until the release ends, rounded up; 0 if it is closed, **and 0 while it is lasting**: there is no
  end to count down to. A page must look at `lasting` before it makes a time of `left_s`.
- `wican.fw` is empty until the adapter has answered. The password of the own access point is not part of
  the answer; it is shown on the screen of the display (menu "Web-Zugriff").

**With a second source** — `web_info_cerbo.json`. A display that was given the address of a Victron Cerbo GX
(`cerbo` of the settings, below) reads values from its MQTT broker as well (`components/core/cerbo.h`). Its
answer has one object more, behind `wican`, and the address in its settings:

```json
{...,"wican":{"host":"192.168.1.50","id":"a1b2c3d4e5f6","fw":"4.21","view":"live"},
 "cerbo":{"host":"192.168.88.2","view":"live","values":41,"ok":12345,"refused":7,"connects":31,"boots":1,"lag_ms":120},
 "layout":{...},"http":{...},
 "settings":{"brightness":80,"night":25,"night_mode":false,"reverse":false,"standby_s":60,"cerbo":"192.168.88.2","web_open":false}}
```

**A display without that address answers `web_info.json`, byte for byte**: no object `cerbo`, no member
`cerbo` in the settings. A page has to cope with both.

- `cerbo.host`: the address that is set. The port is 1883.
- `cerbo.view`: what the display shows about the Cerbo: `off` (safe mode: the source is off whatever is
  stored), `no_wifi` (the display is in no network), `connecting` (no session with the broker for less than
  15 s), `unreachable` (for longer), `quiet` (a session, and no bundle for 10 s), `silent` (a session whose
  broker answers, and no bundle for 20 s: the flow "Display" in Node-RED does not publish), `newer` (the
  flow publishes a newer version of the contract than this firmware reads), `live`.
- `cerbo.values`: the values of the Cerbo the display has to show right now: those of the last bundle, until
  they are 60 s old. It is the number of keys with `GX_` in `GET /api/values` at that moment: what that answer
  leaves out is not counted (a number beyond the range of a double, sent as `1e999`).
- `cerbo.ok`: bundles taken. `cerbo.refused`: messages not taken - no bundle of the
  contract, a newer version, a retained message, another topic, too long. `cerbo.connects`: sessions with the
  broker that stood. `cerbo.boots`: runs of the flow the display has seen (a number that keeps running means
  two flows publish). `cerbo.lag_ms`: by how much the last bundle arrived late. All counted since the start of
  the display, at most 4294967295.
- The display only ever subscribes: it cannot build the MQTT packet that publishes.

What the display does at that address (`main/gx.c`, `components/core/cerbo.h`): one TCP connection to port
1883, MQTT 3.1.1. A CONNECT with a clean session and a keep-alive of 30 s, without a will, a user name or a
password; the client id is made of the letters and digits of the name of its own access point
(`WiCAN-Display-C9A8` names itself `WiCANDisplayC9A8`). One SUBSCRIBE to `display/gx/values` with QoS 0. A
PINGREQ every 10 s. Nothing else is ever sent. A connection without a session after 15 s, or without the
answer to a PINGREQ after 20 s, is closed; the next attempt follows after 2, 5, 10 and then every 30 s, and
at once when the display joins a network or is given another address. In safe mode nothing is opened. What
a broker sends is read at 51 KB a second at most. `tools/mock_flow.py` plays the broker and the flow of a
Cerbo on a PC, with every fault they can have.

### `GET /api/catalog`

```json
{"@BATT_V":{"unit":"V","class":"","profile":false,"delivered":false},
 "ENGINE_RPM":{"unit":"rpm","class":"frequency","profile":true,"delivered":true}}
```

`@BATT_V`, the battery voltage measured by the adapter, comes first; then every value the vehicle profile
of the adapter names (`profile`), in the order of the profile, then what has arrived without being named
there (`delivered`). The last catalogue is kept on the display, so the views can be edited while the
adapter sleeps.

### `GET /api/values` — `web_values.json`

```json
{"view":"live","values":{"ENGINE_RPM":{"v":812.5,"age":"fresh"},"DPF_REGEN_STATUS":{"v":"on","age":"old"}}}
```

`v` is a number, `"on"` or `"off"`; `age` is `fresh` (under 3 s) or `old` (under 10 s). Older values are
left out, and so is a number beyond the range of a double (the source sent `1e999`): the display holds it
and has no text for it.

**With a second source** — `web_values_cerbo.json`. A display that has the address of a Cerbo GX answers
with one member more, `cerbo`, behind `view` - what it shows about the Cerbo, the words of `cerbo.view` in
`GET /api/info` -, and with the values of the Cerbo in the same object `values`, behind those of the
adapter:

```json
{"view":"live","cerbo":"live","values":{"ENGINE_RPM":{"v":812.5,"age":"fresh"},"DPF_REGEN_STATUS":{"v":"on","age":"old"},
 "GX_BATT_SOC":{"v":99.7,"age":"fresh"},"GX_SW_B2B":{"v":"on","age":"fresh"},"GX_TANK_FRESH":{"v":77,"age":"old"}}}
```

Every key of the Cerbo begins with `GX_`, and in this answer the prefix belongs to the Cerbo alone. The
display sees to that: a value of the adapter whose name begins with `GX_` - a vehicle profile may name one
so - is left out of this answer, whether the Cerbo publishes that key or not, from the moment an address is
set (also in safe mode, where the source is off). So no key stands twice in `values`, a key with `GX_` is
always a value of the Cerbo, and there are as many of them as `cerbo.values` in `GET /api/info` says.

A value of the Cerbo is `fresh` under 5 s and `old` under 60 s: the radio link to it drops out, and a state
of charge of 40 s ago still says something. A value the last bundle of the Cerbo did not name is left out at
once: the Cerbo has none.
Without the address the answer is the one above, byte for byte: no member `cerbo`, and no value left out for
its name - one of the adapter named `GX_...` is then a value like every other, as it always was. With both
stores full the answer has at most 19511 bytes.

### Layout — `GET /api/layout`, `PUT /api/layout?mode=...`

The format is described in `components/core/layout.h`; `layouts/w906_default.json` is the built-in one.
At most 12 pages with 1 to 6 values each, 16384 bytes. The answer to a `PUT`:

```json
{"ok":true,"name":"W906 OM651 Standard","pages":2,"items":5,"warnings":0,"warning_path":"","warning":"","unknown":[]}
{"ok":false,"path":"pages[2].items[0].min","problem":"min is not below max"}
```

Status 200 if the layout is taken, 400 if not. `unknown` lists the keys that are not in the catalogue
(they are shown as "n. v."); it is empty while no catalogue is loaded. A key that begins with `GX_` is never
listed: it names a value of the Cerbo GX, which is in no catalogue - whether the display has the address of
a Cerbo or not, and whether its flow sends that key or not. `mode=check` needs no release.

`mode=apply` shows the layout at once without storing it (`layout.source` is
`preview`): a restart, a reset or a save ends the preview, and the page in the browser keeps the text it
loaded if it wants to go back. `mode=save` stores it. With both the display keeps the page it shows if the
new layout shows a page at the same position (not hidden, and with a value of the catalogue), whatever
that page holds now; without one there it starts at the first page the new layout shows.
`POST /api/layout/reset` removes the stored layout and
answers the report of the views the display then uses by itself (the built-in ones, or views made from the
catalogue of another vehicle), shown from their first page; its body is not looked at. `GET /api/layout`
answers the text of the views in use: for views the browser sent, the text that was sent, byte for byte.

**With a second source** — `app_web_layout_cerbo.json`, `app_web_report_cerbo.json`. While the display has the
address of a Cerbo GX (`cerbo` of the settings; not in safe mode), the views it uses by itself end with the
five built-in pages of the Cerbo, `layouts/gx_default.json`: Batterie, Energie, Alternator, Wasser, Klima.
`GET /api/layout` then answers one text for all of them, written by the display without whitespace, under the
name of the two - `"name":"W906 OM651 Standard + Cerbo"`, 12 pages, 4604 bytes for the W906 -, and
`layout.name` of `GET /api/info` is that name; `layout.source` goes on naming the part of the vehicle
(`builtin` or `generated`). Where the views of the vehicle and the five pages are more than 12 pages together,
the last pages of the vehicle give way. The text can be sent back as it is: `mode=check` answers 12 pages, 65
values, nothing unknown. The pages come and go with the address (`POST /api/settings`), and
`POST /api/layout/reset` answers the report of the views with them. **Views the browser sent are never
appended to** - a stored layout and a preview stay what was sent, with the address and without it, and keep
pages of the Cerbo they hold: such a page says "Cerbo nicht eingerichtet" on a display without the address.
A display without the address answers the text of before, byte for byte.

A page shows a value whose key begins with `GX_` from what the Cerbo sent - under 5 s as it is, under 60 s
dimmed, then a dash - with the label and the unit the layout gives it (no catalogue is asked for it). A page of
such values alone shows them whatever the adapter does, also while the vehicle is parked and the adapter
sleeps; a page that names values of both shows dashes for the source that is away.

### `GET /api/dtc/last` — `web_dtc_last.json`

```json
{"read":<result or null>,"read_age_s":120,"before_clear":<result or null>}
```

`read` is the list the display read itself and still shows, `read_age_s` the seconds since that read
ended (0 without a list), `before_clear` the list before the last clear that the adapter accepted or may
have accepted. Both are the result text of the adapter (`tools/w906/API.md`, W906.md).

### WiFi — `web_wifi.json`, `web_wifi_request.json`, `web_forget_request.json`

```json
{"current":"Werkstatt",
 "profiles":[{"ssid":"Werkstatt","host":"192.168.1.50","password":true,"factory":false,"wican_ap":false}],
 "seen":[{"ssid":"Freifunk","rssi":-88,"secure":false}]}
```

`password` tells whether one is stored, `factory` whether it is the password printed in the documentation
of the WiCAN, `wican_ap` whether the network is the access point of a WiCAN. Up to 4 networks are stored;
the order is the priority.

- `POST /api/wifi` with `{"ssid":"..","password":"..","host":".."}`. `host` is optional: empty means the
  display finds the adapter itself (the gateway in the access point of a WiCAN, else the service
  `_wican._tcp`). Without `password` the stored password of that SSID is kept; for a new SSID it means an
  open network - judged by what is stored when the knob is pressed. A password has 8 to 64 bytes. Answer:
  `202` with a ticket; the display shows the SSID with its question. With the press of the knob the display
  leaves the network it is in and looks for one anew.
- `POST /api/wifi/forget` with `{"ssid":".."}`: `200 {"ok":true}`, `404` if no such network is stored.
  Without any stored network the display opens its own access point. With every network that is forgotten
  the display leaves the network it is in and looks for one anew, also when another one was named.
- Both are answered `409 busy` while the display reads or clears the fault memory: it would lose the
  adapter in the middle of it. (While the clear dialog only shows, a network can be stored or forgotten:
  nothing has been sent yet, and the dialog closes when the adapter is gone.)

### `POST /api/settings` — `settings_*.json`

```json
{"brightness":80,"night":25,"night_mode":false,"reverse":false,"standby_s":60,"web_open":true}
```

Every member is optional. `brightness` and `night` 5 to 100 percent, `standby_s` 0 to 3600 (0 = never):
after that long without input the backlight goes dark while there is nothing to show. `reverse` turns the
direction of the knob for pages and rows; the brightness screen does not follow it - there, clockwise is
always brighter. Answer: the settings now in use.

`web_open` — `settings_defaults.json`, `settings_web_closed.json` — tells whether the web interface is
released for good ("Dauerhaft frei" at the display). It is the last member of every answer and of the
settings in `GET /api/info`, `true` on a display as it comes. **A request cannot change it**: the value in
use, or no `web_open` at all, is taken as every other text - a page may send back what it got -, the other
value is answered `400 {"error":"body","member":"web_open"}` (`app_web_member_web_open.json`) and nothing
of the request is taken, its other members neither. A `web_open` that is no boolean is answered the same
way, as every member of the wrong type. Where another member of the text is refused, that member is named.
A firmware from before this member stored and answered the five members alone: a display that is updated
finds no `web_open` in what is stored and so is released for good from its first start on.

`cerbo` — `settings_cerbo.json` — is the address of the MQTT broker of a Victron Cerbo GX, the second source
of values: `{"cerbo":"192.168.88.2"}`. An IPv4 address in dotted form and nothing else - four numbers from 0
to 255 without a sign and without a leading zero - or the empty text `""`, which takes the address away and
switches the source off. No name (it would have to be looked up), no port (it is 1883). Anything else:
`400 {"error":"body","member":"cerbo"}`. The source follows at once: with another address the values of
the old one are gone with the answer. It needs the release and no press of the knob: a wrong address locks
nobody out.
Where a request switches the source on or off - the first address, or `""` - and the views in use are the
ones the display chose itself, they get the built-in pages of the Cerbo or lose them with the same answer
(see Layout above); the display stays on the page it shows if the views still have one there, else it goes to
their first page. Nothing of that is stored but the setting. Another address in place of one changes no view,
and in safe mode no request does.

The answer, and the settings in `GET /api/info`, name `cerbo` **only while an address is set**, behind
`standby_s` and before `web_open`. Without one they are the text above, byte for byte - a display nobody
gave an address stores and answers no member for it. As every member, `cerbo` keeps its value when a
request does not name it. A firmware from before this member ignores it and stores its own five: after an
update that was taken back the address has to be entered again. The factory reset removes it.

### Tickets — `web_ticket.json`, `web_asked.json`

```json
{"ticket":17,"state":"waiting","left_s":42}
```

`state`: `waiting`, `confirmed`, `refused` (at the display by a long press, or the release ended or was
switched off - "Dauerhaft frei" as well -, or the heat switched the screen off while the question waited),
`expired` (nobody pressed the knob), `unknown`. `left_s` are the 60 seconds of the question, cut short only
by the end of a release of ten minutes; the release for good cuts nothing short.
The numbers start again at 1 after a restart of the display. After a confirmed firmware installation or
factory reset the display restarts and cannot answer any more.
A press in the first 1.5 seconds after the question does not count, and a touch on the screen never does.
Known are the last ticket and the one before it.

### `POST /api/ota`

The body is the firmware file (`wican-display.bin`), `Content-Type: application/octet-stream`. The first
bytes are checked before anything is erased; a refused file is answered with `422` and one of
`too_short`, `no_image`, `wrong_chip`, `no_description`, `wrong_project` (for example the firmware of the
WiCAN), `too_large`. A broken upload ends with `500 {"error":"upload"}`; an upload that brings nothing for
30 seconds is ended by the display. A complete upload is answered with `202` and a ticket: the display
shows the version of the file and asks for the knob. Only the press starts the new firmware. After its
first start it asks "Update in Ordnung?" on the screen, and the knob alone says yes (a firmware whose knob
does not work is not kept because its touch screen does); without an answer within 5 minutes, or after any
restart before that, the version before it runs again.

An upload begins only while the release lasts another 5 minutes - the release for good always does. The
upload itself renews a release of ten minutes, so `403 locked` is answered only in the last 5 minutes of
the 30 after which somebody has to give that release again at the display. While a question waits no
upload begins (`409 asking`), while one runs no second one (`409 busy`), and none while the running
firmware still waits for "Update in Ordnung?" (`409 busy`: the other slot holds the version the display
goes back to if nobody confirms), and none while the heat keeps the screen dark (`409 hot`: nobody could
see the question at its end). If the release has ended
when the upload is complete, the answer is `403 locked` and the file is not asked for; if the heat has
switched the screen off by then, it is `409 hot`, and the file is not asked for either. From the begin of
an upload on the display no longer offers "Vorherige Version": the other slot is being overwritten. It
does not offer it either for an update that was started and not confirmed: the version the display went
back from is never the "previous" one.
