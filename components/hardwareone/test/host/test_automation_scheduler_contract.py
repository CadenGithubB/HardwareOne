#!/usr/bin/env python3
"""Integration guards beside the behavioral Automation scheduler owner tests."""

from pathlib import Path


COMPONENT = Path(__file__).resolve().parents[2]


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    pos = brace
    state = "code"
    while pos < len(source):
        current = source[pos]
        following = source[pos + 1] if pos + 1 < len(source) else ""
        if state == "line_comment":
            if current == "\n":
                state = "code"
        elif state == "block_comment":
            if current == "*" and following == "/":
                state = "code"
                pos += 1
        elif state in ("string", "character"):
            if current == "\\":
                pos += 1
            elif (state == "string" and current == '"') or (
                state == "character" and current == "'"
            ):
                state = "code"
        elif current == "/" and following == "/":
            state = "line_comment"
            pos += 1
        elif current == "/" and following == "*":
            state = "block_comment"
            pos += 1
        elif current == '"':
            state = "string"
        elif current == "'":
            state = "character"
        elif current == "{":
            depth += 1
        elif current == "}":
            depth -= 1
            if depth == 0:
                return source[brace : pos + 1]
        pos += 1
    raise AssertionError(f"unterminated function: {signature}")


automation = (COMPONENT / "System_Automation.cpp").read_text(encoding="utf-8")

# Missing or malformed storage is a coherent empty scheduler state, not an
# invalid cache.  Invalid would make automationsAnyDue() force a LittleFS read
# on every main-loop iteration.
empty = function_body(automation, "static void markAutomationsCacheEmptyValid()")
assert "gAutomationsCacheCount = 0;" in empty
assert "gAutomationEventKindMask[w] = 0;" in empty
assert "gAutomationsCacheValid = true;" in empty

rebuild = function_body(automation, "static void rebuildAutomationsCache()")
read_failure = function_body(
    rebuild, "if (!readText(AUTOMATIONS_JSON_FILE, json))"
)
parse_failure = function_body(rebuild, "if (deserializeJson(doc, json))")
assert "markAutomationsCacheEmptyValid();" in read_failure
assert "markAutomationsCacheEmptyValid();" in parse_failure

# The full scheduler reads the file before reaching its trailing rebuild.  Its
# early read-failure path must therefore publish the same valid-empty state or
# the next main-loop pass immediately retries the entire tick.
tick = function_body(automation, "void schedulerTickMinute()")
assert "gAutomationEventsPending.exchange(false);" in tick
tick_read_failure = function_body(
    tick, "if (!readText(AUTOMATIONS_JSON_FILE, json))"
)
assert "markAutomationsCacheEmptyValid();" in tick_read_failure
assert tick_read_failure.index("markAutomationsCacheEmptyValid();") < (
    tick_read_failure.index("return;")
)

due = function_body(automation, "bool automationsAnyDue(time_t now)")
assert "if (!gAutomationsCacheValid) return true;" in due

# Shared mutable scheduler state and its poll policy belong to this owner.
# These integration guards complement runtime behavior in automation_scheduler.
header = (COMPONENT / "System_Automation.h").read_text(encoding="utf-8")
main = (COMPONENT / "HardwareOne.cpp").read_text(encoding="utf-8")
assert "automationTick();" in main
for retired in ("gAutomationsDirty", "gAutomationSchedulerRunning",
                "gAutomationsMemo", "schedulerTickMinute", "automationsAnyDue",
                "automationEventsPending"):
    assert retired not in header
    assert retired not in main
for name in ("automationSchedulerRunning", "notifyAutomationScheduler", "automationTick"):
    assert name in header

# Starting from any entry point (boot init, resume, or `automation system
# enable`) creates the canonical empty v2 file when an older deployment profile
# never seeded one.  Existing files are preserved, including malformed ones
# that need operator recovery rather than silent replacement.
ensure_file = function_body(automation, "static bool ensureAutomationsFile()")
exists = ensure_file.index("VFS::existsGuarded(")
write = ensure_file.index("writeAutomationsJsonAtomic(empty)")
assert exists < write
assert '"version\\\": 2' in ensure_file
assert "return true;" in ensure_file[:write]

start = function_body(automation, "bool startAutomationScheduler()")
assert start.index("ensureAutomationsFile()") < start.index(
    "ensureAutomationsCache()"
)

# Duplicate repair is reachable from schedulerTickMinute() on the main task.
# Keep its 512-ID (2 KiB) scratch allocation off that constrained stack and
# prove the single successful allocation is released on the normal exit.
sanitize = function_body(automation, "bool sanitizeAutomationsJson(String& jsonRef)")
assert "unsigned long seen[kMax]" not in sanitize
allocation = sanitize.index('ps_alloc(kMax * sizeof(unsigned long)')
failure = sanitize.index("if (!seen)", allocation)
release = sanitize.index("ps_free(seen);", failure)
result = sanitize.rindex("return changed;")
assert allocation < failure < release < result

# Writes inside a claimed scheduler pass do not notify the scheduler: the pass
# ends with a cache rebuild that reads them, and a notification would only
# queue a second full pass. A manual run (command task) still notifies.
reschedule = function_body(
    automation,
    "static void rescheduleAfterFire(long id, const char* automationJson, time_t firedAt, bool notify) {")
assert "if (notify) writeAutomationsJsonAtomic(json);" in reschedule
assert "else writeTextAtomic(AUTOMATIONS_JSON_FILE, json);" in reschedule
assert tick.count("rescheduleAfterFire(") == 3
assert tick.count("rescheduleAfterFire(id, obj.c_str(), now, false)") == 3
assert "rescheduleAfterFire(idStr.toInt(), obj.c_str(), now, true);" in automation

# The main loop ticks unconditionally; the owner checks permission itself and
# stops when another writer revoked it (automationSchedulerRunning stays true
# to the permission). Both permission writers go through the owner transition.
assert "if (gSettings.automationEnabled) {\n    automationTick();" not in main
owner_tick = function_body(automation, "void automationTick() {")
assert owner_tick.index("if (!gSettings.automationEnabled)") < owner_tick.index("AutomationStateGuard guard;")
assert "stopAutomationScheduler();" in owner_tick
system = function_body(automation, "const char* automationSetSystemEnabled(bool enable) {")
assert "startAutomationScheduler()" in system and "stopAutomationScheduler();" in system
features = (COMPONENT / "System_FeatureRegistry.cpp").read_text(encoding="utf-8")
assert "return automationSetSystemEnabled;" in features

print("Automation scheduler integration and empty-file source guards passed")
