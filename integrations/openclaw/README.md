# Hardware One OpenClaw device reads

This first Wi-Fi integration packages a portable skill and a standard-library
Python 3.9+ device client. It reads existing Hardware One APIs; it does not install
OpenClaw or configure a gateway. The separate optional ESP-to-agent source and
its setup are documented in [OpenClaw setup](../../docs/OPENCLAW_SETUP.md).
It needs no firmware API changes. There are no mutation commands in this version.

An optional [HTTPS chat relay](relay/README.md) serves the other direction:
XIAO-to-Mingus conversation while keeping the gateway local and its credential
off the device. It is separate from this device-read helper. The user's existing
registered Hardware One tools have already passed live device-read checks;
there is no need to replace them with this helper for the chat prototype.

## Configure the host

Use a dedicated device account. A guest account can read the selected HTTP
endpoints. Keep credentials in the host's private service environment or secret
manager, outside this repository and the skill text. The helper reads:

| Variable | Purpose |
| --- | --- |
| `HW1_DEVICE_URL` | Device origin, for example `https://hardwareone.example:443`; no credentials or path |
| `HW1_USERNAME` | Device account name |
| `HW1_PASSWORD` | Device account password |
| `HW1_CA_FILE` | Optional PEM trust bundle for the device certificate |
| `HW1_TIMEOUT_SECONDS` | Socket timeout and connected-request deadline, default 5, range 0.1–30 seconds |
| `HW1_ALLOW_HTTP` | Only literal `1` enables plaintext for an explicitly chosen bench setup |

HTTPS verifies the trust chain and hostname. For a private/self-signed device
certificate, provision its trusted certificate in `HW1_CA_FILE` and use a URL
whose hostname/IP matches that certificate. There is no insecure TLS switch or
automatic HTTPS-to-HTTP downgrade. Hardware One may fall back to HTTP internally;
this client will still fail its configured HTTPS request. Plain HTTP sends Basic
credentials without encryption and must be chosen deliberately.

Place the complete `hardwareone-device/` directory in an OpenClaw skill location
supported by the installed version, retaining `SKILL.md` and `scripts/` together.
Configure its execution environment to inherit only the intended device settings.
No skill discovery or execution was verified against a live OpenClaw install.

From this directory, with configuration already supplied privately:

```sh
python3 hardwareone-device/scripts/hardwareone_client.py device_info
python3 hardwareone-device/scripts/hardwareone_client.py read_sensor imu
python3 hardwareone-device/scripts/hardwareone_client.py read_sensor --help
```

The same Python client can be wrapped by explicitly registered host tools.
`device_info()` and `read_sensor(sensor_id)` are the intended operation boundary.
No credential arguments, free-form command, arbitrary URL, file API, event stream,
or interactive confirmation operation is exposed by the CLI.

## Request and output contract

| Operation | Requests |
| --- | --- |
| `device_info` | Four sequential GETs: `/api/ping`, `/api/buildconfig`, `/api/devices`, `/api/system` |
| `read_sensor ID` | One GET: `/api/sensors?sensor=ID`, with a fixed allowlist |

Every request uses Basic auth. There are no automatic retries, redirects, cookies,
proxy-environment handling, or writes. Errors omit response bodies, credentials,
and exception details. Requests stop on the first failure. A socket timeout
bounds connection operations; once connected, a watchdog also interrupts slow
headers and trickling bodies. System DNS resolution remains subject to the host
resolver. `device_info` can take four request deadlines plus connection time.

Responses are capped at 16 KiB each and must be uncompressed UTF-8 JSON objects.
The CLI caps its combined serialized output at 96 KiB. Success exits 0 and prints
`{"ok":true,"operation":"...","data":{...}}`; failure exits 1 and prints
`{"ok":false,"error":"...","message":"..."}`. Argument errors exit 2.
Sensor `valid:false`, `connected:false`, or `enabled:false` stays in `data` and
must not be interpreted as a usable current sample. Device-level errors such as
`not_compiled` and `not_enabled` are failures. The API can return cached readings;
the helper does not start sensors or force sampling. The caller must retain
device-side permissions when adding any future tools.

## Tests

```sh
python3 -m unittest discover -s tests -v
```

Tests start ephemeral loopback HTTP servers and exercise the actual client/CLI.
TLS tests generate a temporary certificate with the `openssl` executable if it
is available, then verify trust and hostname failures and a trusted connection.
No certificates, private keys, live device settings, or passwords are stored by
the package. TLS tests are skipped when `openssl` is unavailable; the client
itself has no dependency on that executable.

## Deferred Pi/UART tools

The restricted Pi OpenClaw profile cannot run this skill by adding Markdown
alone. Do not grant general shell, unrestricted networking, or raw serial access
to make it run. A later narrow registered-tool integration will call a local
broker in the existing `hw1-ai-service` daemon, with argument validation and an
operation allowlist. The daemon will remain the sole UART owner and reuse its
authenticated ordinary-user device session. It must support tool callbacks while
chat streaming is active and work with ESP Wi-Fi disabled. Neither that broker
nor a UART adapter is implemented here.
