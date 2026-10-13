#!/usr/bin/env python3
"""Source-contract checks for production notification mutation adapters and
the web toast fan-out (session fence, toast payload serialization)."""

from pathlib import Path

from test_web_batch_handlers import extract_block


ROOT = Path(__file__).resolve().parents[2]
notifications = (ROOT / "System_Notifications.cpp").read_text()
oled = (ROOT / "OLED_Utils.cpp").read_text()
settings = (ROOT / "System_Settings.cpp").read_text()
web_server = (ROOT / "WebServer_Server.cpp").read_text()
web_events = (ROOT / "WebServer_Events.cpp").read_text()

assert '#include "System_NotificationKindListCore.h"' in notifications
assert "notif_kind_core::prepare(" in notifications
assert "notif_kind_core::apply(" in notifications
assert "runUserSettingsTransaction(uid, applyNotifUserKindMutation" in notifications
personal_start = notifications.index("static const char* notifUserKindListCmd(")
personal_end = notifications.index("// Force-OFF:", personal_start)
personal_body = notifications[personal_start:personal_end]
assert "mergeAndSaveUserSettings(" not in personal_body
assert "std::string_view(token.data, token.length)" in notifications
assert "settings.remove(mutation->key)" in notifications
assert "mutation->coreResult.outputCount" in notifications

assert '#include "System_EventKindMask.h"' in notifications
assert "static inline bool maskTest" not in notifications
assert "static inline void maskSet" not in notifications
assert "eventKindMaskTest(gNotifOffMask" in notifications
assert "eventKindMaskSet(gNotifOffMask" in notifications

assert '#include "System_EventKindMask.h"' in oled
assert '#include "System_EventCatalog.h"' in oled
assert 'String("notifyusermute set ")' in oled
assert "ncToggleMute(const SystemEventCatalogKindInfo& kind)" in oled
assert "eventKindMaskTest(sNcViewer.muteMask, kindId)" in oled
assert "+ kind.name" in oled
for provider_call in (
    "systemEventCatalogFamilyCount",
    "systemEventCatalogFamilyAt",
    "systemEventCatalogFamilyKindAt",
):
    assert provider_call in oled
for obsolete in (
    "sNcKindNames",
    "sNcKindIds",
    "sNcKindCount",
    "ncBuildKindList",
):
    assert obsolete not in oled
assert "uint32_t mask[4]" not in oled
assert "ncMuteTest" not in oled
assert "cmd.reserve(2400)" not in oled

for command in ("notifyusermute", "notifyusershow"):
    command_start = settings.index(f'{{ "{command}"')
    command_help = settings[command_start : command_start + 650]
    assert "set <kind> <on|off>" in command_help
    assert "patch <+kind,-kind>" in command_help
    assert "all|none" in command_help


# Web toast fan-out: every broadcast helper and queue accessor runs under the
# session fence; the viewer predicate runs on a copied username with the fence
# released and the enqueue re-checks the slot generation; handleEvents drains
# through the fenced accessors (bound to its sid) and writes sockets outside.
def guard_scope_closed_before(body: str, guard_at: int, call_at: int) -> bool:
    depth = 0
    for char in body[guard_at:call_at]:
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth < 0:
                return True
    return False


for marker in ("void broadcastNoticeToAllSessions(", "void broadcastEventToAllSessions(",
               "void broadcastEventToSessionsIf(", "uint32_t sseEventDropsTotal(",
               "void broadcastSensorStatusToAllSessions(",
               "bool webSessionDequeueSseEvent(", "bool webSessionDequeueSseNotice(",
               "void webSessionSettleSseQueues(", "void webSessionMarkStatusSent("):
    assert web_server.count(marker) == 1, marker
    helper = extract_block(web_server, marker)
    assert "WebSessionFenceGuard guard;" in helper, f"{marker} must take the session fence"
    assert "if (!guard" in helper, f"{marker} must stop when the fence is not taken"
    for io in ("sseWrite(", "httpd_resp_", "httpd_sess_"):
        assert io not in helper, f"{marker} must not do socket I/O under the fence"

# handleNotice: the fence covers the dequeue and slot clear only; the cookie
# header and the response are written after its scope closes.
notice = extract_block(web_server, "esp_err_t handleNotice(")
notice_guard = notice.index("WebSessionFenceGuard sessionGuard;")
for write in ("writeSessionCookie(", "httpd_resp_send(", "jsonEscape("):
    write_at = notice.index(write, notice_guard)
    assert guard_scope_closed_before(notice, notice_guard, write_at), \
        f"handleNotice must release the session fence before {write}"

fanout = extract_block(web_server, "void broadcastEventToSessionsIf(")
assert "allow(gSessions" not in fanout, "the predicate must not read the live session table"
call_at = fanout.index("allow(user.c_str(), arg)")
first_guard = fanout.index("WebSessionFenceGuard guard;")
assert first_guard < call_at
assert guard_scope_closed_before(fanout, first_guard, call_at), \
    "the fence must be released before the viewer predicate runs"
assert "generation = sWebSessionSlotGeneration[i];" in fanout[:call_at]
after_call = fanout[call_at:]
assert "WebSessionFenceGuard guard;" in after_call
assert after_call.index("sWebSessionSlotGeneration[i] != generation") < after_call.index("sseEnqueueEvent(")

invalidate = extract_block(web_server, "void webInvalidateSessionSlotLocked(")
assert "++sWebSessionSlotGeneration[idx];" in invalidate

handler = extract_block(web_events, "esp_err_t handleEvents(")
assert "sseDequeueEvent(gSessions" not in handler and "sseDequeueNotice(gSessions" not in handler
assert "webSessionDequeueSseNotice(sessIdx, sid, n)" in handler
assert "webSessionDequeueSseEvent(sessIdx, sid, evName, evData)" in handler
assert "webSessionSettleSseQueues(sessIdx, sid);" in handler
assert "needsNotificationTick = false" not in handler
# The sensor-status flag is cleared under the fence, bound to the sid, and only
# when no newer status was published after the snapshot's sequence was read.
assert "needsStatusUpdate = false" not in handler and "lastSensorSeqSent =" not in handler
assert handler.index("const unsigned long seqAtBuild = gSensorStatusSeq;") < \
    handler.index("buildSensorStatusJson()")
assert "webSessionMarkStatusSent(sessIdx, sid, seqAtBuild);" in handler
web_server_h = (ROOT / "WebServer_Server.h").read_text()
for decl in ("bool webSessionDequeueSseEvent(", "bool webSessionDequeueSseNotice(",
             "void webSessionSettleSseQueues(", "void webSessionMarkStatusSent("):
    assert web_server_h.count(decl) == 1, f"WebServer_Server.h must declare {decl}"
mark_sent = extract_block(web_server, "void webSessionMarkStatusSent(")
assert "gSessions[idx].sid != sid" in mark_sent
assert "if (gSensorStatusSeq == seqSent) gSessions[idx].needsStatusUpdate = false;" in mark_sent

# Toast payload: serialized JSON (escapes quotes, backslashes, control
# characters), never a hand-built "%s" string.
tick = extract_block(notifications, "void systemEventsNotifyTick(")
toast_block = extract_block(tick, "if (gSettings.notifToasts)")
assert '\\"msg\\":\\"%s\\"' not in toast_block
assert "notifRenderToastJson(json, sizeof(json)" in toast_block
renderer = extract_block(notifications, "static size_t notifRenderToastJson(")
assert "serializeJson(doc, out, outSize)" in renderer and "measureJson(doc)" in renderer

print("notification integration source guards passed")
