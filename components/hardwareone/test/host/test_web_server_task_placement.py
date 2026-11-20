#!/usr/bin/env python3
"""The web server task runs on core 0 at priority 3 in both server configs.

esp_http_server's defaults are priority 5 with no core affinity, which put
the httpd task level with the real-time paths (ESP-NOW, microphone capture,
speech recognition) on whichever core the scheduler chose. The owner's
decision (S-6) is PRO_CORE and TASK_PRIORITY_NORMAL for the HTTPS and the
plain-HTTP server alike.

Source contract on WebServer_Server.cpp:
  - the file starts exactly one HTTPS and one HTTP server;
  - each config sets core_id = PRO_CORE and task_priority =
    TASK_PRIORITY_NORMAL before its start call, once;
  - the two names mean core 0 and priority 3 (System_TaskUtils.h), below
    TASK_PRIORITY_HIGH and above TASK_PRIORITY_LOW;
  - the stack sizes are the ones from before this change.

Not covered: the restore-only server of WebServer_MigrationTool.cpp (it runs
only while first-time setup blocks setup(), before the main loop exists, and
keeps the defaults), and anything about scheduling on a device: the main loop
is the IDF main task on core 0 at priority 1, so handler CPU time still runs
before it. NOTES.md of the 2026-11-20 behaviour batch lists the device checks.
"""
from __future__ import annotations

from pathlib import Path
import re

from test_web_batch_handlers import require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def strip_comments(code: str) -> str:
    return re.sub(r"//[^\n]*", "", code)


def main() -> None:
    server = (COMPONENT / "WebServer_Server.cpp").read_text()
    code = strip_comments(server)
    tasks = (COMPONENT / "System_TaskUtils.h").read_text()

    require(re.search(r"constexpr BaseType_t PRO_CORE = 0;", tasks) is not None, "PRO_CORE is core 0")
    priorities = {name: int(value) for name, value in
                  re.findall(r"constexpr UBaseType_t (TASK_PRIORITY_\w+)\s*= (\d+);", tasks)}
    require(priorities.get("TASK_PRIORITY_NORMAL") == 3, "TASK_PRIORITY_NORMAL is 3")
    require(priorities["TASK_PRIORITY_LOW"] < 3 < priorities["TASK_PRIORITY_HIGH"],
            "priority 3 sits between the loop-level and the real-time tasks")
    require('#include "System_TaskUtils.h"' in server, "the names come from System_TaskUtils.h")

    require(code.count("HTTPD_SSL_CONFIG_DEFAULT()") == 1 and code.count("httpd_ssl_start(") == 1,
            "one HTTPS server config")
    require(code.count("HTTPD_DEFAULT_CONFIG()") == 1 and code.count("httpd_start(") == 1,
            "one HTTP server config")

    https = code[code.index("HTTPD_SSL_CONFIG_DEFAULT()"):code.index("httpd_ssl_start(")]
    http = code[code.index("HTTPD_DEFAULT_CONFIG()"):code.index("httpd_start(")]
    for name, block, prefix, stack in (("HTTPS", https, "sslConfig.httpd.", "11059"),
                                       ("HTTP", http, "config.", "7680")):
        for field, value in (("core_id", "PRO_CORE"), ("task_priority", "TASK_PRIORITY_NORMAL")):
            assignments = re.findall(rf"{re.escape(prefix)}{field}\s*=\s*([^;]+);", block)
            require(assignments == [value],
                    f"{name} config: {field} must be set once, to {value}: {assignments}")
        require(re.findall(rf"{re.escape(prefix)}stack_size\s*=\s*(\d+);", block) == [stack],
                f"{name} config: the stack size stays {stack}")
    # Nothing else in the file overrides the placement.
    require(len(re.findall(r"\bcore_id\s*=", code)) == 2 and
            len(re.findall(r"\btask_priority\s*=", code)) == 2,
            "core_id and task_priority are set in the two configs only")
    print("web server task: core 0, priority 3, in the HTTPS and the HTTP config")


if __name__ == "__main__":
    main()
