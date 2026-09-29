# Transcription interfaces

The existing Dictation/STT and transcript writer now have three app interfaces:

- **G2: Apps → Transcription**.
- **OLED/display: Apps → Transcription** (the shared display/input HAL).
- **Web: Sensors → Microphone → Transcription**, a collapsible panel.

Each offers Start/Stop, recent recognized text, the persistent saving preference,
and saved transcripts. Local P4 STT uses the existing continuous provider. A
Pi-backed build uses its existing bounded recording/result exchange; these
interfaces do not make the Pi protocol continuous or change recognition quality.
The provider must be available before starting (for example, ESP-SR must release
the microphone first). A busy keyboard dictation or another app session is not
stolen or cancelled.

## Saving and viewing

The saving switch changes `sttsavetranscripts`, the existing admin setting. Its
saved value is copied when a session starts. Changing it during a session affects
the next session, not the active one. The active session reports its own saving
state and storage errors separately from recognition errors.

Recent text is a bounded preview: approximately 1 KiB on OLED/G2 and 12,000
characters in the browser. Preview eviction does not remove saved text. Text
appears after completed recognition segments, not one word at a time.

Saved files are the existing per-account plain-text transcripts in internal
storage or SD. OLED/G2 enumerate eight files at a time and read 512-byte windows;
forward reading is not limited by the older file viewer's whole-file cap. Their
bounded backward-window history returns to the beginning after its history is
exhausted. The web panel uses existing authenticated file APIs and a paged preview.
The ordinary File Manager remains available for downloading/managing files.

On OLED, select **Recent live text** to scroll the preview. Select an internal/SD
file, scroll its text, and continue down or press A at the end to load the next
window; scroll above the top to read the previous window. The display's existing
A/select and B/back mappings apply to the wheel, buttons, or other HAL inputs.
On G2, the existing list selection and text-page gestures apply.

Stop finishes accepted work and drains final recognized text. Cancel discards
unfinished recognition; previously saved words remain. Leaving the whole OLED/G2
app cancels only the session owned by that app. Back from a text/file subview
returns within the app. Leaving the web page requests cancellation; browser
network delivery during exit is best effort. Collapsing the web panel keeps its
active session draining. Normal authentication expiry still applies.

## Integration

No frontend owns a recognition engine or transcript writer. The small
`DictationAppLease` API identifies an app admission by source, authenticated
transport epoch, and exchange ID. Existing keyboard consumers remain distinct.
Lost start replies can recover that same admission; repeated exact final text
acknowledgements are idempotent. Pi supervision runs in its existing worker so a
closed frontend cannot disable its timeout/revocation checks.

OLED and G2 use their existing authenticated asynchronous command bridges. File
I/O stays off rendering/I2C work; callbacks are fenced against identity and view
changes. The web adapter derives identity from a named cookie session and sends
private no-store responses. Automatic polls and receipts do not extend idle
activity or fill command audit logs. Saved text is rendered as text, never HTML.

The implementation is selected by microphone/UI capabilities, not by chip name.
Local P4 and Pi-backed ESP32/S3 builds use the same interface and storage code.
Physical accessory/provider qualification limits are recorded separately in
TRANSCRIPTION_UI_RESULTS.md.
