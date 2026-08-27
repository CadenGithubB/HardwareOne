#!/usr/bin/env python3
"""Enforce the typed System Event catalog's production ownership boundary."""

from __future__ import annotations

import os
from pathlib import Path
import re


HERE = Path(__file__).resolve().parent
REPOSITORY_ROOT = HERE.parents[3]
COMPONENT = HERE.parents[1]
CATALOG_ROWS = COMPONENT / "System_EventCatalogRows.h"
CATALOG_HEADER = COMPONENT / "System_EventCatalog.h"
CATALOG_CORE = COMPONENT / "System_EventCatalogCore.h"
CATALOG_SOURCE = COMPONENT / "System_EventCatalog.cpp"
CATALOG_JSON_HEADER = COMPONENT / "System_EventCatalogJson.h"
CATALOG_JSON_CORE = COMPONENT / "System_EventCatalogJsonCore.h"
CATALOG_JSON_SOURCE = COMPONENT / "System_EventCatalogJson.cpp"
CATALOG_TEXT_CORE = COMPONENT / "System_EventCatalogTextCore.h"
EVENTS_HEADER = COMPONENT / "System_Events.h"
EVENTS_SOURCE = COMPONENT / "System_Events.cpp"
WEB_SERVER_SOURCE = COMPONENT / "WebServer_Server.cpp"
OLED_AUTOMATIONS_SOURCE = COMPONENT / "OLED_Mode_Automations.cpp"
OLED_UTILS_SOURCE = COMPONENT / "OLED_Utils.cpp"
COMPONENT_CMAKE = COMPONENT / "CMakeLists.txt"
MAX_DIRECT_KIND_NAME_LITERALS_PER_SOURCE = 4

if not __debug__:
    raise RuntimeError(
        "event catalog structure guards require Python assertions; do not use -O"
    )

ROW_INCLUDE_RE = re.compile(
    r'^\s*#\s*include\s*[<"]'
    r'(?:[^>"\n]*/)?System_EventCatalogRows\.h[>"]',
    re.M,
)
FAMILY_ROW_RE = re.compile(
    r"^\s*HW1_EVENT_CATALOG_FAMILY_ROW\(\s*SYSEVT_FAM_[A-Z0-9_]+\s*,",
    re.M,
)
KIND_ROW_RE = re.compile(
    r'^\s*HW1_EVENT_CATALOG_KIND_ROW\(\s*SYSEVT_[A-Z0-9_]+\s*,\s*'
    r'"([a-z0-9_]+)"\s*,\s*SYSEVT_FAM_[A-Z0-9_]+\s*\)',
    re.M,
)
STRING_LITERAL_RE = re.compile(r'"((?:\\.|[^"\\])*)"')


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


SOURCE_SUFFIXES = {".h", ".hpp", ".c", ".cc", ".cpp"}
EXCLUDED_DIRECTORY_NAMES = {
    "arduino",
    "build",
    "dist",
    "docs",
    "docs2",
    "external",
    "hardwareone_libs",
    "managed_components",
    "node_modules",
    "output",
    "third_party",
    "tmp",
    "vendor",
    "vendors",
    "venv",
}


def exclude_directory(name: str) -> bool:
    return (
        name.startswith(".")
        or name in EXCLUDED_DIRECTORY_NAMES
        or name.startswith("build-")
    )


def iter_first_party_sources() -> tuple[Path, ...]:
    sources: list[Path] = []
    for directory, child_directories, filenames in os.walk(REPOSITORY_ROOT):
        child_directories[:] = sorted(
            name for name in child_directories if not exclude_directory(name)
        )
        directory_path = Path(directory)
        for filename in sorted(filenames):
            path = directory_path / filename
            if path.suffix in SOURCE_SUFFIXES:
                sources.append(path)
    return tuple(sources)


def strip_comments_and_literals(source: str) -> str:
    token_re = re.compile(
        r'"(?:\\.|[^"\\])*"'
        r"|'(?:\\.|[^'\\])*'"
        r"|//[^\n]*"
        r"|/\*.*?\*/",
        re.S,
    )

    def replace(match: re.Match[str]) -> str:
        token = match.group(0)
        return "\n" * token.count("\n")

    return token_re.sub(replace, source)


