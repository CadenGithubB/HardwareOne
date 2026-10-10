# Icon ownership verification — 2026-10-09

Implementation, host checks and both firmware builds are complete. The tested
application is flashed on the QT Py. Automated screen/health checks have run;
a stationary File Browser follow-up is complete. The user confirmed correct icon
rendering and noted high placement. The final spacing correction and one
ToF shutdown counter increment are recorded below.

## Scope

Icon availability and file-extension mapping now use `System_Icons.h`, with
handwritten policy in `System_Icons_Lookup.cpp`. Generated `System_Icons.cpp`
payloads and their feature guards are unchanged. OLED drawing now belongs to
`OLED_UI`, through one `Adafruit_GFX*` canvas overload; the existing named global
adapter uses it. The shared canvas supports the existing SSD1306 and ST7789
backends.

The icon implementation and unused icon helpers are removed from
`System_Utils`. File-browser, menu, shared OLED, microphone, FM and web callers
use the owning headers instead of copied declarations. Asset names, extension
mapping, ordinary scaling and HTTP PNG payloads are preserved. Rendering
rejects invalid names, metadata, displays and scales without drawing.

## Host verification

Four focused suites pass with ASan/UBSan: `icon_ownership`,
`small_sensor_snapshots`, `fm_radio_snapshot` and `display_lifecycle`.

The new icon fixture was captured by executing the pre-change implementation.
It checks generated asset bytes and source hash, extension mappings/fallbacks,
and each compiled icon's pixels at nine scales plus a clipped placement.
The production lookup and renderer are exercised across OLED+web, OLED-only,
web-only and headless selections. The actual display header selects SSD1306
and ST7789 aliases over controlled pixel sinks; web-only/headless checks have
no display-driver headers. Named global drawing is compared with the shared
canvas overload, including invalid-input cases.

These checks establish source/API and pixel-output compatibility. The pixel
sinks do not qualify panel transport or physical appearance. See the
[host test guide](../components/hardwareone/test/host/README.md#icon-lookup-and-rendering-ownership)
for reproduction details.

## Firmware

| Profile | Application bytes | Free application space | Change from preceding build |
| --- | ---: | ---: | ---: |
| `qtpy-oled`, ownership move | 2,893,168 | 3,091,088 | -288 |
| `qtpy-oled`, final spacing correction | 2,893,520 | 3,090,736 | +64 |
| `xiao-default` | 5,888,272 | 95,984 | -112 |

Both builds pass. Saved feature selections are unchanged, and dependency lock
files were restored to their pre-build contents. The existing P4 mesh overlay
was intentionally preserved; no P4 build or hardware result is claimed here.

QT Py ownership-move application SHA-256:
`946a1a3f88c0f7786b5cee8325487dc0cc51944db7f472fc34e84588d9b4a393`.

QT Py ownership-move ELF SHA-256:
`453ea93ad19b0af236cffa1a0d14ad38bebfab17effa32ea17fa78f4fc2729eb`.

QT Py final application SHA-256:
`1f9636cb9d78f62947a26247c43a59d5ff8907ff259cc65e15044eefbc311c8d`.

QT Py final ELF SHA-256:
`c611393ff66eed0faab9420a2302b2c4e23a0c2acccc10a10691b06923fd91a7`.

XIAO application SHA-256:
`62514c4eb73aeffeb0c38662349da71187aea7454edee9597c66d2ddb8e6b853`.

XIAO ELF SHA-256:
`b6bf3382e39e306f8057290c8091ec74b8d269f1d1d621a7d47ba8679d15e42d`.

## Hardware verification

The ownership-move application-only flash at `0x10000` was hash verified. Boot reported the
expected ELF prefix `453ea93ad`; data/settings partitions were preserved. The
scan found GPS at `0x10`, ToF at `0x29`, SSD1306 at `0x3D`, and presence at
`0x5A`. APDS and gamepad were absent. GPS progress was checked indoors without
requiring a satellite fix.

The first 183.25-second run verified Menu, File Browser and System Status for
45 seconds each. Each reported mode was checked repeatedly alongside fresh
readings and single polling tasks for GPS, ToF and presence. Boot count stayed
40, crash count stayed zero, and tracked I2C errors and allocation failures
stayed zero. The user initially reported seeing only Menu, so this run does
not establish physical appearance for the other screens.

Cleanup removed the temporary display login correctly, causing the existing
authentication gate to show Login. The first harness incorrectly expected the
transient boot Logo to return and marked restoration failed. Settings,
authentication and initially stopped sensor states were restored. The harness
now expects Login when restoring an unauthenticated display with auth required;
no firmware policy was changed for this correction.

The second run lasted 159.50 seconds, including 120 seconds in File Browser.
Its 28 fresh reading sets and repeated task/mode checks passed. Boot count stayed
41, crashes stayed zero, and all health counters stayed zero during rendering.
During restoration, ToF's historical NACK/total-error counter rose once, while
consecutive errors returned to zero and the device did not become degraded.
Sensors stopped and settings/authentication were restored, but the strict
no-new-errors check correctly marked the complete run failed.

This is not proven to be an electrical NACK. The existing manager also labels
a false polling result as NACK when its address probe succeeds. A stop request
arriving while ToF waits for its bus can cause that false result; the observed
trace does not establish that exact interleaving. The empty driver destructor
and void stop transaction cannot directly account for this counter. Keep this
as a sensor shutdown/accounting follow-up rather than dismissing it as expected
or broadening the icon change without evidence.

The user saw Login after that run's automatic logout. At their request, a final
staging command explicitly logged into the display, selected and requested a
redraw of File Browser, verified that mode and the active display session, then
logged out of serial only. File Browser is left open for visual confirmation;
serial is closed, saved settings are unchanged, and sensors remain stopped as
at boot. The user then confirmed the icon looked correct except for being too
high; everything else was fine. The firmware has no framebuffer
export, so reported modes and runtime health are separate from panel appearance.

Local build artifacts `icon-test-*` and `hardware-test-*` index these results;
`icon-three-screen-results.json` and `icon-static-results.json` retain the raw
failed-script outcomes and their reasons. The preceding image/metadata are
preserved under `historical-before-icon-ownership-2026-10-09/`.


## Visual feedback and final spacing correction

The File Browser's old coordinate expression placed a 32-pixel icon at y=3 on
the 128x64 canvas, above the content origin y=11. Its label fit check also
compared an absolute coordinate to a height, hiding the one-line file details.
The screen now centers the 32-pixel icon, two-pixel gap and eight-pixel label
as one group inside the content area. Icon rows are 11–42 and label rows are
45–52; the footer starts at 54. Lookup, assets and rendering are unchanged.

Independent review confirmed these bounds. Both saved profiles rebuild;
XIAO's application and ELF remain byte-identical to the ownership-move build.
The final QT Py adds 352 bytes to that build because file details now fit and
are rendered. No additional regression framework was added for this small
positioning correction. The longer coexistence runs above belong to the
ownership-move image, not this final layout image.

The final application-only flash was hash verified, and boot identity matched
`c611393ff`. Final File Browser mode and display login were verified; boot count
was 44, crash count zero, all tracked I2C errors zero, and allocation
failures zero. The corrected File Browser is left open with the user's requested
display login; serial is logged out and closed. Saved settings were preserved.
No second user visual confirmation of the adjusted spacing is claimed.


The subsequent [ToF polling follow-up](QTPY_TOF_POLL_TEST.md) reproduces and
removes a false shutdown-error path and records six clean hardware restart
cycles. The original icon-test counters and failed-script outcomes above remain
part of the historical record.
