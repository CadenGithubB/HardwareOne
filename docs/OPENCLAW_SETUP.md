# OpenClaw Wi-Fi prototype

Hardware One can select `http:openclaw` as an optional answer source for its
existing chat UI. The ESP sends the user's current message and displays streamed
text; the agent, conversation history, and tools run on the OpenClaw host.
The optional [device skill](../integrations/openclaw/README.md) lets that host
read existing device APIs. Conversation does not require the skill.

The source is consolidated on `main` for 0.99.98. Both `xiao-openclaw` and
`p4-openclaw` saved profiles are available. The live XIAO evidence below comes
from the earlier 0.99.97.4-based image; the combined P4 voice path still needs
hardware qualification. See [current working status](OPENCLAW_WORK_STATUS.md)
and [consolidation checks](CONSOLIDATION_0.99.98.md).

## Collect host information without changing its network

Run these in a terminal on the OpenClaw host, using the same account/profile as
its gateway. For a container install, run them in the OpenClaw container.

```sh
openclaw --version
openclaw config get gateway.port
openclaw config get gateway.bind
openclaw config get gateway.tls.enabled
openclaw config get gateway.http.endpoints.chatCompletions.enabled
openclaw agents list
```

Record the version, port, bind mode, TLS/endpoint flags, and intended agent ID.
An unset/default or unknown-path response is useful version information. Do not
share tokens, passwords, or full configuration dumps. These are read-only
commands; they do not enable an endpoint or connect the VLANs. The Studio and
XIAO use the same test VLAN; the Mac mini remains outside it and uses USB for
device access. This work does not bridge the VLANs.
The user selected the password-protected `TestESPNetwork` for the XIAO. Enter its
password directly on the device during setup; do not store it here. The user
confirmed that this network is on the OpenClaw host's VLAN, so a device on it
can test the direct path without bridging VLANs.

Initial host inspection on 2026-10-12 found macOS, OpenClaw `2026.9.4`
(`3a9d69d`), port `18789`, and bind `loopback`. At that stage, both
`gateway.tls.enabled` and `gateway.http.endpoints.chatCompletions.enabled` were
valid but unset, leaving TLS and chat completions disabled by default. Chat
Completions has since been enabled through hot reload; Gateway TLS, loopback
binding, authentication, and sandbox configuration remain unchanged. Available agent IDs are `main`, `devlab`, and `android-trial`,
with Ollama configured behind the agents. The user selected `main` (Mingus), so
the device target is `openclaw/main`. Follow-up inspection confirmed gateway
authentication mode `token`, a valid but unset Control UI allowed-origins setting,
and IPv4 `192.168.21.143` on `en0` (no IPv6 network state). Effective sandbox/tool
policy was inspected below. The selected relay keeps this listener unchanged;
follow the [relay setup](../integrations/openclaw/relay/README.md) for the local
chat endpoint and separate HTTPS listener.

