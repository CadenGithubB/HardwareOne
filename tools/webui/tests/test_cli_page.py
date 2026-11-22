"""Behavioral coverage for interactive-help state and CLI credential scrubbing.

The browser cannot infer the CLI's mode from command spelling: ``help espnow``
enters help directly, module-level ``back`` remains in help, and an ordinary
command may pass through help and leave the mode.  The server therefore returns
``X-HW1-CLI-Help: active|inactive`` and the page must reconcile its runtime and
persisted state from that response.

This test executes the JavaScript extracted from ``WebPage_CLI.h`` against a
small fake DOM and scripted HTTP Responses.  It does not copy the page logic.

Run from the repository root:

    python3 -m unittest tools.webui.tests.test_cli_page
"""

from __future__ import annotations

import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest
import urllib.parse

from tools.webui import extract_js
from tools.webui.js_engine import JS_ENGINE, run_js


REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
PAGE = REPO_ROOT / "components" / "hardwareone" / "WebPage_CLI.h"
LOGIN_PAGE = REPO_ROOT / "components" / "hardwareone" / "WebPage_Login.h"
SERVER = REPO_ROOT / "components" / "hardwareone" / "WebServer_Server.cpp"
CLI_STORAGE_KEYS = ("cliHistory", "cliOutputHistory", "cliOutputHistoryBackup", "cliInHelp", "cliStoreV")
HARNESS = REPO_ROOT / "tools" / "webui" / "harness" / "cli_page_harness.js"
EXPECTED_CHECKS = 82


class CliPageExtractionTests(unittest.TestCase):
    """Fail loudly if extraction or the response-aware transport regresses."""

    def test_shipping_page_exposes_response_headers_to_the_handler(self) -> None:
        page_js, _ = extract_js.page_js(PAGE)
        self.assertGreater(len(page_js), 8_000, "extracted CLI JS is implausibly small")
        self.assertNotIn("<script", page_js, "script tags must be removed before execution")
        self.assertIn("function executeCommand", page_js)
        self.assertIn("X-HW1-CLI-Help", page_js)
        self.assertIn("hw.postForm('/api/cli'", page_js)
        self.assertNotIn(
            "hw.postFormText('/api/cli'",
            page_js,
            "postFormText discards X-HW1-CLI-Help and cannot reconcile mode",
        )


@unittest.skipUnless(
    JS_ENGINE, "a JavaScript engine (node/deno/bun/qjs/jsc/osascript) is required"
)
class CliPageBehaviorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory()
        extracted = pathlib.Path(cls.temporary.name) / "cli_page.js"
        page_js, _line_map = extract_js.page_js(PAGE)
        extracted.write_text(page_js, encoding="utf-8")
        cls.result = run_js(HARNESS, [str(extracted)])
        cls.lines = cls.result.stdout.splitlines()

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temporary.cleanup()

    def test_engine_did_not_crash(self) -> None:
        self.assertEqual(
            0,
            self.result.returncode,
            f"JS engine {JS_ENGINE} exited {self.result.returncode}\n"
            f"stderr:\n{self.result.stderr}\nstdout:\n{self.result.stdout}",
        )

    def test_harness_reported_one_verdict(self) -> None:
        verdicts = [line for line in self.lines if line.startswith("HARNESS_RESULT ")]
        self.assertEqual(
            1,
            len(verdicts),
            f"expected one harness verdict\n{self.result.stdout}{self.result.stderr}",
        )

    def test_all_shipping_behaviors_pass(self) -> None:
        verdicts = [line for line in self.lines if line.startswith("HARNESS_RESULT ")]
        if not verdicts:
            self.skipTest("verdict shape is covered separately")
        failures = [line for line in self.lines if line.startswith("FAIL ")]
        self.assertEqual(
            "HARNESS_RESULT PASS",
            verdicts[0],
            "CLI page behavior regressed:\n  " + "\n  ".join(failures),
        )

    def test_expected_number_of_checks_ran(self) -> None:
        checks = [
            line
            for line in self.lines
            if line.startswith("PASS ") or line.startswith("FAIL ")
        ]
        self.assertEqual(
            EXPECTED_CHECKS,
            len(checks),
            f"ran {len(checks)} checks, expected {EXPECTED_CHECKS}\n"
            f"{self.result.stdout}",
        )