def braced_body(source: str, signature: str) -> str:
    """Return a function body with comments and literals removed."""
    stripped = strip_comments_and_literals(source)
    start = stripped.index(signature)
    brace = stripped.index("{", start)
    depth = 0
    for position in range(brace, len(stripped)):
        if stripped[position] == "{":
            depth += 1
        elif stripped[position] == "}":
            depth -= 1
            if depth == 0:
                return stripped[brace : position + 1]
    raise AssertionError(f"unterminated function: {signature}")


def include_consumers(header_name: str) -> set[Path]:
    include_re = re.compile(
        rf'^\s*#\s*include\s*[<"](?:[^>"\n]*/)?{re.escape(header_name)}[>"]',
        re.M,
    )
    return {
        path.resolve()
        for path in first_party_sources
        if include_re.search(read(path))
    }


rows = read(CATALOG_ROWS)
header = read(CATALOG_HEADER)
core = read(CATALOG_CORE)
source = read(CATALOG_SOURCE)
json_header = read(CATALOG_JSON_HEADER)
json_core = read(CATALOG_JSON_CORE)
json_source = read(CATALOG_JSON_SOURCE)
text_core = read(CATALOG_TEXT_CORE)
events_header = read(EVENTS_HEADER)
events_source = read(EVENTS_SOURCE)
web_server_source = read(WEB_SERVER_SOURCE)
oled_automations_source = read(OLED_AUTOMATIONS_SOURCE)
oled_utils_source = read(OLED_UTILS_SOURCE)
cmake = read(COMPONENT_CMAKE)
first_party_sources = iter_first_party_sources()

# The private file is the single literal row owner and remains intentionally
# repeat-included. A conventional guard would silently suppress later table
# generations in the same translation unit.
assert "#pragma once" not in rows
assert not re.search(r"^\s*#ifndef\s+SYSTEM_EVENTCATALOGROWS", rows, re.M)
assert "define exactly one System Event catalog row macro" in rows
assert len(FAMILY_ROW_RE.findall(rows)) == 12
kind_names = KIND_ROW_RE.findall(rows)
assert len(kind_names) == 152
assert len(set(kind_names)) == 152
assert not ({"boot", "none", "set", "patch", "all", "list"} & set(kind_names))

literal_row_owners: list[Path] = []
for path in first_party_sources:
    if path == CATALOG_ROWS:
        continue
    text = read(path)
    if re.search(
        r"HW1_EVENT_CATALOG_(?:FAMILY|KIND)_ROW\(\s*SYSEVT_", text
    ):
        literal_row_owners.append(path)
assert literal_row_owners == [], literal_row_owners

# Only catalog generation and its real host test may repeat-include rows.
allowed_row_consumers = {
    CATALOG_HEADER.resolve(),
    CATALOG_SOURCE.resolve(),
    (HERE / "test_event_catalog.cpp").resolve(),
}
actual_row_consumers: set[Path] = set()
for path in first_party_sources:
    text = read(path)
    if ROW_INCLUDE_RE.search(text):
        actual_row_consumers.add(path.resolve())
assert actual_row_consumers == allowed_row_consumers, actual_row_consumers

# Internal implementation seams have intentionally narrow include allowlists.
# Production adapters walk public provider operations; they may not bypass that
# boundary through the raw catalog-validation core or private row source.
allowed_catalog_core_consumers = {
    CATALOG_SOURCE.resolve(),
    (HERE / "test_event_catalog.cpp").resolve(),
}
assert include_consumers("System_EventCatalogCore.h") == (
    allowed_catalog_core_consumers
)

allowed_json_core_consumers = {
    CATALOG_JSON_SOURCE.resolve(),
    (HERE / "test_event_catalog_json.cpp").resolve(),
}
assert include_consumers("System_EventCatalogJsonCore.h") == (
    allowed_json_core_consumers
)

allowed_text_core_consumers = {
    EVENTS_SOURCE.resolve(),
    (HERE / "test_event_catalog_text.cpp").resolve(),
}
assert include_consumers("System_EventCatalogTextCore.h") == (
    allowed_text_core_consumers
)

