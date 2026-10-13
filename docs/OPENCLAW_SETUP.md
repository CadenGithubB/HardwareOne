# OpenClaw Wi-Fi prototype

Hardware One can select `http:openclaw` as an optional answer source for its
existing chat UI. The ESP sends the user's current message and displays streamed
text; the agent, conversation history, and tools run on the OpenClaw host.
The optional [device skill](../integrations/openclaw/README.md) lets that host
read existing device APIs. Conversation does not require the skill.

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
commands; they do not enable an endpoint or connect the VLANs. The existing host
is on another VLAN, and this work does not bridge it to the Mac mini or XIAO.
The user selected the password-protected `TestESPNetwork` for the XIAO. Enter its
password directly on the device during setup; do not store it here. The user
confirmed that this network is on the OpenClaw host's VLAN, so a device on it
can test the direct path without bridging VLANs.

Host inspection supplied on 2026-10-12: macOS, OpenClaw `2026.9.4` (`3a9d69d`),
port `18789`, bind `loopback`. Both `gateway.tls.enabled` and
`gateway.http.endpoints.chatCompletions.enabled` are valid but unset. The current
release defaults leave TLS and chat completions disabled; this is not yet a
live endpoint probe. Available agent IDs are `main`, `devlab`, and `android-trial`,
with Ollama configured behind the agents. The user selected `main` (Mingus), so
the device target will be `openclaw/main`. Follow-up inspection confirmed gateway
authentication mode `token`, a valid but unset Control UI allowed-origins setting,
and IPv4 `192.168.21.143` on `en0` (no IPv6 network state). Effective sandbox/tool
policy and the listener/TLS migration plan are still pending. Do not change the
listener or restart the host service until the concrete setup is determined.

