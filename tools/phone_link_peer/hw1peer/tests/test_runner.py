"""The test runner itself: collection order, outcomes, filters and the summary."""

import sys
import types
import unittest

from hw1peer.tests import Skip, load_fixture, unhex
from hw1peer.tests.__main__ import (Outcome, Report, Result, discover, module_wanted, run_one, summarise, tests_of,
                                   wanted)


def _module_with(source: str) -> types.ModuleType:
    module = types.ModuleType("hw1peer.tests.synthetic_module")
    exec(compile(source, "synthetic_module.py", "exec"), module.__dict__)
    return module


def test_discover_finds_this_module():
    names = discover()
    assert "hw1peer.tests.test_runner" in names
    assert all(n.rsplit(".", 1)[1].startswith("test_") for n in names)
    assert names == sorted(names)


def test_tests_of_keeps_source_order_and_own_functions():
    module = _module_with(
        "from hw1peer.tests import unhex as test_imported\n"
        "def test_b(): pass\n"
        "def helper(): pass\n"
        "def test_a(): pass\n"
        "test_value = 3\n")
    assert [name for name, _ in tests_of(module)] == ["test_b", "test_a"]


def test_run_one_outcomes():
    def passes() -> None:
        pass

    def fails() -> None:
        assert 1 == 2, "boom"

    def errors() -> None:
        raise KeyError("missing")

    def skips() -> None:
        raise Skip("no radio")

    def skips_unittest() -> None:
        raise unittest.SkipTest("later")

    assert run_one("p", passes).outcome is Outcome.PASS
    failed = run_one("f", fails)
    assert failed.outcome is Outcome.FAIL and "boom" in failed.detail
    errored = run_one("e", errors)
    assert errored.outcome is Outcome.ERROR and "KeyError" in errored.detail
    assert run_one("s", skips).detail == "no radio" and run_one("s", skips).outcome is Outcome.SKIP
    assert run_one("u", skips_unittest).outcome is Outcome.SKIP


def test_filters_and_summary():
    assert wanted("test_channel.test_x1", []) and wanted("test_channel.test_x1", ["x2", "x1"])
    assert not wanted("test_channel.test_x1", ["paircode"])
    assert module_wanted("test_channel", []) and module_wanted("test_channel", ["chan"])
    assert module_wanted("test_channel", ["test_channel.test_x1"]) and not module_wanted("test_channel", ["x1"])
    assert summarise(Report([Result("a", Outcome.FAIL)])).startswith("1 test: 0 passed, 1 failed")
    report = Report([Result("a", Outcome.PASS, 0.5), Result("b", Outcome.SKIP, 0.25)])
    assert report.ok and summarise(report) == "2 tests: 1 passed, 0 failed, 0 errors, 1 skipped in 0.8s"
    assert not Report([]).ok, "nothing collected is not a pass"
    assert not Report([Result("a", Outcome.PASS), Result("c", Outcome.ERROR)]).ok


def test_fixture_helpers():
    assert unhex(None) == b"" and unhex("0a0B") == b"\x0a\x0b"
    assert load_fixture("phone_secure_channel_v1_pipe_v2_vectors.json")["label_ascii"] == "HW1-PIPE-v1"
    assert sys.version_info >= (3, 14), "the package targets Python 3.14"
