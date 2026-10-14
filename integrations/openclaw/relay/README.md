# Hardware One HTTPS chat relay

This optional service runs on the same host as OpenClaw. It accepts one device's
chat requests over HTTPS and forwards them to the local gateway's
`POST /v1/chat/completions`. OpenClaw can retain its loopback listener and existing
sandbox. No firmware-specific protocol is added to OpenClaw.

```text
XIAO --HTTPS + device token--> relay --loopback + gateway token--> Mingus
```

There are three separate credentials in this setup:

| Credential | Used by | Purpose |
| --- | --- | --- |
| Existing device user `openclaw` and its password | Mingus's existing Hardware One tools | Read or operate the XIAO, as already tested |
| Newly generated relay device token | XIAO's HTTP chat source | Converse with Mingus through this relay |
| Existing OpenClaw gateway token | Relay, retained on Studio | Authenticate to the local gateway |

This is a transport boundary, not a new agent permission system. Device token
holders can send user prompts to Mingus and request actions allowed by Mingus's
existing policies. The relay restricts HTTP routes, request fields, agent
selection, and conversation namespaces. It does not make those prompts read-only
or override sandbox/tool policies. The upstream token remains a full operator
credential; see the [version-matched HTTP contract](https://github.com/openclaw/openclaw/blob/v2026.9.4/docs/gateway/openai-http-api.md).

The relay adds a fixed system note identifying requests as direct, interactive
device chat, with concise visible replies expected even for greetings. This
addresses a P4 voice turn where Mingus returned `NO_REPLY`, which OpenClaw
suppressed and replaced with `No response from OpenClaw.`. The device's original
words remain a separate user message; device-supplied system messages are still
rejected. This is request-scoped model guidance, not a guarantee of a reply or a
change to the agent's global instructions, tools, or sandbox. Verify an ordinary
greeting and follow-up on the live model after installation.

## Prepare on the Studio

Requires Python 3.9+ and OpenSSL/LibreSSL; no Python packages are installed.
Copy this directory to the Studio, then run from that directory:

```sh
python3 setup_relay.py --bind 192.168.21.143
```

Enter the existing OpenClaw gateway token at the masked prompt. Alternatively,
`--gateway-token-file /absolute/private/path` reads an existing owner-only file.
This is the gateway token, not the XIAO account password. The setup command:

- Creates `~/.openclaw/hardwareone-relay` with mode `0700`; refuses an existing path.
- Generates a private CA and a server certificate for the explicit IP address.
- Generates a separate random device token; token/key files are owner-only.
- Copies the relay code and prepares configuration plus a macOS LaunchAgent plist.
- Validates the files locally, without changing OpenClaw or starting any service.

The server certificate lasts 365 days. Keep the CA private key on the Studio for
certificate renewal. A changed Studio IP needs a matching certificate and device
URL; there is no fallback to listening on all interfaces. The generated plist
uses the Python executable that ran setup; keep that interpreter installed.

Preflight can be repeated without starting a listener:

```sh
python3 "$HOME/.openclaw/hardwareone-relay/relay.py" \
  --config "$HOME/.openclaw/hardwareone-relay/config.json" --check-config
```

## Enable the local API and test the relay

For the verified OpenClaw 2026.9.4 installation, enable only the optional chat
endpoint; keep `gateway.bind=loopback` and the existing authentication, TLS,
origins, sandbox, and tool configuration:

```sh
openclaw config set gateway.http.endpoints.chatCompletions.enabled true
openclaw config get gateway.http.endpoints.chatCompletions.enabled
```

Endpoint configuration supports hot reload in this release. Check the running
service before deciding whether a restart is necessary; do not blindly apply
newer releases' stop/edit/restart procedures to this installation.

Start the relay in the foreground for the first test:

```sh
python3 "$HOME/.openclaw/hardwareone-relay/relay.py" \
  --config "$HOME/.openclaw/hardwareone-relay/config.json"
```

From a second Studio terminal, send a small test request. The token enters curl
through its configuration input, rather than as a command-line argument:

```sh
set +x
relay_dir="$HOME/.openclaw/hardwareone-relay"
printf 'header = "Authorization: Bearer %s"\n' "$(cat "$relay_dir/device-token.txt")" |
  curl --config - --fail --show-error --no-buffer \
    --connect-timeout 5 --max-time 90 \
    --cacert "$relay_dir/ca-cert.pem" \
    --header 'Content-Type: application/json' \
    --data '{"model":"openclaw/main","user":"hw1:00000000000000000000000000000001","stream":true,"messages":[{"role":"user","content":"Reply exactly: HW1 relay ready. Do not use any tools."}]}' \
    https://192.168.21.143:18790/v1/chat/completions
```

Expect SSE text deltas followed by `data: [DONE]`. Repeating this smoke test uses
the same dedicated test conversation. Normal firmware generates a random
conversation ID. Inspect the resulting OpenClaw API session's effective policy
before hardware acceptance; HTTP and existing messaging sessions are distinct.

## Device settings

Once the compatible HTTP-enabled firmware has been built and flashed:

| Setting | Value |
| --- | --- |
| `llmhttpurl` | `https://192.168.21.143:18790/v1` |
| `llmhttpmodel` | `openclaw/main` |
| `llmhttptoken` | Contents of the private **device-token.txt**, entered privately |
| `llmhttpca` | Public **ca-cert.pem**, with literal `\n` escapes when using the CLI |
| `llmhttpplaintext` | `0` |
| `llmhttpenabled` | `1` |

Use `llmclear`, `llmload http:openclaw`, and `llmgenerate json <prompt>`;
read the asynchronous reply with `llmresult json 0`. The plain synchronous
`llmgenerate <prompt>` form is unavailable in HTTP-only builds.
The ESP must have a valid clock for certificate verification. Check `time json`;
use existing NTP or an explicit `timeset <unix_timestamp>` if needed. Do not
disable certificate verification to work around an unset clock.

Only the public CA certificate needs to be shared for device setup. Keep both
token files, the CA private key, and the server private key on the appropriate
host/device credential paths, out of Git and shared transcripts.

## Optional macOS background service

After foreground testing succeeds, stop it with Ctrl-C. Install the generated
plist without overwriting an existing LaunchAgent:

```sh
mkdir -p "$HOME/Library/LaunchAgents"
ln -s "$HOME/.openclaw/hardwareone-relay/launchagent.plist" \
  "$HOME/Library/LaunchAgents/ai.openclaw.hardwareone-relay.plist"
launchctl bootstrap "gui/$(id -u)" \
  "$HOME/Library/LaunchAgents/ai.openclaw.hardwareone-relay.plist"
```

To stop it, use `launchctl bootout` with that same domain and plist path. The
LaunchAgent runs while this user is logged in, not as a machine-wide daemon.
Its log contains service status and fixed failure stages/categories, not chat
bodies, raw exception messages or credentials. For example,
`relay: upstream_stream failed (deadline)` means the total turn deadline expired;
`upstream_stream failed (UpstreamError)` means the gateway stream was invalid,
reported an error, ended early or exceeded a size bound. Check that session on
the gateway before submitting it again: agent actions may already have run.
A config/credential change requires restarting the relay.

## Update an existing relay

Copy the updated relay directory to the Studio. Stop the foreground relay with
Ctrl-C in its Terminal, then run the following from the new source directory:

```sh
python3 relay.py --config "$HOME/.openclaw/hardwareone-relay/config.json" --check-config &&
  cp relay.py "$HOME/.openclaw/hardwareone-relay/relay.py" &&
  python3 "$HOME/.openclaw/hardwareone-relay/relay.py" --config "$HOME/.openclaw/hardwareone-relay/config.json"
```

Keep this Terminal open for foreground use. If using the optional LaunchAgent,
restart that service instead of starting a second foreground copy. Updating
`relay.py` preserves the installed configuration, tokens and certificates. Do
not rerun `setup_relay.py` or reflash the ESP for this update. Existing device
conversation IDs continue to select the same sessions.

## Bounds and verification

One agent request is forwarded at a time; other requests receive `503`. There
are at most four TLS/request workers. Prompts are limited to 2047 UTF-8 bytes,
SSE events to 8192 bytes, total upstream output to 1 MiB, and a turn to five
minutes. A local model may load or think silently before response headers or
between streamed tokens; those upstream reads can wait until the same five-minute
deadline. They no longer fail after 30 idle seconds. Upstream connect/request
writes retain their 30-second bound. Incoming header/body, TLS handshake, and
downstream write deadlines are unchanged. Device-side timeouts can still end a
turn sooner. No redirect, proxy environment, or automatic retry is used. Closing
a connection cannot undo agent actions already performed.

Run local tests from the repository root:

```sh
python3 -B integrations/openclaw/tests/test_relay.py
python3 -B integrations/openclaw/tests/test_relay_setup.py
```

They use temporary loopback servers, temporary certificates, and fake gateway
credentials. They do not contact the Studio gateway or attached hardware.

## Native Terminal sessions

The optional `/v1/terminal` route connects the P4 to the computer-side official
Even Terminal server with the Hardware One OpenClaw provider. It supports the
bounded session browser/history/event/action contract in
[`../terminal/DEVICE_API.md`](../terminal/DEVICE_API.md). The existing chat route
continues to work. Terminal remains disabled until both optional configuration
fields are set:

```json
{
  "terminal_port": 3457,
  "terminal_token_file": "/absolute/private/path/to/bridge-token.txt"
}
```

Use the Terminal setup helper to add these fields while preserving existing
relay settings. The token file must be a private regular file owned by the relay
user, distinct from both the device token and Gateway token. The relay connects
only to `127.0.0.1` and the fixed `/api/hardwareone` route; the device cannot supply
a host, route, provider, agent, or Gateway RPC method. It injects the configured
device identity rather than trusting one in a request. Both request and response
bodies are limited to 4096 encoded bytes. Terminal requests have a 30-second hard
deadline and are never automatically retried. Mutations carry stable request IDs
so a lost response cannot silently become a second prompt.

The user has enabled browsing and resuming existing `main` / Mingus sessions.
This is broader than the older dedicated Hardware One chat session: a holder of
the device token can read those sessions and send prompts to a selected session.
Gateway approval/question capability still depends on the host bridge's granted
scopes and paired identity. No sandbox policy or elevated-execution setting is
changed by enabling this route. Oversized or private pending requests require
the OpenClaw dashboard rather than an incomplete approval on the glasses.