# The old public expansion macros and duplicate enum declarations must not
# survive the extraction anywhere in first-party source.
for path in first_party_sources:
    text = read(path)
    assert not re.search(r"#\s*define\s+SYSEVT_(?:FAMILY|KIND)_LIST\b", text), path
assert "enum SystemEventFamily" not in events_header
assert "enum SystemEventKind" not in events_header
assert '#include "System_EventCatalog.h"' in events_header

# A renamed copied table must not evade checks that know only the former table
# symbols. Direct canonical-name literals are allowed for a handful of focused
# comparisons/messages, but any file embedding a larger slice of the catalog
# is a second vocabulary owner and must instead traverse the provider.
canonical_kind_names = set(kind_names)
literal_catalog_copies: dict[Path, list[str]] = {}
for path in first_party_sources:
    if path == CATALOG_ROWS:
        continue
    direct_names = sorted(
        canonical_kind_names & set(STRING_LITERAL_RE.findall(read(path)))
    )
    if len(direct_names) > MAX_DIRECT_KIND_NAME_LITERALS_PER_SOURCE:
        literal_catalog_copies[path] = direct_names
assert literal_catalog_copies == {}, literal_catalog_copies

# Provider/core headers stay dependency-light and presentation-neutral.
for path, text in ((CATALOG_HEADER, header), (CATALOG_CORE, core)):
    lowered = strip_comments_and_literals(text).lower()
    for banned in (
        "arduino",
        "freertos",
        "arduinojson",
        "jsondocument",
        "httpd_",
        "bluetooth",
        "oled",
        "g2_",
        "renderer",
        "semaphore",
        "mutex",
        "lock_guard",
        "critical_section",
    ):
        assert banned not in lowered, (path, banned)
assert "JsonSink" not in header
assert "JsonStatus" not in header
assert "SystemEvent" not in core

# JSON and human-text adapters are dependency-light projections. The public
# JSON contract contains no provider internals, and neither core may reach the
# private rows or the provider validation/index seam.
for path, text in (
    (CATALOG_JSON_HEADER, json_header),
    (CATALOG_JSON_CORE, json_core),
    (CATALOG_TEXT_CORE, text_core),
):
    lowered = strip_comments_and_literals(text).lower()
    for banned in (
        "arduino",
        "freertos",
        "arduinojson",
        "jsondocument",
        "httpd_",
        "bluetooth",
        "oled",
        "g2_",
        "semaphore",
        "mutex",
        "lock_guard",
        "critical_section",
        "system_eventcatalogrows.h",
        "system_eventcatalogcore.h",
    ):
        assert banned not in lowered, (path, banned)

assert "SystemEventCatalogFamilyInfo" not in json_header
assert "SystemEventCatalogKindInfo" not in json_header
assert "System_EventCatalogJsonCore.h" not in events_source
assert "System_EventCatalogJsonCore.h" not in web_server_source
assert "System_EventCatalogTextCore.h" not in web_server_source

# Production tables/indexes have one out-of-line immutable owner. Descriptor
# arrays remain constant-evaluation locals instead of a second runtime table.
assert "static constexpr const char* kFamilyLabels[]" in source
assert "static constexpr const char* kKindNames[]" in source
assert "static constexpr uint8_t kKindFamilies[]" in source
assert "static constexpr auto kFamilyIndex" in source
assert not re.search(
    r"^static\s+constexpr\s+catalog_core::(?:Family|Kind)Descriptor\s+k",
    source,
    re.M,
)

stripped_source = strip_comments_and_literals(source)
for pattern in (
    r"\bnew\b",
    r"\bmalloc\s*\(",
    r"\bcalloc\s*\(",
    r"\brealloc\s*\(",
    r"\bps_(?:alloc|calloc)\s*\(",
    r"\bheap_caps_(?:malloc|calloc|realloc)\s*\(",
    r"\bString\b",
    r"\bmutex\b",
    r"\bsemaphore\b",
    r"\block_guard\b",
    r"taskENTER_CRITICAL",
    r"portENTER_CRITICAL",
):
    assert not re.search(pattern, stripped_source, re.I), pattern

