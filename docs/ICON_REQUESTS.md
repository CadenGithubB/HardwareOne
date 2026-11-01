# HardwareOne icon requests

Icons to draw for the HardwareOne firmware, from a scan of the firmware repo on 2026-10-22.

## How to draw them

- 32×32 pixels, 1-bit (pure on/off, no grey), drawn white on black, as in the
  HardwareOne Icon Tool's current 87-icon default set.
- Match that set: outline style, strokes about 2 px wide, about 2 px of empty
  margin, subject centered.
- Keep each icon readable when shrunk to 16×16. The OLED draws some icons at
  half size.
- Use each name exactly as written. Names are the firmware's lookup keys
  (lowercase, underscores).
- Prefer generic symbols over brand logos. They read better at this size.

Sections are in priority order.

## 1. Redraw in the new style

These icons are already in the firmware but missed the redesign, so they still
use the old artwork.

| Name | Draw |
|---|---|
| camera | Camera body with lens |
| debug | Bug |
| edgeimpulse | On-device machine learning (small neural-network nodes, or a chip with a spark) |
| espsr | Speech recognition (speech bubble with a sound wave) |
| help | Question mark in a circle (pairs with `info` and `warning`) |
| mic | Microphone |
| microphone | Microphone, studio/desk style (same subject as `mic`) |
| mqtt | Publish/subscribe hub (a center node with arrows in and out) |
| neopixel | Addressable RGB LED (a square pixel with rays), distinct from `led` |
| notify_display | Small screen / monitor |
| presence | Person with radiating waves (presence sensing) |
| rtc | Real-time clock chip (a chip with a clock face), distinct from `clock` |
| servo | Servo motor with its arm/horn |
| vol_mute | Speaker with an X (matches `speaker`) |
| vol_min | Speaker with one sound wave |
| vol_max | Speaker with three sound waves |
| web | Globe |

Also make these pairs look different. In the new set they are nearly identical:

| Name | Draw | Problem |
|---|---|---|
| memory | RAM stick (long board with chips and edge pins) | Differs from `notify_system` (chip) by only 16 pixels; both show in the same OLED menu |
| notify_cli | Terminal prompt with a small badge or bell | Differs from `terminal` by only 20 pixels |

## 2. New icons for menus that borrow another icon today

Each of these OLED menu entries currently shows another feature's icon.

| Name | Draw | Menu entry | Shows today |
|---|---|---|---|
| apps | 2×2 grid of rounded squares | Top level: Apps | notify_automation |
| signal | Antenna mast with waves | Top level: Connect | notify_server |
| sensor | Small sensor board giving off waves | Hardware: Sensors | notify_sensor |
| i2c | Chip on a two-wire bus (two parallel lines) | Hardware: I2C Scan | notify_sensor |
| chart | Line graph | Sensors: Data | notify_sensor |
| list | Bulleted list | Sensors: List | notify_sensor |
| gps | Satellite, or a location crosshair | Sensors: GPS | compass |
| perf | Speedometer / gauge | System: Perf | memory |
| keyboard | Keyboard | System: CLI Input | terminal |
| login | Arrow going into a door | Config: Login | user |
| logout | Arrow coming out of a door | Config: Logout | user |
| users | Two people | Config: Users | user |
| password | Text field showing `***` | Config: Change PW | lock |
| transcript | Microphone next to lines of text | Apps: Transcription | mic |
| map | Folded paper map | Apps: Maps | compass |
| llm | Chat bubble with a sparkle | Apps: LLM Chat | terminal |
| health | Heart with a pulse line | Apps: Health (smart-ring vitals) | bt_idle |

## 3. File-type icons

The OLED and web file browsers both pick an icon by file extension. The
firmware writes these file types, but today they show the plain `file` icon.

Draw them like the existing `file_*` icons: page outline with a folded corner
and a symbol inside.

| Name | Symbol on the page | Extensions |
|---|---|---|
| file_csv | Small table / grid | .csv (sensor logs) |
| file_audio | Music note or waveform | .wav .mp3 .pcm .lc3 |
| file_video | Film strip | .avi (camera recordings) |
| file_map | Map | .hwmap (map tiles) |
| file_model | Neural-network nodes | .tflite .gguf (ML models) |
| file_key | Key | .crt .pem .key (certificates and keys) |

## 4. Features with no icon yet

These firmware features have no icon of their own yet. Useful for future menu
rows and web pages.

| Name | Draw | Feature |
|---|---|---|
| ring | Smart ring | R1 smart ring |
| note | Sticky note | Quick notes |
| coprocessor | Small single-board computer with a link arrow | Linux co-processor (CM5) over UART, including its power control |
| fan | Fan blades | Co-processor (CM5) fan control |
| rotary_encoder | Knob with tick marks | Rotary encoder input |
| led_matrix | Grid of dots | LED matrix display |
| video | Video camera | Camera recording |
| mesh_nodes | Three or four connected dots | ESP-NOW mesh |
| shield | Shield | HTTPS / security |
| firmware_update | Chip with a circular arrow | Firmware (OTA) update |
| backup | Box with a circular arrow | Backup and restore (HardwareOne Migration Tool) |
| error | X in a circle (pairs with `info`, `warning`, `help`) | Error states |

## 5. Map controls

The OLED map's actions menu has these commands. That menu is plain text today,
so these icons only get used if it, or the web Maps page, gains icons.

| Name | Draw | Map command |
|---|---|---|
| map_pin | Map pin | Mark / Goto / Delete Waypoint |
| zoom_in | Magnifier with plus | Zoom In |
| zoom_out | Magnifier with minus | Zoom Out |
| recenter | Crosshair | Recenter, Center on GPS |
| route | Dotted path between two points | Transit Routes |

## 6. Earlier planned names, lower priority

The July backlog planned these names. Nothing draws them yet. Four of the
`notify_*` names don't match a notification category; the Matches column says
where those notifications fall instead.

| Name | Draw | Matches |
|---|---|---|
| bond_master | Bond link with a crown or star | Master side of a bonded pair |
| bond_worker | Bond link with a gear | Worker side of a bonded pair |
| mesh_master | Mesh with one highlighted node | ESP-NOW mesh primary/backup master |
| pair_beacon | Beacon / broadcast tower | ESP-NOW discovery beacons |
| key | Key | Stored keys and mesh passphrases |
| logo | HardwareOne mark | OLED logo screen |
| text | Letter "T" or text lines | OLED custom-text screen |
| animation | Play button with motion lines | OLED animation screen |
| notify_auth | Notification: sign-in / security | Notification category "Auth & Users" |
| notify_power | Notification: power / battery | Notification category "Power & Battery" |
| notify_voice | Notification: voice | Notification category "Voice, AI & Media" |
| notify_camera | Notification: camera | No category of its own (falls under "Voice, AI & Media") |
| notify_bluetooth | Notification: Bluetooth | No category of its own (split across "Connectivity" and "Glasses") |
| notify_network | Notification: network | Notification category "Connectivity" |
| notify_mqtt | Notification: MQTT | No category of its own (falls under "Connectivity") |
| notify_storage | Notification: storage | Notification category "Storage" |
| notify_time | Notification: time | No category of its own (falls under "Connectivity") |
