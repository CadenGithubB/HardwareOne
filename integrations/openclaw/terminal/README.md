# Hardware One provider for Even Terminal

This directory preserves the earlier integration snapshot. The maintained
first-party host, relay, launcher and conversation skill are now in
[HardwareOne G2 Terminal Bridge](https://github.com/CadenGithubB/HardwareOne_G2_Terminal_Bridge).
Use that repository for current installation and updates; it does not require
the Even Terminal computer server. The instructions below describe the legacy
integration retained here for reference.

This extends the official **Even Terminal 0.10.5** computer server with an
OpenClaw provider. The P4 reaches it through the existing restricted HTTPS
relay; the server connects only to OpenClaw on `127.0.0.1`. A phone, Even
account, public tunnel and cloud bridge are not part of this path.

The Gateway contract is pinned to the installed **OpenClaw 2026.9.4**, commit
`3a9d69db306cd7f081e06254cb89c4bcc14a7107`, protocol 4. These are verified
versions, not a claim about the newest release. An unsupported Gateway version
fails closed instead of guessing its API.

## What is implemented

| Official bridge feature | OpenClaw mapping and limits |
|---|---|
| Browse and resume | Existing, unarchived `main` / Mingus sessions; opaque handles retain the original Gateway identity. Choosing a session does not send a message. |
| New conversation | Reserves a new `agent:main:openai-user:hw1:terminal:…` session; the first explicit prompt starts it. |
| History | Reads Gateway `chat.history`; the host does not invent a separate transcript. It requests ten recent entries, then keeps their user/assistant text, so the device can show fewer than ten messages. |
| Live response | Actual chat deltas and final text, reconciled with history. Empty / `NO_REPLY` completion is reported as no visible reply. |
| Tool activity | Actual start/update/result events, tool identity, reported error status and text output. Output previews are limited to 4 KiB and marked when truncated. `ask_user`, including its deferred `tool_call` wrapper, shows a concise question status (real failures remain marked); its raw answer result/JSON stays off the glasses. Attachments and non-text result objects remain in the Gateway UI. |
| Activity and progress | Observed thinking/tool/response activity drives native status. Explicit Gateway plan steps produce completed/total task progress and the current step, including in-flight replay. No tasks are inferred from tool calls. |
| Run statistics | Gateway-reported cumulative output tokens and elapsed time from known run timing. Timing refreshes on event/poll activity, at most every ten seconds between authoritative updates. Unknown input-token counts, costs and model-call/turn counts are omitted. |
| Interrupt | Exact selected run via `chat.abort`; the UI must wait for the outcome. Recovered runs requiring session-wide abort are directed to the OpenClaw dashboard. |
| Approvals | The Gateway's offered decisions only, re-read immediately before resolution. Complete critical details must fit the device UI; otherwise approval stays on the dashboard. |
| Questions | Offered options, supported multiple selections, free text and cancellation. Accepted answers appear as readable answer items, including selected labels or spoken text. Secret-store/password/URL questions stay on the dashboard. |
| Reconnect | A new instance identity invalidates old device views. An interrupted connection is an uncertain outcome, never an automatic prompt resend. |

Progress and statistics use stable text items in the native scrolling view;
the native protocol has no numeric statistics panel or image payload. Reasoning
text is not copied onto the glasses; only its observed activity is shown. Some
channel-originated runs do not expose live plan/usage events through Gateway;
missing values stay absent. The fixed `main` provider does not emulate other
providers' shell sessions, account/cwd controls or unoffered permissions.
Answer items are emitted after the complete question request is accepted;
unfinished panels, cancellation and failed submission do not create an accepted
answer. Up to eight recent answer receipts can be replayed from the existing
bounded session event buffer when reopening Terminal, where the initial view
has room; history and pending controls take priority. This buffer lasts only
for the running host; it does not add a separate persistent transcript or change
Gateway history. Replayed receipts retain stable content IDs within each view.
It uses `deliver:false` for the chat
response and rejects slash/bang-prefixed commands and the pinned Gateway's bare
cancellation aliases locally; use the explicit Interrupt action to stop a run.
Ordinary text is forwarded unchanged. The Gateway's
`suppressCommandInterpretation` field requires admin scope and is not sent.
It does not change
Mingus's sandbox, tool allowlist, models, files or channel configuration.
Approving a real Gateway request still has the effect described by that request.
This follows the pinned Gateway's [send admission rule](https://github.com/openclaw/openclaw/blob/3a9d69db306cd7f081e06254cb89c4bcc14a7107/src/gateway/server-methods/chat-send-request.ts)
and [cancellation vocabulary](https://github.com/openclaw/openclaw/blob/3a9d69db306cd7f081e06254cb89c4bcc14a7107/src/auto-reply/reply/abort-primitives.ts).

## Install on the Mac Studio

Keep `terminal/` and `relay/` beside one another in the copied kit. In a Terminal
opened in this `terminal` directory, run:

```sh
node --version
python3 install.py
node "$HOME/.local/share/hardwareone-even-terminal/start.mjs" --init
```

Node.js **22 or newer**, npm and Python 3 are required. The installer verifies
the official package's pinned SHA-512 integrity and the exact files it patches.
It installs only the three runtime dependencies needed by the retained server,
with npm lifecycle scripts, audit, funding and update notifications disabled.
It saves the resulting lockfile and provenance. Failed installs remove their
own staging directory; an existing installation is never overwritten. No
service is started by installation or initialization.

Private files are created under
`~/.local/share/hardwareone-even-terminal/`:

- `hardwareone.json`: ports and absolute credential-file paths.
- `gateway-identity.pem`: persistent Ed25519 identity used for normal pairing.
- `bridge-token.txt`: separate local host credential, read by the relay.

The Gateway token defaults to the already-saved
`~/.openclaw/hardwareone-relay/gateway-token.txt`. Leave tokens and private keys
in these private files; do not paste them into commands, chat or this repository.
The existing relay device token and CA are retained.

For a different install location, use `python3 install.py --directory PATH` and
use that absolute path in the commands below. `--tarball PATH` accepts an
already-downloaded official archive with the same mandatory integrity check.

## Update an existing Hardware One host

Stop the Terminal host with Ctrl-C, leave the relay running, and run these from
the new kit's `terminal/` directory (or the small host-update folder):

```sh
python3 update.py --check && python3 update.py
node "$HOME/.local/share/hardwareone-even-terminal/start.mjs"
```

This verifies the installed module hashes against their saved provenance,
backs up the changed modules, and replaces only the Hardware One extensions.
It preserves the configuration, paired identity, tokens and installed runtime
dependencies. No reinstall, `--init`, relay restart or P4 flash is needed.
Close and reopen Terminal in the glasses after restarting the host.

## Pair the host through OpenClaw

Run the private connection check:

```sh
node "$HOME/.local/share/hardwareone-even-terminal/start.mjs" --check
```

It prints the **public device ID**, then either the granted scopes or a
connection error. If pairing is required, run these on the Studio:

```sh
openclaw devices list
```

Find **Hardware One Terminal**, compare its device ID with the check output,
and review its requested `operator.read`, `operator.write`,
`operator.approvals` and `operator.questions` scopes. Approve that exact pending
request using the request ID shown in the list:

```sh
openclaw devices approve REQUEST_ID
node "$HOME/.local/share/hardwareone-even-terminal/start.mjs" --check
```

Replace `REQUEST_ID` with that request's ID. This is the normal Gateway pairing
flow, not a sandbox exemption or `operator.admin` grant. A missing private
identity is an error when approvals are enabled. The check does not list chats,
send prompts, approve tool requests or modify Gateway configuration.

The pinned [OpenClaw devices reference](https://github.com/openclaw/openclaw/blob/3a9d69db306cd7f081e06254cb89c4bcc14a7107/docs/cli/devices.md)
describes the pairing commands. If the request has expired or changed, list
again and inspect the current request instead of approving an unrelated one.

## Connect the existing relay and start

From the copied kit's `terminal` directory:

```sh
python3 enable_terminal.py --check
python3 enable_terminal.py
node "$HOME/.local/share/hardwareone-even-terminal/start.mjs"
```

The helper checks both configurations, saves private backups and updates the
installed relay plus its local host port/token-file path. It starts nothing and
preserves the device token, Gateway token, TLS certificate and keys. Leave the
host running. In the Terminal running the old relay, stop it with **Control-C**,
then restart it:

```sh
python3 "$HOME/.openclaw/hardwareone-relay/relay.py" --config "$HOME/.openclaw/hardwareone-relay/config.json"
```

The host binds to `127.0.0.1:3457`; the P4 continues using the relay's existing
HTTPS address and device credential. It does not connect to port 3457 directly.
The old `/v1/chat/completions` path remains available for the existing chat app.
The native Terminal path uses the bounded [device API](DEVICE_API.md).

Both commands currently run in the foreground. Stop each with Control-C; rerun
the same commands to restart. This kit does not silently install launch agents.
Keep the private identity/configuration when updating so pairing survives.

## Local-only runtime boundary

The launcher bypasses the official CLI and forces the OpenClaw provider and
loopback bind. Other providers and their SDK/transports are not imported or
constructed. Only the retained info/session/history/message/event/chat and
Hardware One API routes are reachable. Update, debug, Claude hooks, Codex
app-server startup, tunnel/expose and account-setup routes are blocked. Browser
Origin requests and query-string tokens are rejected; use the authenticated
relay. No secret-bearing startup banner or instance pidfile is written, and
prompt/question payload logging is disabled.
Failed device operations log only the operation, a fixed error category and a
recognized Gateway error code. They never log upstream error messages, prompt
text, session identities or credentials.

At runtime this adapter's outbound connection is the local Gateway WebSocket.
Installing dependencies contacts the npm registry. Mingus's own allowed tools
may use the network under its existing policy; this adapter does not rewrite
that policy. The official source is retained for provenance, including unused
modules, but its other provider imports and outbound startup actions are gated
out of this launch path.

## Verification and current limits

`node --test test_provider.mjs` exercises the production provider and Gateway
codec with a fake WebSocket peer: Ed25519 handshake, fixed-agent routing,
idempotent sends, tool events, offered decisions, history/stream races through
the real device API, uncertain disconnect and bounded cache retirement.
`node --test tests/device-api.test.mjs` covers the bounded device contract.
A separate temporary HTTP smoke test exercised the patched official server,
real Express dependencies, authentication, blocked routes and device history.
These are host-side checks; Studio pairing and a real P4 round trip remain
separate qualification steps.

The P4's voice input accepts at most **512 UTF-8 bytes** per confirmed prompt,
and the native view retains **64 content items**. Longer replies use successive
items within that recent window; full history remains on the Studio. Closing
Terminal only closes the glasses view. Use its explicit interrupt action to
request cancellation of the selected run.

The statistics mapping follows the pinned Gateway's
[cumulative output-token event](https://github.com/openclaw/openclaw/blob/3a9d69db306cd7f081e06254cb89c4bcc14a7107/src/infra/agent-run-usage.ts),
[plan-step statuses](https://github.com/openclaw/openclaw/blob/3a9d69db306cd7f081e06254cb89c4bcc14a7107/src/channels/streaming.ts),
and [in-flight snapshot](https://github.com/openclaw/openclaw/blob/3a9d69db306cd7f081e06254cb89c4bcc14a7107/src/gateway/chat-abort.ts).
Session-wide usage totals are not presented as this run's statistics.

Memory is bounded: 200 cached sessions, 200 recent prompt outcomes, at most 500
or 256 KiB of events per session, 128 KiB of reply text, 32 current tool items,
and eight pending requests of each kind per session. Idle sessions can retire;
active/uncertain sessions are retained. Forgotten prompt IDs fail closed through
a bounded tombstone filter, which may conservatively reject a fresh colliding
ID. Reopen history before deliberately sending a new prompt. Caches are not a
durable transcript and restarting the host invalidates device views.
