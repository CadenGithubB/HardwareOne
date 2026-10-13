"""The Settings Users panel ranks userlist rows by `role` alone.

`userlist json` (System_User.cpp, userListJsonVisit) emits id, username, role,
banned and createdAt per account and nothing else. This checks that the set of
emitted fields is exactly that and that the panel's comment names each one,
that no comment still says the list copies users.json verbatim, and that
userRankOf has no `isAdmin` fallback. With a JavaScript engine present it also
runs the shipping userRankOf: every role maps to its rank, and an `isAdmin`
field on a row does not raise it. It does not render the panel or talk to a
device.
"""
from __future__ import annotations

import json
from pathlib import Path
import re
import tempfile
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js

ROOT = Path(__file__).resolve().parents[3]
SETTINGS = ROOT / "components/hardwareone/WebPage_Settings.h"
USER = ROOT / "components/hardwareone/System_User.cpp"
RANK_MARKER = "window.userRankOf = function(u) {"
EMITTED = {"id", "username", "role", "banned", "createdAt"}

HARNESS = r"""
var window = {};
eval(slurp(__argv[0]));
var cases = JSON.parse(slurp(__argv[1]));
var failures = [];
cases.forEach(function(c) {
  var got = window.userRankOf(c.row);
  if (got !== c.rank) failures.push(JSON.stringify(c.row) + ' -> ' + got + ' != ' + c.rank);
});
if (failures.length) throw new Error(failures.join('\n'));
__out('USERRANK PASS');
"""


def block(text: str, marker: str) -> str:
    start = text.index(marker)
    depth = 0
    for pos in range(text.index("{", start), len(text)):
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
            if depth == 0:
                return text[start:pos + 1]
    raise AssertionError(f"unterminated block: {marker}")


class SettingsUserlistTest(unittest.TestCase):
    def setUp(self) -> None:
        self.settings = SETTINGS.read_text(encoding="utf-8")
        self.user = USER.read_text(encoding="utf-8")
        self.assertEqual(self.settings.count(RANK_MARKER), 1, "one userRankOf")
        self.rank = block(self.settings, RANK_MARKER)

    def test_emitted_fields(self) -> None:
        marker = "static void userListJsonVisit("
        self.assertEqual(self.user.count(marker), 1, "userlist json has one row writer")
        fields = set(re.findall(r'row\["(\w+)"\]', block(self.user, marker)))
        self.assertEqual(fields, EMITTED)
        start = self.settings.rindex("/*", 0, self.settings.index(RANK_MARKER))
        comment = self.settings[start:self.settings.index(RANK_MARKER)]
        for field in sorted(EMITTED):
            self.assertIn(field, comment, f"the userRankOf comment names {field}")

    def test_no_stale_comments_or_fallback(self) -> None:
        self.assertNotIn("isAdmin", self.rank)
        self.assertNotIn("verbatim", self.settings)
        self.assertNotIn("userlist json may not even", self.settings)
        # Rows of the user list carry no isAdmin field (settings' s.user does).
        self.assertIsNone(re.search(r"(?<![.\w])(?:u|user)\.isAdmin\b", self.settings))

    @unittest.skipIf(JS_ENGINE is None, "no JavaScript engine available")
    def test_rank_runs_on_role_only(self) -> None:
        cases = [
            {"row": {"role": "superadmin"}, "rank": 3},
            {"row": {"role": "admin"}, "rank": 2},
            {"row": {"role": "ADMIN"}, "rank": 2},
            {"row": {"role": "user"}, "rank": 1},
            {"row": {"role": "guest"}, "rank": 0},
            {"row": {"role": "user", "isAdmin": True}, "rank": 1},
            {"row": {"role": "guest", "isAdmin": True}, "rank": 0},
            {"row": {"isAdmin": True}, "rank": 1},
            {"row": {}, "rank": 1},
        ]
        with tempfile.TemporaryDirectory(prefix="hw1_userrank_") as directory:
            folder = Path(directory)
            (folder / "rank.js").write_text(self.rank + ";\n", encoding="utf-8")
            (folder / "cases.json").write_text(json.dumps(cases), encoding="utf-8")
            (folder / "harness.js").write_text(HARNESS, encoding="utf-8")
            result = run_js(folder / "harness.js", [folder / "rank.js", folder / "cases.json"],
                            read_roots=[folder])
        self.assertIn("USERRANK PASS", result.stdout.splitlines(),
                      f"rc={result.returncode}\nstdout={result.stdout}\nstderr={result.stderr}")


if __name__ == "__main__":
    unittest.main()