The [v2026.9.4 release](https://github.com/openclaw/openclaw/releases/tag/v2026.9.4)
matches the reported commit. For a direct connection, its custom IPv4 listener can bind the test VLAN
address while retaining IPv4 loopback access; non-loopback startup also requires
authentication and, with Control UI enabled, configured allowed origins. Preserve
existing origins. See the tagged [listener implementation](https://github.com/openclaw/openclaw/blob/v2026.9.4/src/gateway/net.ts)
and [startup validation](https://github.com/openclaw/openclaw/blob/v2026.9.4/src/gateway/server-runtime-config.ts).
TLS and listener edits require a concrete migration/restart plan for existing
clients. Use version-matched CLI guidance: newer documentation's stop-before-edit
requirement does not apply to this release.

Read-only network/authentication inspections completed on the Studio:

```sh
openclaw config get gateway.auth.mode
openclaw config get gateway.controlUi.allowedOrigins
scutil --nwi
```

Collect the authentication mode, allowed origins, and the IPv4 address/interface
on the test VLAN. Do not collect the token or password in chat or this repository.

### Preserve the existing agent sandbox

The user reports that Mingus has a carefully configured sandbox. Preserve that
configuration as an integration constraint. Start with text conversation only;
the device skill is optional. Inspect the current effective policy read-only:

```sh
openclaw sandbox explain --agent main
```

This report describes OpenClaw's sandbox policy, not any additional container,
VM, firewall, or host-level restrictions. The HTTP source sends only an agent
target, conversation ID, current user message, and streaming flag; it does not
send tool definitions, model overrides, or sandbox/elevated overrides. The API
conversation has its own session key, so its exact effective sandbox and tool
policy must also be inspected; this check passed for the live device session
below. The `main` agent ID does not mean the request uses its main session;
`non-main` sandbox mode depends on the session key. See the version-matched [sandbox modes](https://github.com/openclaw/openclaw/blob/v2026.9.4/docs/gateway/sandboxing/modes-scope-and-backend.md)
and [policy inspector](https://github.com/openclaw/openclaw/blob/v2026.9.4/docs/cli/sandbox.md).

Shared-secret authentication on this release's HTTP endpoint conveys full
operator authority and owner-sender semantics. Agent restrictions still apply,
but an existing channel/sender policy or creator-role sandbox requirement must
not be assumed to transfer to this ingress. Selecting `openclaw/main` in firmware
does not limit the credential to that agent. Assess this before placing a gateway
credential on the XIAO. If that authority exceeds the intended device access,
design a separately authenticated, narrowly constrained intermediary while
keeping the gateway local; it must enforce permitted routes, agent/session
selection, headers, and request fields. The optional
[HTTPS relay](../integrations/openclaw/relay/README.md) implements that boundary.
See the tagged [HTTP authentication contract](https://github.com/openclaw/openclaw/blob/v2026.9.4/docs/gateway/openai-http-api.md#security-boundary-important).

The optional Python device helper needs approved Python execution, readable
helper files, device credentials, and network reachability from its actual
execution environment to the XIAO. A skill cannot grant these permissions. If
the existing sandbox intentionally denies them, leave the skill unused and
design narrowly registered device tools through an explicitly approved broker;
do not enable broad execution, host networking, or elevated access as a shortcut.
The same separation applies to a future Pi/UART daemon that owns the serial port.

The user's effective-policy report confirms `main` uses Docker with `mode: all`,
`scope: agent`, and `workspaceAccess: none`; it has a separate sandbox workspace
and a protected read-only skills mount. The existing allowlist already contains
`hardwareone_camera`, `hardwareone_cli`, `hardwareone_devices`, and
`hardwareone_ping`. Prefer integrating with those existing tools and their device
configuration before installing the optional helper. Their implementation and
environment variable names have not been inspected. Elevated execution is
enabled globally but `allowedByConfig: false` for the report's unknown channel;
that result is not a guarantee for a different caller/session. Preserve the
existing sandbox, mounts, and tool policy.

The user requested a device account named `cm5` and the XIAO's IP after it joins
`TestESPNetwork`, for updating the existing tool environment on the Studio.
The account password is not recorded here. After the user supplied administrator
access, USB status verified `/dev/cu.usbmodem11201` as a Seeed XIAO ESP32S3 Sense,
firmware `0.99.97.4`, MAC `68:EE:8F:50:E9:D0`. The `cm5` account was created with
the requested password as an ordinary `user` (no forced password change), and
the account listing confirmed it. The administrator serial session was retained.
The user entered the Wi-Fi password in a masked Mac mini prompt. It was passed
in memory over USB and saved on the device, without writing the password to this
Mac or printing it in tool output. USB status then confirmed connection to
`TestESPNetwork` at `192.168.21.24`. The existing device HTTP server was started
and reported running on port `80`, so the current device base URL is
`http://192.168.21.24`. The previous saved network was retained. Wi-Fi is enabled
but `wifiAutoStart` remains `false`; reconnect after a device reboot. At this
initial provisioning stage, no firmware had been flashed and Gateway settings
were unchanged. Subsequent app-only updates and endpoint activation are recorded
below. No VLANs were bridged.
The account creation/listing and network/server status were verified through USB;
authentication and reachability were subsequently verified from the Studio below.
Do not use `/dev/cu.usbmodem2201`, which is occupied by the separate P4 work.
The user subsequently created a dedicated device account named `openclaw` with
its own credentials and updated the Studio's device configuration and address.
USB account listing confirms that account exists with the user-selected `admin`
role; its role and the older `cm5` account were left unchanged. Use `openclaw`
for the next authenticated device-tool test. The supplied host setup script
stores devices in `${HW1_DEVICES_FILE:-$HOME/.openclaw/hardwareone.devices.json}`;
it is a JSON device registry, not the optional helper's `HW1_*` configuration.
Neither its contents nor the new credentials have been read from the Studio.
A Studio-side authenticated read can use
`curl --user openclaw http://192.168.21.24/api/system`, which prompts for the
password. A successful `/api/ping` alone does not verify authentication.
Then test the existing `hardwareone_devices` tool from Mingus's sandbox. These
checks validate agent-to-device access; the new ESP-to-OpenClaw chat firmware
and gateway endpoint setup remain separate acceptance work.

Agent-to-device acceptance passed on 2026-10-12: the Studio's authenticated
`GET /api/system` returned the expected board, firmware `0.99.97.4`, SSID, IP, and
MAC. Mingus then used its existing `hardwareone_cli` tool to run `status json`
and reported the same live board/version/network details. Registry listing alone
was not counted as a live read. Preserve this working integration and sandbox.

For device-to-agent chat, the user selected a restricted HTTPS relay on the
Studio, with a separate device credential and the gateway kept on loopback.
The user has started the foreground relay at `https://192.168.21.143:18790/v1`,
fixed to `main`, and enabled Gateway Chat Completions through hot reload. The
relay is running as a foreground process; a persistent service installation has
not been verified. Its real device token was entered through a masked prompt,
its public CA was provisioned on the XIAO, and the device clock is correct.
Normal relay logs are silent. Both direct access and the relay use tokens:
direct access uses the gateway credential on the device; the relay uses its own
device token and keeps the gateway credential on the Studio. The relay limits
HTTP routes and agent selection, not Mingus's configured tool permissions.

The prototype uses the documented [OpenClaw chat-completions endpoint](https://docs.openclaw.ai/gateway/openai-http-api).
It must be enabled on the chosen test host. The configured target is an agent
alias such as `openclaw/default` or `openclaw/<agent-id>`, not its underlying
provider model. Check the installed version before modifying gateway settings.
CLI references: [configuration](https://docs.openclaw.ai/cli/config) and
[agents](https://docs.openclaw.ai/cli/agents).

## Build and device settings

Use the saved configuration on the `0.99.97.4` source baseline. After sourcing
the ESP-IDF environment, run:

```sh
python3 tools/build_config.py build xiao-openclaw
```

This test profile adds `ENABLE_LLM_SOURCE_HTTP=1` and explicitly sets
`ENABLE_LLM_SOURCE_ONBOARD=0`, keeping CM5 enabled. The user chose to omit onboard
inference while testing the remote agent so the image fits the existing app
partition. TLS trust/verification and all other baseline choices are retained;
shared feature headers are unchanged. The normal `xiao-default` build retains
the onboard source and keeps HTTP at `0`. Outputs and the compiler-resolved
`BUILD_INFO.md` live in `build-config-xiao-openclaw/`; this command does not flash.
The earlier `0.99.95` HTTP-only prototype is obsolete and must not be used to
update the XIAO running `0.99.97.4`.

On a device running that test image, configure these admin commands over its
authenticated settings/command interface. Substitute the test host and agent;
never paste a real token into this repository or a shared transcript.

| Device command | Purpose |
| --- | --- |
| `llmhttpurl https://HOST:PORT/v1` | Base URL, including `/v1`; no embedded credentials, query, or fragment |
| `llmhttpmodel openclaw/main` | Selected test agent, Mingus |
| `llmhttptoken TOKEN` | Relay **device token** for the selected relay path; use a gateway token only for a deliberate direct connection. Stored through the existing encrypted-secret settings path |
| `llmhttpca PEM` | Relay public CA certificate, required for the selected private-CA relay; use literal `\n` escapes for line breaks |
| `llmhttpenabled 1` | Make the configured source visible |
| `llmclear` | Clear old chat before changing between an agent and another source |
| `llmload http:openclaw` | Select the configured agent source |
| `llmstatus json` | Check readiness and `retrySafe:false` |
| `llmgenerate json Hello` | Start an asynchronous text conversation; HTTP-only builds require the `json` form |
| `llmresult json 0` | Read the streamed reply from offset zero |

The LLM feature must also be enabled in the ordinary app settings. Optional
startup uses `llmdefaultmodel http:openclaw` and `llmautostart 1`; selection waits
for Wi-Fi and valid configuration. No request is sent just by selecting a source.

Follow the [HTTPS relay setup](../integrations/openclaw/relay/README.md) for its
endpoint, separate device token, and public CA certificate. HTTPS verifies the
hostname and CA and requires a valid device clock. Check `time json`; use the
existing NTP setup or `timeset <unix_timestamp>` if needed. A build with no
certificate bundle requires `llmhttpca`; clearing that field does not bypass
verification. The URL limit is 384 bytes, model 127, token command 512, and CA storage 4096. Normal command ingress
limits still apply to a CA entered through the CLI. `-` clears a string setting.

For a deliberately chosen isolated plaintext bench only, `llmhttpplaintext 1`
allows an `http://HOST:PORT/v1` URL. It sends the configured bearer credential
without encryption; the selected HTTPS relay does not use this option. No
automatic TLS downgrade or redirect is followed. Direct gateway tokens can
authorize more than a single agent; select a test gateway and its tool policy
accordingly. Keep each test host/device pair within one network; do not bridge
the user's VLANs.

## Conversation behavior

- Clear starts a new host conversation ID and clears the display. Reboot or
  configuration changes also start a new ID. Host transcripts and agent memory
  are not deleted.
- OLED offers New chat in its X menu and Y:new in the model picker. G2 offers
  New chat in its menu and chat viewer. Use it before changing agent sources.
- Agent Retry is disabled in shared chat, the web UI, OLED, and G2. Requests are
  never automatically replayed, including after a disconnect or auth failure.
  A retained onboard guided menu is hidden and cannot submit/repeat through HTTP.
- Stop closes the local stream. Actions already started on the host may continue.
  Wait for the worker to finish before Clear or another selection.
- Replies use the existing 2048-byte display budget. A visible notice marks
  truncation or interruption, while preserving text already received. Prompts
  are limited to 2047 bytes. OpenClaw controls agent sampling and tool execution;
  onboard sampling settings are not forwarded.
- The endpoint exchanges text. G2 Ask Mic/Keys can transcribe locally before
  sending it. With local STT and chat compiled in (including `p4-openclaw`),
  native **Hey Even** also transcribes on the P4 and streams the selected
  source's answer into the native popup. The glasses must have a live paired
  user, both required temple connections, an available microphone and a ready
  selected AI source. Existing relay settings are reused. Choosing a CM5 model
  retains the UART-host flow. Speech playback is unchanged.
- Native local questions end after a 1.5-second speech pause. Long speech can
  span STT segments; capture fails after 30 seconds, or if the complete prompt
  exceeds 1024 bytes, instead of sending a shortened request. Dismissal cancels
  that exact speech/chat run. There is no automatic retry. `g2evenai status`
  reports the native provider and exchange; see [qualification notes](G2_EVENAI_LOCAL_WORK.md).
- One shared device chat is used, as before. This prototype does not introduce
  separate per-account OpenClaw sessions.

The worker uses a 2-second transport I/O timeout, a 60-second idle/header window,
and a 5-minute total request limit. Platform DNS/connect/TLS work can affect how
quickly a cancellation completes before a socket is available.

## Verification and remaining work

Host tests exercise the production SSE parser, HTTP worker, backend dispatch,
chat clear/retry handling, configuration gates, and credential handling. The
firmware host tests use `clang++` and the repository's bundled ArduinoJson. Run:

```sh
python3 components/hardwareone/test/host/test_llm_http.py --sanitize
python3 components/hardwareone/test/host/test_llm_http_runtime.py --sanitize
python3 components/hardwareone/test/host/test_llm_http_integration.py --sanitize
python3 components/hardwareone/test/host/test_llm_http_settings.py --sanitize
python3 components/hardwareone/test/host/test_llm_guided_source.py --sanitize
python3 -m unittest discover -s integrations/openclaw/tests -v
```

Validation on 2026-10-12 against the current firmware baseline:

- Nine selected firmware host suites passed: the five HTTP suites above with
  ASan/UBSan, G2 conversation, saved build configuration, build-configuration
  workflow, and shared settings mutation. The settings test uses the production
  validation macro and verifies that validation-only token writes and clears do
  not mutate settings; repeated clears do not save or emit another change event.
- All 45 web tests passed, including 68 extracted CLI checks.
- All 23 optional device-helper tests passed, including loopback TLS trust and
  hostname verification. The relay passed 29 tests and its private setup helper
  passed 11 tests. These local tests do not establish live Studio/device acceptance.

The firmware was ported to source baseline `f4cc542c` (`0.99.97.4`, matching the
installed version). The initial qualifying source/profile revision was
`04886ed7`; this guide was the only uncommitted file at that build stage.
Follow-up revision `236589c9` adds a one-line initialization fix so Clear works
on a cold start before generation. Its ASan/UBSan integration checks passed
three source variants and seven feature gates, and its profile rebuild
retains the same image size and headroom. Both initial full builds used the normal
repository build path with local ESP-IDF `v5.5.5-dirty`, Xtensa GCC `14.2.0`, and
the required local Arduino patches. No additional warning demotions or shared
feature/header edits were needed.

| Saved configuration | Answer sources | Result | Image bytes | App space remaining |
| --- | --- | --- | ---: | ---: |
| `xiao-default` | Onboard + CM5; HTTP off | Full build and partition check passed | 5,908,640 | 75,616 (1.3%) |
| `xiao-openclaw` | HTTP + CM5; onboard off | Full build and partition check passed | 5,936,096 | 48,160 (0.8%) |

The initial attempt with onboard + CM5 + HTTP compiled and linked, but its
6,027,344-byte image exceeded the existing partition by 43,088 bytes. The user
then authorized omitting onboard inference for this test profile. No other
feature was disabled. The final profiles differ in exactly two of 139 resolved
feature values: HTTP and onboard-source selection. All 2,475 SDK values match,
including the full certificate bundle and TLS settings.

Both use the same 5,984,256-byte factory app partition at `0x10000` and the
unchanged filesystem layout. The partition-table SHA-256 is
`c6ed2513218eaa1d5b591914aea7ce95715500cb49068ed13b891ce5fa7efc71`.
Image SHA-256 values by revision:

- Initial `xiao-openclaw`, `04886ed7`:
  `8d91298c0a16530b85497f8a8343bb09c234ec686aedc34cdd9081eb1751503c`
- Current `xiao-openclaw`, `236589c9` (`build-config-xiao-openclaw/hardwareone-idf.bin`):
  `7f9d28179b3875add82746f0c0b5849b95fe5681199f7834c4473735f1758d7e`
- Initial comparison `xiao-default` (`build-config-xiao-default/hardwareone-idf.bin`):
  `d68d1e24de9251a2bb214ea242b0c553ab608030193723b207a3a6811720c648`

Each build directory contains its generated `BUILD_INFO.md`, saved input
snapshot, and compiler-resolved flags. Build logs are under
`work/openclaw-build/`: `xiao-default-build.log` and
`xiao-openclaw-test-profile-build.log` record the successful initial profile
runs. Earlier attempt logs are retained for diagnosis; the older `0.99.95` HTTP-only image is
obsolete. Recheck partition fit before adding features to this narrow headroom.

## Hardware acceptance

- Initial image `04886ed7` passed app-only flashing and verification. The first
  device chat received valid SSE containing `No response from OpenClaw.` This
  is the Gateway's fallback text, confirmed in tagged
  [OpenClaw 2026.9.4 source](https://github.com/openclaw/openclaw/blob/v2026.9.4/src/gateway/openai-http.ts#L1112-L1149).
  Transport reached the Gateway, but an actual Mingus reply was not established.
- Follow-up image `236589c9` passed app-only flashing and a separate
  `verify_flash` digest comparison. An additional esptool reset was needed
  before the console returned; the subsequent boot was healthy. All five stored
  secrets decrypted successfully. With user `a` logged in, cold-start `llmclear`
  returned `ok:true` before any generation request.
- Latest XIAO checks: `TestESPNetwork`, `192.168.21.24`, HTTP port `80`, correct
  time, preserved masked token and public CA, zero crashes, about 52 KB free
  heap and 6,672 KB free PSRAM. Clock verified at `2026-10-13T01:36:30Z`;
  Wi-Fi autostart remains off.
- Device-to-agent text acceptance passed on `236589c9`: a request through the
  real HTTPS relay returned the 16-byte reply `HW1 relay ready.`, `done:true`,
  final state `READY`, an empty error, and `retrySafe:false`. The earlier
  fallback was not reproduced by this successful request.
- Conversation continuity passed: Mingus acknowledged an ephemeral test phrase
  with `noted.` and recalled it correctly on the next turn. New chat separation
  passed: `llmclear` returned `ok:true`, and the next conversation returned
  `no test phrase` when asked about the prior phrase. These turns all finished
  `READY` with an empty error and `retrySafe:false`; crash count stayed zero
  and free heap/PSRAM remained about 52 KB/6,672 KB.
- A Studio-local direct Gateway probe returned `HW1 relay ready` and final
  `[DONE]` in the diagnostic session ending `0002`. This local check preceded
  the successful device requests above.
- Exact HTTP-session policy inspection passed. The Studio CLI selected the
  newest actual device session:
  `agent:main:openai-user:hw1:xiao-studio:d6624f0093f968b9ebd797f68bb8487b`.
  Its report identifies agent `main`, main session `agent:main:main`, and a
  `sandboxed` runtime: Docker, `mode: all`, `scope: agent`,
  `workspaceAccess: none`, runtime workdir `/workspace`, and a protected
  read-only skills mount. The global tool allowlist matches the initial report;
  `memory_search` and `memory_get` remain denied. Elevated execution is globally
  enabled, but the inspector reports channel `unknown` and
  `allowedByConfig: false`. Source review of v2026.9.4
  `sandbox-explain.ts:253-268` shows that an unknown channel forces this boolean
  false; it does not establish that elevation is denied for the actual HTTP
  caller. The reported Docker configuration/tool list is preserved, but strict
  host-execution isolation remains unverified. Exec routing/session overrides,
  elevated allowlists, and custom tool implementation boundaries require a
  separate read-only Studio inspection. No sandbox/elevation/exec settings were
  changed, and no host-execution audit was performed.
- OpenClaw UI visibility remains pending: the user cannot yet find the new
  device conversation. Its actual session was found through the CLI; the
  direct UI link and visibility are being checked separately from text,
  continuity, New chat, and reported session-policy acceptance.

The pre-test app-only rollback image is in the primary checkout at
`/Users/cadbecaimacmini/Documents/Codex/Projects/hardwareone/work/openclaw-usb-backup/20261013T011617Z/factory-before.bin`,
5,984,256 bytes, SHA-256
`d1b623c177ce367cc09405df4a0dd9586f167859889f066c4fa400899790fe9f`.
Both updates preserve the existing bootloader, partition table, NVS, LittleFS,
users, and network settings. Continue using only the validated application
region for updates or rollback. App rollback does not revert runtime data
changes made after boot. Ordinary flash arguments also write the bootloader and
partition table, and `littlefs-flash` writes a blank filesystem; neither is the
intended update path. Confirm `/dev/cu.usbmodem11201` and target MAC
`68:EE:8F:50:E9:D0` before further device access. Credentials remain private.

Before porting HTTP support to the P4, preserve the actual active STT working
tree at `/Users/cadbecaimacmini/Documents/ChatGPT/HardwareOne-stt-speedups`:
branch `stt-speedups`, HEAD `2e56063`, 34 modified tracked files plus untracked
STT work, profile `p4-glasses-test`, model `quartznet15x5gen2`. Do not substitute a
clean `f4cc542c` baseline for that work. P4 USB `/dev/cu.usbmodem2201` remains
outside this test.

UART OpenClaw is planned in [the integration plan](OPENCLAW_INTEGRATION_PLAN.md).
It needs a Pi daemon provider plus conversation/capability negotiation and narrow
registered device tools. The current CM5 UART protocol is unchanged.
