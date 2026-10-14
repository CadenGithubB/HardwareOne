# Hardware One Terminal device API, schema 1

The existing TLS relay accepts `POST /v1/terminal` with its device bearer token.
It adds its configured `deviceId` and forwards only this API to the local Even
Terminal server's `POST /api/hardwareone`, using a separate private bridge token.
The Terminal server and OpenClaw Gateway listen on loopback. Neither Gateway
credentials nor raw Gateway session keys are returned to the device.

Every body is UTF-8 JSON, at most **4096 encoded bytes**, including JSON escaping.
Every request has `schema: 1`, `op`, and a fresh `requestId` of 32 lowercase hex
digits. Reusing the same request ID with the identical mutation returns its
original result; a different body is rejected. Never retry an uncertain prompt
under a new ID. HTTP failure is not evidence that a prompt or approval did not
run. Unknown fields and operations are rejected.

Responses have `schema: 1`, `ok`, `requestId`, and `instance` (server UUID).
Failures have `error: {code, message}`. `reopen_required` invalidates the current
view, option mappings, run controls, and cursors. Reopen explicitly; do not replay
a mutation after reconnect. HTTP admission and `ok` do not imply run completion.

## Operations

| op | Additional request fields | Successful response fields |
|---|---|---|
| `list` | `offset` (0–10000), `limit` (1–10) | `sessions: [{id,title,state}]`, `nextOffset` (number or null) |
| `open` | `sessionId` | `view`, `sessionId`, `cursor:0`, `state`, optional `activity`, `runId` (string or null) |
| `new` | none | Same as open; a new Mingus conversation, without sending a prompt |
| `poll` | `view`, `instance`, `after` (last consumed event ID) | `events`, `cursor`, `more`, `state`, optional `activity`, `runId` |
| `prompt` | `view`, `instance`, `text` (1–2047 UTF-8 bytes) | `runId`, `accepted:true` |
| `interrupt` | `view`, `instance`, `runId` | `accepted` |
| `permission` | `view`, `instance`, `pendingId`, `decision` | `applied` |
| `question` | `view`, `instance`, `pendingId`, `questionId`, `optionIds` (array), or `text`, or `cancel:true` | `applied`, `complete` |

`sessionId` is `hwo_` followed by 64 lowercase hex digits: the host's stable
SHA-256 handle for a canonical `agent:main:` key discovered by `sessions.list`.
It grants no authority by itself. The server rechecks scope and selection.
`view` and `pendingId` are random 32-hex handles bound to device, server instance,
and selected session. Opening replaces that device's previous view. Old view
actions fail even if the same session is selected again.
Idempotency lasts for the current view and server instance. At 256 actions the
device must reopen; retiring a view releases its action cache. The last 32
selection responses are retained. Retired selection IDs enter a bounded
rejection filter, which may conservatively reject an old or colliding ID but
can never replay its action. A fresh explicit selection uses a fresh ID.

The list contains recent unarchived Mingus sessions, up to ten per page. A
non-null `nextOffset` requests the next page; it must not silently alias another
conversation. Titles may be shortened for the 64-byte native list label. Full
conversation history remains in OpenClaw. Open requests ten recent Gateway
entries, then filters them to user/assistant text; this can yield fewer than ten
visible messages. It queues them as content before live events, without copying
the entire transcript onto the P4. The native scrolling view retains 64 content
items. P4 voice confirmation accepts at most 512 UTF-8 bytes, even though this
host API also accepts longer prompts from compatible clients. Closing the
glasses view does not abort the run; cancellation requires `interrupt`.
`prompt` accepts ordinary chat text. Slash/bang prefixes and the pinned
Gateway's bare cancellation phrases fail locally with `command_not_supported`;
the host does not request admin-only command-suppression privileges.

## Poll events

Poll returns up to three events and never more than 4096 encoded bytes. `cursor`
is the ID of the last event actually returned, not the latest host event. Repeat
the same `after` to obtain the same retained events. `more:true` asks for another
poll without the ordinary idle delay. The bounded event ring reports
`reopen_required` on a gap; it never silently advances over lost text.

Each event has monotonic `id`. The following forms are defined:

