#!/usr/bin/env python3
"""Integration guards beside the behavioral Automation scheduler owner tests.

--component <dir> reads the production sources from another tree (default:
this component), so the guards replay against a pre-fix tree.
"""

import argparse
from pathlib import Path


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--component", type=Path,
                    default=Path(__file__).resolve().parents[2],
                    help="component directory to read production sources from")
COMPONENT = parser.parse_args().component.resolve()


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


def code_only(source: str) -> str:
    """The source with C/C++ comments removed (string literals kept)."""
    out = []
    pos = 0
    state = "code"
    while pos < len(source):
        current = source[pos]
        following = source[pos + 1] if pos + 1 < len(source) else ""
        if state == "line_comment":
            if current == "\n":
                state = "code"
                out.append(current)
        elif state == "block_comment":
            if current == "*" and following == "/":
                state = "code"
                pos += 1
        elif state in ("string", "character"):
            out.append(current)
            if current == "\\":
                out.append(following)
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
        else:
            if current == '"':
                state = "string"
            elif current == "'":
                state = "character"
            out.append(current)
        pos += 1
    return "".join(out)


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
    rebuild, "if (!readAutomationsJson(json))"
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
    tick, "if (!readAutomationsJson(json))"
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
assert "const String empty = automationsEmptyStoreJson();" in ensure_file
assert "return true;" in ensure_file[:write]

# automations.json format version: one named constant, checked on every read.
# The owner's only raw read is readAutomationsFile (behind readAutomationsJson);
# the add command refuses a newer file before it would seed an empty store;
# the file-system sanitize passes read through the same check; the writer
# refuses a newer document; no writer spells the version as a literal.
assert "constexpr uint32_t kAutomationsFileVersion = 2;" in header
assert automation.count("readText(AUTOMATIONS_JSON_FILE") == 1
reader = function_body(automation, "static AutomationsRead readAutomationsFile(String& json, uint32_t* newerVersion = nullptr)")
assert "readText(AUTOMATIONS_JSON_FILE" in reader and "automationsNewerFileVersion(json)" in reader
writer = function_body(automation, "bool writeAutomationsJsonAtomic(const String& json)")
assert writer.index("automationsNewerFileVersion(json)") < writer.index("writeTextAtomic(")
# The add command loads through loadAutomationsStoreForEdit (behavior in the
# scheduler harness) and stops when it refuses: its first statement after the
# load is that refusal, which returns. It never seeds or reads on its own.
add = code_only(function_body(automation, "const char* cmd_automation_add(const String& argsInput)"))
load_guard = "if (!loadAutomationsStoreForEdit(json, !validateOnly))"
assert add.count(load_guard) == 1 and add.count("loadAutomationsStoreForEdit(") == 1
refusal = " ".join(function_body(add, load_guard)[1:-1].split())
assert refusal == 'broadcastAutomationsReadError(); return "ERROR";', refusal
for forbidden in ("readText(", "readAutomationsFile(", "readAutomationsJson(", "automationsEmptyStoreJson()"):
    assert forbidden not in add, f"cmd_automation_add must load only through loadAutomationsStoreForEdit ({forbidden})"
edit_load = code_only(function_body(automation, "static bool loadAutomationsStoreForEdit(String& json, bool persistSeed)"))
assert edit_load.index("if (read == AutomationsRead::Refused) return false;") < edit_load.index(
    "automationsEmptyStoreJson()")
# The web Automations page and its exports read through the same check and
# name a refused file's format instead of listing it.
web = code_only((COMPONENT / "WebServer_Server.cpp").read_text(encoding="utf-8"))
assert "readText(AUTOMATIONS_JSON_FILE" not in web, "web automations reads must go through readAutomationsJson"
for handler, report in (("esp_err_t handleAutomationsGet(httpd_req_t* req)", "automationsWebReadError("),
                        ("esp_err_t handleAutomationsExport(httpd_req_t* req)",
                         "return sendAutomationsExportReadError(req, newerVersion);")):
    body = function_body(web, handler)
    assert "readAutomationsJson(" in body and report in body, handler
export = function_body(web, "esp_err_t handleAutomationsExport(httpd_req_t* req)")
assert export.count("readAutomationsJson(json, &newerVersion)") == 2
assert export.count("return sendAutomationsExportReadError(req, newerVersion);") == 2
get_handler = function_body(web, "esp_err_t handleAutomationsGet(httpd_req_t* req)")
assert get_handler.index("readAutomationsJson(") < get_handler.index("sanitizeAutomationsJson(body)")
filesystem = (COMPONENT / "System_Filesystem.cpp").read_text(encoding="utf-8")
assert "readText(AUTOMATIONS_JSON_FILE" not in filesystem
assert filesystem.count("readAutomationsJson(json)") == 2
for source in COMPONENT.glob("*.cpp"):
    text = source.read_text(encoding="utf-8", errors="replace")
    assert '\\"version\\": 2' not in text, f"{source.name} writes the automations version as a literal"

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
assert "else writeTextAtomic(AUTOMATIONS_JSON_FILE, json, VFS::systemAuth(VFS::Scopes::SYSTEM_STATE, \"automation.state\"));" in reschedule
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

# The OLED and G2 automation pages show the owner's summary copies
# (automationCopySummaries / automationFindSummary). They never open or parse
# automations.json themselves, and G2 taps act on the ids of the rows shown.
g2_page = (COMPONENT / "G2_Page_Automations.cpp").read_text(encoding="utf-8")
g2_header = (COMPONENT / "G2_Page_Automations.h").read_text(encoding="utf-8")
oled_page = (COMPONENT / "OLED_Mode_Automations.cpp").read_text(encoding="utf-8")
for name, text in (("G2_Page_Automations.cpp", g2_page), ("G2_Page_Automations.h", g2_header),
                   ("OLED_Mode_Automations.cpp", oled_page)):
    for private in ("AUTOMATIONS_JSON_FILE", "readText(", "deserializeJson(",
                    "streamParseAutomations"):
        assert private not in text, f"{name} still reads the automation owner's file: {private}"
for name, text in (("G2_Page_Automations.cpp", g2_page), ("OLED_Mode_Automations.cpp", oled_page)):
    assert "automationCopySummaries(" in text, f"{name} must list the owner's summaries"
assert "automationFindSummary(" in g2_page
assert "loadAutomationByVisibleIndex" not in g2_page
assert "showDetailMenu(gListIds[sel]);" in function_body(g2_page, "static void listHandleTap(uint32_t idx)")
summary = function_body(automation, "static void fillAutomationSummary(")
assert "parseOneTrigger(" in summary
assert "streamParseAutomations" not in automation and "streamParseAutomations" not in header

print("Automation scheduler integration, empty-file and page summary source guards passed")