# Serialization/text projections are likewise allocation-free and lock-free.
# Their only mutable output belongs to a caller buffer or synchronous sink.
for path, text in (
    (CATALOG_JSON_SOURCE, json_source),
    (CATALOG_JSON_CORE, json_core),
    (CATALOG_TEXT_CORE, text_core),
):
    stripped_adapter = strip_comments_and_literals(text)
    for pattern in (
        r"\bnew\b",
        r"\bmalloc\s*\(",
        r"\bcalloc\s*\(",
        r"\brealloc\s*\(",
        r"\bps_(?:alloc|calloc)\s*\(",
        r"\bheap_caps_(?:malloc|calloc|realloc)\s*\(",
        r"\bString\b",
        r"\bmutex\b",
        r"\bsemaphore\b",
        r"\block_guard\b",
        r"taskENTER_CRITICAL",
        r"portENTER_CRITICAL",
    ):
        flags = 0 if pattern == r"\bString\b" else re.I
        assert not re.search(pattern, stripped_adapter, flags), (path, pattern)

# All typed and legacy definitions moved together; the event-ring unit merely
# calls the compatibility API and no longer owns catalog tables.
provider_functions = (
    "systemEventCatalogFamilyCount",
    "systemEventCatalogKindCount",
    "systemEventCatalogFamilyAt",
    "systemEventCatalogKindAt",
    "systemEventCatalogFamilyKindAt",
    "systemEventCatalogFindKind",
    "systemEventFamilyName",
    "systemEventKindFamily",
    "systemEventKindName",
    "systemEventKindFromName",
)
production_sources = list(COMPONENT.glob("*.cpp"))
for function in provider_functions:
    definition_re = re.compile(rf"\b{function}\s*\([^)]*\)\s*\{{", re.S)
    owners = [path for path in production_sources if definition_re.search(read(path))]
    assert owners == [CATALOG_SOURCE], (function, owners)

assert "kEventKindNames" not in events_source
assert "kEventKindFamily" not in events_source
assert "kEventFamilyNames" not in events_source
assert "System_EventCatalogRows.h" not in events_source

# Both native OLED pickers traverse the shared immutable family index directly.
# They must not regress to fixed-size copied projections or dense enum scans.
assert '#include "System_EventCatalog.h"' in oled_automations_source
for provider_call in (
    "systemEventCatalogFamilyCount",
    "systemEventCatalogFamilyAt",
    "systemEventCatalogFamilyKindAt",
):
    assert provider_call in oled_automations_source, provider_call
for obsolete in (
    "sWizKindPtrs",
    "sWizKindCount",
    "wizBuildKindList",
    "sWizEventKind[",
    "systemEventKindFamily",
    "systemEventKindName",
    "systemEventFamilyName",
    "SYSEVT_COUNT",
    "SYSEVT_FAM_COUNT",
):
    assert obsolete not in oled_automations_source, obsolete
assert "sWizEventFamilyIndex = SIZE_MAX" in oled_automations_source
assert "sWizEventKindIndex = SIZE_MAX" in oled_automations_source

assert '#include "System_EventCatalog.h"' in oled_utils_source
notification_picker_start = oled_utils_source.index("enum NcLevel")
notification_picker_end = oled_utils_source.index(
    "// Helper to get source name string", notification_picker_start
)
notification_picker = strip_comments_and_literals(
    oled_utils_source[notification_picker_start:notification_picker_end]
)
for provider_call in (
    "systemEventCatalogFamilyCount",
    "systemEventCatalogFamilyAt",
    "systemEventCatalogFamilyKindAt",
):
    assert provider_call in notification_picker, provider_call
for obsolete in (
    "sNcKindNames",
    "sNcKindIds",
    "sNcKindCount",
    "ncBuildKindList",
    "systemEventKindFamily",
    "systemEventKindName",
    "systemEventFamilyName",
    "SYSEVT_COUNT",
    "SYSEVT_FAM_COUNT",
):
    assert obsolete not in notification_picker, obsolete
assert "sNcFamilyIndex = SIZE_MAX" in notification_picker
assert re.search(
    r"systemEventCatalogFamilyKindAt\s*\(\s*family\.id\s*,",
    notification_picker,
)