def _function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    depth = 0
    for index in range(source.index("{", start), len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise AssertionError(f"unbalanced body for {signature}")


class CliDisplayHeaderServerTests(unittest.TestCase):
    """N-38, device side: the page may keep only what the device redacted."""

    def setUp(self) -> None:
        self.source = SERVER.read_text(encoding="utf-8")
        self.handler = _function_body(self.source, "esp_err_t handleCliCommand(")

    def test_header_comes_from_the_audit_redaction(self) -> None:
        self.assertRegex(self.handler, r"const String safeCmd = redactCmdForAudit\(cmd\);")
        self.assertRegex(
            self.handler,
            r"std::unique_ptr<char, void \(\*\)\(void\*\)> cmdDisplay\(encodeCliDisplayHeader\(safeCmd\), free\);",
            "the X-Cmd-Display value must be built from redactCmdForAudit's output and live to the end of the handler",
        )
        self.assertNotRegex(self.handler, r"encodeCliDisplayHeader\((?!safeCmd\))",
                            "the display header must never be built from the raw command")

    def test_every_reply_after_parsing_carries_the_header(self) -> None:
        after = self.handler[self.handler.index("redactCmdForAudit(cmd)"):]
        sends = re.findall(r"httpd_resp_send(?:str)?\(req,", after)
        headers = re.findall(r'httpd_resp_set_hdr\(req, "X-Cmd-Display", cmdDisplay\.get\(\)\)', after)
        self.assertEqual(2, len(sends), "reply paths after parsing changed; give each one the header")
        self.assertEqual(len(sends), len(headers), "a reply after parsing is sent without X-Cmd-Display")
        for match in re.finditer(r"httpd_resp_send(?:str)?\(req,", after):
            preceding = after[:match.start()]
            self.assertGreater(preceding.rfind('"X-Cmd-Display"'), preceding.rfind("return ESP_OK;"),
                               "X-Cmd-Display must be set on the same path, before the send")

    @unittest.skipUnless(shutil.which("c++"), "a host C++ compiler is required")
    def test_header_value_is_ascii_and_decodes_back(self) -> None:
        encoder = _function_body(self.source, "static char* encodeCliDisplayHeader(")
        cases = ["wifiadd net ***", "login bob *** \"quoted\"", "x\r\ny", "100% sure & more", "caf\u00e9 \u2026 \u00ff",
                 "A-Z_a.z!~*'()", ""]
        shim = r"""
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
struct String { std::string v; size_t length() const { return v.size(); } const char* c_str() const { return v.c_str(); } };
enum class AllocPref { PreferPsram };
static void* ps_alloc(size_t n, AllocPref, const char*) { return std::malloc(n); }
"""
        main = "int main(int argc, char** argv) { for (int i = 1; i < argc; ++i) { String s{argv[i]}; char* o = encodeCliDisplayHeader(s); if (!o) return 2; std::printf(\"%s\\n\", o); std::free(o); } return 0; }\n"
        with tempfile.TemporaryDirectory(prefix="hw1-cli-display-") as directory:
            source = pathlib.Path(directory) / "encoder.cpp"
            binary = pathlib.Path(directory) / "encoder"
            source.write_text(shim + encoder + "\n" + main, encoding="utf-8")
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Werror", str(source), "-o", str(binary)], check=True, timeout=60)
            raw = subprocess.run([str(binary), *cases], check=True, capture_output=True, timeout=10).stdout
        outputs = raw.decode("ascii").split("\n")[:-1]
        self.assertEqual(len(cases), len(outputs))
        for case, encoded in zip(cases, outputs):
            self.assertRegex(encoded, r"^[A-Za-z0-9\-_.!~*'()%]*$", "header value must be token-safe ASCII")
            self.assertEqual(case, urllib.parse.unquote(encoded, errors="strict"),
                             "decodeURIComponent of the header must give back the redacted line")
        self.assertEqual("wifiadd%20net%20***", outputs[0])


LOGIN_HARNESS = r"""
var passes = 0, failures = 0;
function check(name, condition, detail) {
    if (condition) { passes++; __out('PASS ' + name); }
    else { failures++; __out('FAIL ' + name + (detail ? '   [' + detail + ']' : '')); }
}
var source = slurp(__argv[0]);
function Storage(seed) { this.data = {}; for (var k in seed) this.data[k] = String(seed[k]); }
Storage.prototype.getItem = function (k) { return Object.prototype.hasOwnProperty.call(this.data, k) ? this.data[k] : null; };
Storage.prototype.setItem = function (k, v) { this.data[k] = String(v); };
Storage.prototype.removeItem = function (k) { delete this.data[k]; };
function load(storage) {
    var windowStub = { listeners: [], addEventListener: function (n, fn) { this.listeners.push(n); } };
    new Function('window', 'localStorage', 'sessionStorage', 'setTimeout', 'alert', source)(
        windowStub, storage, new Storage({}), function () {}, function () {});
    return windowStub;
}
var seeded = new Storage({cliHistory: '["wifiadd net secretpass"]', cliOutputHistory: '$ wifiadd net secretpass',
                          cliOutputHistoryBackup: 'backup', cliInHelp: 'true', cliStoreV: '2', theme: 'dark'});
var threw = null;
try { load(seeded); } catch (e) { threw = String(e); }
check('login page script runs', threw === null, threw);
check('login page removes every CLI storage key',
      seeded.getItem('cliHistory') === null && seeded.getItem('cliOutputHistory') === null &&
      seeded.getItem('cliOutputHistoryBackup') === null && seeded.getItem('cliInHelp') === null,
      JSON.stringify(seeded.data));
check('login page leaves other storage alone', seeded.getItem('theme') === 'dark');
check('login page also drops the CLI store format mark', seeded.getItem('cliStoreV') === null);
var blocked = { getItem: function () { throw new Error('denied'); }, setItem: function () { throw new Error('denied'); },
                removeItem: function () { throw new Error('denied'); } };
threw = null;
try { load(blocked); } catch (e) { threw = String(e); }
check('blocked storage does not break the login page', threw === null, threw);
__out(failures ? 'HARNESS_RESULT FAIL ' + failures : 'HARNESS_RESULT PASS');
undefined;
"""


class LoginPageStorageTests(unittest.TestCase):
    """N-38, logout side: the page every logout and expiry lands on."""

    def test_login_script_names_every_cli_key(self) -> None:
        login_js, _ = extract_js.page_js(LOGIN_PAGE, delim="LOGIN")
        for key in CLI_STORAGE_KEYS:
            self.assertIn(f"'{key}'", login_js)
        cli_js, _ = extract_js.page_js(PAGE)
        used = set(re.findall(r"localStorage\.(?:getItem|setItem|removeItem)\('([^']+)'", cli_js))
        self.assertEqual(set(CLI_STORAGE_KEYS), used, "a new CLI localStorage key must also be cleared at login")

    @unittest.skipUnless(JS_ENGINE, "a JavaScript engine is required")
    def test_loading_the_login_page_clears_cli_storage(self) -> None:
        login_js, _ = extract_js.page_js(LOGIN_PAGE, delim="LOGIN")
        with tempfile.TemporaryDirectory(prefix="hw1-login-") as directory:
            extracted = pathlib.Path(directory) / "login.js"
            extracted.write_text(login_js, encoding="utf-8")
            harness = pathlib.Path(directory) / "login_harness.js"
            harness.write_text(LOGIN_HARNESS, encoding="utf-8")
            result = run_js(harness, [str(extracted)])
        lines = result.stdout.splitlines()
        self.assertIn("HARNESS_RESULT PASS", lines, result.stdout + result.stderr)
        self.assertEqual(5, len([line for line in lines if line.startswith("PASS ")]), result.stdout)


if __name__ == "__main__":
    unittest.main()
