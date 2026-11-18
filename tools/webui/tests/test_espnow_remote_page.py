"""The ESP-NOW page's remote command and remote file browser, on the firmware's real replies.

The page's remote file browser used to return early unless the reply to
``espnowremote`` contained "Remote command sent", a phrase only the bond prefix
produces, so it stopped at the acknowledgement; the remote command form showed
that acknowledgement as a "Result:" bubble.

This test runs the shipping ``web_assets/espnow-core.js`` against a fake device
(``harness/espnow_remote_page_harness.js``). What the fake device answers is
taken from the firmware sources on every run, through
``components/hardwareone/test/host/espnow_remote_replies.py``:

* the text acknowledgement, printed by the production formatter
  ``espnowFormatRemoteAck`` compiled for the host, as the command executor
  stamps it ("OK: ...");
* the JSON acknowledgement format and every refusal ``cmd_espnow_remote``
  returns, split into the ones /api/cli delivers with HTTP 200 and the ones it
  delivers with HTTP 400.

So a reworded reply fails here instead of in a browser. The listing the fake
peer returns follows ``buildFilesListJson`` (``System_Filesystem.cpp``); a source
contract below fails if that shape or the record fields of
``/api/espnow/messages`` change.

Run from the repository root:

    python3 -B -m unittest tools.webui.tests.test_espnow_remote_page

``HW1_COMPONENT_DIR=<component directory>`` replays other production sources
(for example the tree before the fix, which fails the acknowledgement and
browser checks).
"""

from __future__ import annotations

import json
import os
import pathlib
import re
import shutil
import sys
import tempfile
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
HOST_TESTS = REPO_ROOT / "components" / "hardwareone" / "test" / "host"
COMPONENT = pathlib.Path(os.environ.get("HW1_COMPONENT_DIR") or REPO_ROOT / "components" / "hardwareone")
CORE = COMPONENT / "web_assets" / "espnow-core.js"
HARNESS = REPO_ROOT / "tools" / "webui" / "harness" / "espnow_remote_page_harness.js"
CXX = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
EXPECTED_CHECKS = 51

MAC_TEXT = "02:00:00:00:00:01"
MAC = bytes([0x02, 0x00, 0x00, 0x00, 0x00, 0x01])


def _replies():
    if str(HOST_TESTS) not in sys.path:
        sys.path.insert(0, str(HOST_TESTS))
    import espnow_remote_replies  # noqa: PLC0415  (lives with the host suites)
    return espnow_remote_replies


def peer_listing() -> str:
    """A `files json` reply as buildFilesListJson writes it, long enough to be
    stored in several 200-byte pieces."""
    rows = []
    for name, count in (("logs", 3), ("espnow", 12)):
        rows.append('{"name":"%s","type":"folder","size":"%d items","count":%d,"perms":7}' % (name, count, count))
    for index in range(9):
        rows.append('{"name":"capture-%02d-of-the-bench-run.jsonl","type":"file","size":"%d bytes","perms":7}'
                    % (index, 1000 + 37 * index))
    return '{"success":true,"dirPerms":7,"files":[' + ",".join(rows) + "]}"


def build_fixture(workdir: pathlib.Path) -> dict:
    replies = _replies()
    block = replies.remote_block(COMPONENT)
    stamp = replies.ok_stamp(COMPONENT)
    cases = [("status", MAC_TEXT, MAC, 77),
             ('files json "/logs"', "kitchen", MAC, 4000000001),
             ("espnowsend lab " + "long text " * 20, MAC_TEXT, MAC, 4294967295)]
    texts = replies.render_text_acks(COMPONENT, cases, CXX, workdir)
    acks = [{"text": replies.stamped(text, stamp), "reqId": case[3]} for text, case in zip(texts, cases)]
    http_ok, http_bad = [], []
    for text, json_text in replies.refusals(block):
        if text is not None:
            reply = replies.stamped(text.replace("%s", "kitchen"), stamp)
            (http_ok if reply.startswith(stamp) else http_bad).append(reply)
        # Several refusals share one JSON document; each is kept once, so the
        # harness's fixed slices of this list hold different replies.
        if json_text is not None and json_text not in http_ok:
            http_ok.append(json_text)
    http_bad.append("Error: Admin access required for command 'espnowremote'. Contact an administrator.")
    return {
        "mac": MAC_TEXT,
        "acks": acks,
        "jsonAck": {"text": replies.json_ack_format(block).replace("%lu", "305419896"), "reqId": 305419896},
        "refusals": http_ok,
        "httpRefusals": http_bad,
        "listing": peer_listing(),
    }