The [v2026.9.4 release](https://github.com/openclaw/openclaw/releases/tag/v2026.9.4)
matches the reported commit. Its custom IPv4 listener can bind the test VLAN
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
send tool definitions, model overrides, or sandbox/elevated overrides. Nevertheless,
the API conversation has its own session key. Before live acceptance, inspect
that exact session's effective sandbox and tool policy too. The `main` agent ID
does not mean the request uses its main session; `non-main` sandbox mode depends
on the session key. See the version-matched [sandbox modes](https://github.com/openclaw/openclaw/blob/v2026.9.4/docs/gateway/sandboxing/modes-scope-and-backend.md)
and [policy inspector](https://github.com/openclaw/openclaw/blob/v2026.9.4/docs/cli/sandbox.md).

Shared-secret authentication on this release's HTTP endpoint conveys full
operator authority and owner-sender semantics. Agent restrictions still apply,
but an existing channel/sender policy or creator-role sandbox requirement must
not be assumed to transfer to this ingress. Selecting `openclaw/main` in firmware
does not limit the credential to that agent. Assess this before placing a gateway
credential on the XIAO. If that authority exceeds the intended device access,
design a separately authenticated, narrowly constrained intermediary while
keeping the gateway local; it must enforce permitted routes, agent/session
selection, headers, and request fields. That intermediary is not implemented.
See the tagged [HTTP authentication contract](https://github.com/openclaw/openclaw/blob/v2026.9.4/docs/gateway/openai-http-api.md#security-boundary-important).

The optional Python device helper needs approved Python execution, readable
helper files, device credentials, and network reachability from its actual
execution environment to the XIAO. A skill cannot grant these permissions. If
the existing sandbox intentionally denies them, leave the skill unused and
design narrowly registered device tools through an explicitly approved broker;
do not enable broad execution, host networking, or elevated access as a shortcut.
The same separation applies to a future Pi/UART daemon that owns the serial port.

The prototype uses the documented [OpenClaw chat-completions endpoint](https://docs.openclaw.ai/gateway/openai-http-api).
It must be enabled on the chosen test host. The configured target is an agent
alias such as `openclaw/default` or `openclaw/<agent-id>`, not its underlying
provider model. Check the installed version before modifying gateway settings.
CLI references: [configuration](https://docs.openclaw.ai/cli/config) and
[agents](https://docs.openclaw.ai/cli/agents).

## Build and device settings

Set the literal `ENABLE_LLM_SOURCE_HTTP` to `1` in
`components/hardwareone/System_BuildConfig.h` for a test build. Keep
`ENABLE_LLM_BACKEND` and `ENABLE_WIFI` enabled. For the qualified XIAO test image,
set both `ENABLE_LLM_SOURCE_ONBOARD` and `ENABLE_LLM_SOURCE_CM5` to `0` (HTTP only).
Other sources can coexist in the registry, but the mixed image's final flash fit
has not been verified. The normal checked-in build keeps HTTP at `0`.

On a device running that test image, configure these admin commands over its
authenticated settings/command interface. Substitute the test host and agent;
never paste a real token into this repository or a shared transcript.

| Device command | Purpose |
| --- | --- |
| `llmhttpurl https://HOST:PORT/v1` | Base URL, including `/v1`; no embedded credentials, query, or fragment |
| `llmhttpmodel openclaw/main` | Selected test agent, Mingus |
| `llmhttptoken TOKEN` | Gateway bearer credential; stored using the existing encrypted-secret settings path |
| `llmhttpca PEM` | Optional private CA certificate, with literal `\n` escapes for line breaks |
| `llmhttpenabled 1` | Make the configured source visible |
| `llmclear` | Clear old chat before changing between an agent and another source |
| `llmload http:openclaw` | Select the configured agent source |
| `llmstatus json` | Check readiness and `retrySafe:false` |
| `llmgenerate Hello` | Start a text conversation |
| `llmresult json 0` | Read the streamed reply from offset zero |

The LLM feature must also be enabled in the ordinary app settings. Optional
startup uses `llmdefaultmodel http:openclaw` and `llmautostart 1`; selection waits
for Wi-Fi and valid configuration. No request is sent just by selecting a source.

HTTPS verifies the hostname and CA. A build with no certificate bundle requires
`llmhttpca`; clearing that field does not bypass verification. The URL limit is
384 bytes, model 127, token command 512, and CA storage 4096. Normal command ingress
limits still apply to a CA entered through the CLI. `-` clears a string setting.

For a deliberately chosen isolated plaintext bench only, `llmhttpplaintext 1`
allows an `http://HOST:PORT/v1` URL. It sends the gateway credential without
encryption. No automatic TLS downgrade or redirect is followed. Gateway tokens
can authorize more than a single agent; select a test gateway and its tool policy
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
- Only text is supported. The ordinary G2 Ask Mic/Keys path can use this source;
  the separate native Hey Even CM5 flow and speech playback are unchanged.
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

Validation on 2026-10-12: six focused host suites passed with ASan/UBSan (the five
above plus the existing G2 conversation regression); all 45 web tests passed,
including 68 extracted CLI checks; all 23 device-helper tests passed, including
loopback TLS trust and hostname verification. The OLED/G2 clear helpers also
passed extracted production-code sanitizer checks.

Firmware validation used ESP-IDF 5.5.5 and the Xtensa GCC 14 toolchain in isolated
source copies, with final production source hashes checked against the branch:

| Configuration | Result |
| --- | --- |
| XIAO HTTP only | Full compile, link, binary and partition checks passed |
| Mixed onboard + CM5 + HTTP | 11 affected translation units compiled; no full link/flash-fit check |
| HTTP disabled | 10 affected translation units compiled; symbol check found no unresolved `httpLlm` references; no full link |

The HTTP-only binary is 5,845,520 bytes in a 5,984,256-byte app partition, leaving
138,736 bytes (2.3%). Treat that as limited headroom when enabling more features.
The full build required demoting existing GCC `stringop-truncation` warnings in
the copied firmware and vendor targets. No other warning class was demoted and
no production build flags were changed. Focused source checks retained strict
flags except the existing G2 address-copy warning. XIAO's board policy excludes
OLED; OLED validation here was the extracted helper check, not a full OLED build.

Live gateway compatibility, skill discovery, device memory use, and actual Wi-Fi
streaming remain hardware acceptance checks. No device was flashed for this
implementation. Two Espressif USB devices were present during preparation; confirm
the XIAO's port before flashing. Its Wi-Fi password is not recorded in this project.

UART OpenClaw is planned in [the integration plan](OPENCLAW_INTEGRATION_PLAN.md).
It needs a Pi daemon provider plus conversation/capability negotiation and narrow
registered device tools. The current CM5 UART protocol is unchanged.