* `{id,type:"content",itemId,kind,text}`: absolute replacement of a native
  content item. `itemId` is in 1–0x7fffffff local to this view; the high bit is
  reserved for device presentation notices. `kind` is 1 user,
  2 assistant, 3 tool, 4 query, 5 answer. `text` is at most 512 UTF-8 bytes,
  split only between complete code points. A streaming tail keeps the same
  item ID until it reaches capacity. Longer text gets successive items; it is
  not truncated to one lens page. A replacement that shortens a previous
  answer emits empty updates for retired tail items.
* `{id,type:"state",state,runId,activity?}`: `state` is `idle`, `busy`, `awaiting`, or
  `uncertain`. A run ID is an exact host-issued identity, never a guessed
  "latest" run. Interrupt applies only to that run, preserving side runs.
  Optional `activity` is `thinking`, `tool`, or `responding`, and only applies
  while busy. It is omitted when unknown. It changes the native display state,
  never the run's admission, completion or cancellation state.
* `{id,type:"query",pendingId,queryType,questionId,question,options,multiSelect,
  allowText,expiresAtMs,expiresInMs}`: `queryType` is `permission` or `question`. Question
  text is at most 1024 UTF-8 bytes; at most eight options each have an opaque
  `id` and an exact offered `label` at most 128 UTF-8 bytes. Permission option
  IDs are the exact offered Gateway decisions (`allow-once`, `allow-always`,
  `deny`). Question option IDs map to the exact offered Gateway labels.
  `expiresInMs` is recalculated when the poll is serialized, so the P4 can
  retire options without a synchronized wall clock; the host rechecks expiry.
  Question headers and option descriptions are included in the question body.
  Every panel is checked before the first is offered; descriptions are never
  silently removed to make an actionable question fit.
* `{id,type:"query_clear",pendingId}` invalidates its options immediately.
* `{id,type:"host_required",text}` says that a request needs the OpenClaw host
  UI. It offers no approval shortcut. Oversized critical command details,
  external verification, private/secret questions, or unsupported field shapes
  produce this event instead of a misleading shortened approval.

Gateway plan progress and reported run statistics are projected as stable
`kind:3` content items per run. Updates replace the same item rather than
appending a new task/statistics card on every poll. Plans use actual reported
step statuses; statistics use cumulative output tokens and known run timing.
No input-token/cost/turn count is invented when Gateway omits it. No image or
numeric statistics field is claimed in the native Terminal wire schema.

All option mappings are invalidated on instance, view, session, or pending
request changes, expiry, resolution, and disconnect. A question may contain up
to three sequential panels. `questionId` must match the current panel. A
multi-select panel collects the offered options then confirms; `allowText`
permits a voice answer only where Gateway explicitly allows an Other answer.
Answers are staged locally until all panels are answered, then one exact
`question.resolve` is sent. Cancel resolves that pending question as cancelled.
The native QUERY wire only returns an option ID: free-text and multi-select
collection are device UI flows, not invented native wire fields.

## Host provider and authority

Provider helpers: `getDeviceHistorySnapshot(id,10)`,
`getDeviceSnapshot(id)`, `getDeviceEvents(id,after,max)`, `getInstanceId()`, and
`createSession()`. History and live state must be reconciled before the baseline
cursor is advanced, so a completion racing history cannot vanish. Gateway
subscriptions precede history reads. No transcript write or model prompt is
used to probe permissions.

The version-matched contract is OpenClaw `v2026.9.4`, commit
`3a9d69db306cd7f081e06254cb89c4bcc14a7107` (the installed `3a9d69d`). Use
`sessions.list`, `sessions.messages.subscribe`, `chat.history`, `chat.send`
with stable `idempotencyKey`, and `chat.abort` with the exact run. Live tool
events come from `agent`/`session.tool`; observed activity, plans and usage from
the corresponding `agent` streams and the in-flight history snapshot. Approvals come from `session.approval` and
subscription replay; questions from `question.requested`/`question.resolved`
and `question.list` replay.

Approval resolution uses `approval.get`/`approval.resolve` and the complete
safe `presentation.allowedDecisions`; the bridge never fabricates an offer.
It needs a persistent paired host identity plus `operator.approvals`.
Questions need `operator.questions`. Read/write scopes alone do not grant
approval authority. No `operator.admin`, agent sandbox changes, elevated shell,
generic RPC proxy, arbitrary host route, agent selector, or credential read API
is added by this transport.