class EspNowRemoteContractTests(unittest.TestCase):
    """Source contracts; no JavaScript engine or compiler needed."""

    def test_page_does_not_wait_for_the_bond_phrase(self) -> None:
        core = CORE.read_text(encoding="utf-8")
        start = core.index("window.doRemoteExec = function")
        end = core.index("window.parseRemoteFileListing = function")
        region = core[start:end]
        self.assertNotIn("Remote command sent", region,
                         "the page matches a phrase `espnowremote` does not reply")
        self.assertIn("files json \"' + browsePath + '\"'", region,
                      "the browser must ask for `files json`: the text listing is cut to one frame")

    def test_firmware_shapes_the_fake_device_copies(self) -> None:
        filesystem = (COMPONENT / "System_Filesystem.cpp").read_text(encoding="utf-8")
        for fragment in (r'"{\"success\":true,\"dirPerms\":"', r'"0,\"files\":["', r'"{\"name\":\""',
                         r'"\",\"type\":\""', r'"folder" : "file"', r'" items\",\"count\":"', r'" bytes\""',
                         r'",\"perms\":"',
                         r'"{\"success\":false,\"error\":\"Directory not found or not accessible\"}"'):
            self.assertIn(fragment, filesystem, f"`files json` no longer writes {fragment}")
        page = (COMPONENT / "WebPage_ESPNow.cpp").read_text(encoding="utf-8")
        for fragment in (r'"\"seq\":%lu,"', r'"\"reqId\":%lu,"', r'"\"piece\":%u,\"of\":%u,"', r'"\"mac\":\"%s\","',
                         r'",\"msg\":"', r'",\"sent\":%s"'):
            self.assertIn(fragment, page, f"/api/espnow/messages no longer writes {fragment}")
        espnow = (COMPONENT / "System_ESPNow.cpp").read_text(encoding="utf-8")
        self.assertRegex(espnow, r"const size_t FRAG = 200;",
                         "a stored answer is no longer cut into 200-byte pieces (harness pieces())")
        listing = json.loads(peer_listing())
        self.assertGreater(len(peer_listing()), 600, "the fixture listing must span several pieces")
        self.assertEqual(11, len(listing["files"]))


@unittest.skipUnless(JS_ENGINE, "a JavaScript engine (node/deno/bun/qjs/jsc/osascript) is required")
@unittest.skipUnless(CXX, "a host C++ compiler is required to print the firmware's acknowledgement")
class EspNowRemotePageBehaviorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory(prefix="hw1-espnow-remote-page-")
        work = pathlib.Path(cls.temporary.name)
        cls.fixture = build_fixture(work)
        fixture_path = work / "fixture.json"
        fixture_path.write_text(json.dumps(cls.fixture), encoding="utf-8")
        core_path = work / "espnow-core.js"
        core_path.write_text(CORE.read_text(encoding="utf-8"), encoding="utf-8")
        cls.result = run_js(HARNESS, [str(core_path), str(fixture_path)], read_roots=[str(work)])
        cls.lines = cls.result.stdout.splitlines()

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temporary.cleanup()

    def test_fixture_holds_the_firmware_replies(self) -> None:
        fixture = self.fixture
        self.assertTrue(fixture["acks"][0]["text"].endswith("(match reqId 77)"), fixture["acks"][0]["text"])
        self.assertTrue(all(a["text"].endswith("(match reqId %d)" % a["reqId"]) for a in fixture["acks"]),
                        "an acknowledgement lost its request id")
        self.assertIn('"reqId":305419896', fixture["jsonAck"]["text"])
        self.assertGreaterEqual(len(fixture["refusals"]), 6)
        self.assertEqual(len(fixture["refusals"]), len(set(fixture["refusals"])),
                         "a refusal is in the list twice: the harness would run one input as two cases")
        self.assertGreaterEqual(len(fixture["httpRefusals"]), 4)
        self.assertTrue(any("Failed to send remote command" in text for text in fixture["httpRefusals"]))

    def test_harness_reported_one_verdict(self) -> None:
        verdicts = [line for line in self.lines if line.startswith("HARNESS_RESULT ")]
        self.assertEqual(1, len(verdicts),
                         f"expected one harness verdict\n{self.result.stdout}{self.result.stderr}")

    def test_all_page_behaviors_pass(self) -> None:
        verdicts = [line for line in self.lines if line.startswith("HARNESS_RESULT ")]
        if not verdicts:
            self.skipTest("verdict shape is covered separately")
        failed = [line for line in self.lines if line.startswith("FAIL ")]
        self.assertTrue(verdicts[0].startswith("HARNESS_RESULT PASS"),
                        "ESP-NOW remote page behavior regressed:\n  " + "\n  ".join(failed))

    def test_expected_number_of_checks_ran(self) -> None:
        checks = [line for line in self.lines if re.match(r"(PASS|FAIL) ", line)]
        self.assertEqual(EXPECTED_CHECKS, len(checks),
                         f"ran {len(checks)} checks, expected {EXPECTED_CHECKS}\n{self.result.stdout}")


if __name__ == "__main__":
    unittest.main()
