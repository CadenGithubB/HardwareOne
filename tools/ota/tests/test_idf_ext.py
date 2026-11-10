"""The project idf.py extension refuses a full erase on a recovery-OTA build."""
from __future__ import annotations

import importlib.util
import os
import pathlib
import re
import sys
import tempfile
import types
import unittest
from unittest import mock


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]


def load_extension() -> types.ModuleType:
    spec = importlib.util.spec_from_file_location("hw1_idf_ext", REPOSITORY / "idf_ext.py")
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


idf_ext = load_extension()


class FatalError(RuntimeError):
    pass


class Args:
    def __init__(self, build_dir: str) -> None:
        self.build_dir = build_dir


class Context:
    info_name = "idf.py"


class FullEraseGuardTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="hw1-idf-ext-")
        self.addCleanup(self.temporary.cleanup)
        self.build_dir = self.temporary.name
        self.configured: list[str] = []
        self.erased: list[tuple] = []
        tools = types.ModuleType("idf_py_actions.tools")
        tools.ensure_build_directory = lambda args, name: self.configured.append(args.build_dir)
        errors = types.ModuleType("idf_py_actions.errors")
        errors.FatalError = FatalError
        package = types.ModuleType("idf_py_actions")
        patcher = mock.patch.dict(sys.modules, {
            "idf_py_actions": package, "idf_py_actions.tools": tools, "idf_py_actions.errors": errors})
        patcher.start()
        self.addCleanup(patcher.stop)

    def write_mode(self, mode: str) -> None:
        pathlib.Path(self.build_dir, idf_ext.LAYOUT_MODE_FILE).write_text(mode + "\n")

    def stock(self, action, ctx, args, **action_args) -> None:
        self.erased.append((action, args.build_dir, action_args))

    def actions(self) -> dict:
        base = {"actions": {
            "erase-flash": {"callback": self.stock, "help": "Erase entire flash chip.", "options": ["port"]},
            "erase_flash": {"callback": self.stock, "hidden": True},
            "flash": {"callback": self.stock},
        }}
        return idf_ext.action_extensions(base, str(REPOSITORY))["actions"]

    def run_erase(self, name: str = "erase-flash", override: str | None = None) -> None:
        environ = dict(os.environ)
        environ.pop(idf_ext.OVERRIDE_VARIABLE, None)
        if override is not None:
            environ[idf_ext.OVERRIDE_VARIABLE] = override
        with mock.patch.dict(os.environ, environ, clear=True):
            self.actions()[name]["callback"](name, Context(), Args(self.build_dir))

    def test_only_the_erase_actions_are_wrapped_and_keep_their_fields(self) -> None:
        actions = self.actions()
        self.assertEqual(sorted(actions), ["erase-flash", "erase_flash"])
        self.assertEqual(actions["erase-flash"]["help"], "Erase entire flash chip.")
        self.assertEqual(actions["erase-flash"]["options"], ["port"])
        self.assertTrue(actions["erase_flash"]["hidden"])
        self.assertEqual(idf_ext.action_extensions({"actions": {}}, "")["actions"], {})

    def test_recovery_layouts_are_refused_under_both_names(self) -> None:
        for mode in ("recovery-ota", "deployment:headless:hw1-hl-f3-ota-v1"):
            self.write_mode(mode)
            for name in idf_ext.ERASE_ACTIONS:
                with self.assertRaises(FatalError) as refused:
                    self.run_erase(name)
                text = str(refused.exception)
                self.assertIn(mode, text)
                self.assertIn("littlefs-flash", text)
                self.assertIn("factory-flash", text)
                self.assertIn(idf_ext.OVERRIDE_VARIABLE + "=1", text)
        self.assertEqual(self.erased, [])
        self.assertEqual(len(self.configured), 4)  # configured before the check, every time

    def test_other_layouts_erase_as_before(self) -> None:
        for mode in ("ordinary", "deployment:handheld:factory"):
            self.write_mode(mode)
            self.run_erase()
        self.assertEqual([entry[0] for entry in self.erased], ["erase-flash", "erase-flash"])

    def test_a_directory_without_a_layout_file_is_not_refused(self) -> None:
        self.run_erase()
        self.assertEqual(len(self.erased), 1)
        self.write_mode("")
        self.run_erase()
        self.assertEqual(len(self.erased), 2)

    def test_the_override_must_be_exactly_one(self) -> None:
        self.write_mode("recovery-ota")
        for value in ("0", "yes", "true", " 1"):
            with self.assertRaises(FatalError):
                self.run_erase(override=value)
        self.assertEqual(self.erased, [])
        self.run_erase(override="1")
        self.assertEqual(len(self.erased), 1)

    def test_action_arguments_reach_the_stock_action(self) -> None:
        self.write_mode("ordinary")
        self.actions()["erase-flash"]["callback"]("erase-flash", Context(), Args(self.build_dir), extra=7)
        self.assertEqual(self.erased[0][2], {"extra": 7})


class SourceAgreementTests(unittest.TestCase):
    def test_the_modes_match_the_ones_the_build_writes(self) -> None:
        cmake = (REPOSITORY / "CMakeLists.txt").read_text()
        written = set(re.findall(r'set\(_hw1_layout_mode "([^"]+)"\)', cmake))
        self.assertEqual(written, {
            "ordinary", "recovery-ota",
            "deployment:${_hw_deployment}:${_ota_layout_id}", "deployment:${_hw_deployment}:factory"})
        self.assertIn('"${CMAKE_BINARY_DIR}/' + idf_ext.LAYOUT_MODE_FILE + '"', cmake)
        self.assertFalse(idf_ext.is_recovery_layout("ordinary"))
        self.assertFalse(idf_ext.is_recovery_layout(None))
        self.assertTrue(idf_ext.is_recovery_layout("recovery-ota"))

    def test_the_installed_idf_still_runs_the_erase_outside_cmake(self) -> None:
        idf_path = os.environ.get("IDF_PATH", "")
        serial = pathlib.Path(idf_path, "tools", "idf_py_actions", "serial_ext.py")
        if not idf_path or not serial.is_file():
            self.skipTest("IDF_PATH is not set")
        source = serial.read_text()
        for name in idf_ext.ERASE_ACTIONS:
            self.assertRegex(source, rf"'{name}': \{{\s*'callback': erase_flash,")
        self.assertIn("project_dir, 'idf_ext.py'", pathlib.Path(idf_path, "tools", "idf.py").read_text())


if __name__ == "__main__":
    unittest.main()