# JSON production owns the three public serializer functions, delegates its
# grammar to the tested JSON core, and adapts only the public typed provider.
assert '#include "System_EventCatalogJson.h"' in json_source
assert '#include "System_EventCatalogJsonCore.h"' in json_source
assert "System_EventCatalogRows.h" not in json_source
assert "System_EventCatalogCore.h" not in json_source
assert "ArduinoJson" not in json_source
assert "HW1_EVENT_CATALOG_FAMILY_ROW" not in json_source
assert "HW1_EVENT_CATALOG_KIND_ROW" not in json_source
assert re.search(r"\b(?:hw1_event_catalog_json_core|json_core)::writeJson\s*\(",
                 json_source)
assert re.search(r"\b(?:hw1_event_catalog_json_core|json_core)::measureJson\s*\(",
                 json_source)
for provider_call in (
    "systemEventCatalogFamilyCount",
    "systemEventCatalogFamilyAt",
    "systemEventCatalogFamilyKindAt",
):
    assert provider_call in json_source, provider_call

json_functions = (
    "systemEventCatalogJsonSize",
    "systemEventCatalogWriteJson",
    "systemEventCatalogJsonToBuffer",
)
for function in json_functions:
    definition_re = re.compile(rf"\b{function}\s*\([^)]*\)\s*\{{", re.S)
    owners = [
        path for path in production_sources if definition_re.search(read(path))
    ]
    assert owners == [CATALOG_JSON_SOURCE], (function, owners)

# The CLI no longer owns either catalog grammar. JSON uses the bounded public
# adapter; human output uses the checked text core and the real debug payload
# limit. The old ArduinoJson tree and unchecked 120-byte snprintf loop may not
# survive as renamed local builders.
assert '#include "System_EventCatalogJson.h"' in events_source
assert '#include "System_EventCatalogTextCore.h"' in events_source
events_command = braced_body(events_source, "const char* cmd_events(")
assert "systemEventCatalogJsonToBuffer" in events_command
assert "writeCatalog" in events_command
assert "DEBUG_MSG_SIZE - 1" in events_command
for obsolete in (
    "PSRAM_JSON_DOC",
    "JsonArray",
    "JsonObject",
    "serializeJson",
    "SYSEVT_FAM_COUNT",
    "SYSEVT_COUNT",
    "char line[120]",
):
    assert obsolete not in events_command, obsolete
assert not re.search(r"\+=\s*snprintf\s*\(", events_command)

# The authenticated HTTP endpoint streams the same public serializer. It may
# use ArduinoJson elsewhere in this large translation unit, but the catalog
# handler itself cannot rebuild or buffer a private families tree.
assert '#include "System_EventCatalogJson.h"' in web_server_source
events_http_handler = braced_body(
    web_server_source, "static esp_err_t handleEventsKinds("
)
assert "systemEventCatalogJsonSize" in events_http_handler
assert "systemEventCatalogWriteJson" in events_http_handler
assert events_http_handler.index("systemEventCatalogJsonSize") < (
    events_http_handler.index("systemEventCatalogWriteJson")
)
http_sink_match = re.search(
    r"systemEventCatalogWriteJson\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)",
    events_http_handler,
)
assert http_sink_match is not None
events_http_sink = braced_body(
    web_server_source, f"{http_sink_match.group(1)}("
)
assert "httpd_resp_send_chunk" in events_http_sink
for obsolete in (
    "PSRAM_JSON_DOC",
    "JsonArray",
    "JsonObject",
    "serializeJson",
    "CMD_RESULT_MAX",
    "SYSEVT_FAM_COUNT",
    "SYSEVT_COUNT",
):
    assert obsolete not in events_http_handler, obsolete

# The provider is in the unconditional base source list, before any feature
# gate appends optional consumers.
base_source_match = re.search(
    r"set\(hardwareone_srcs(?P<body>.*?)\n\)", cmake, re.S
)
assert base_source_match is not None
assert "System_EventCatalog.cpp" in base_source_match.group("body")
assert "System_EventCatalogJson.cpp" in base_source_match.group("body")

print("event catalog structure guards passed")
