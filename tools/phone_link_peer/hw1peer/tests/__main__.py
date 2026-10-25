"""The hw1peer test runner: ``python -m hw1peer.tests [-v] [-x] [filter ...]``.

Discovers ``hw1peer/tests/test_*.py``, runs every ``test_*`` function in source
order and prints a summary. A filter keeps tests whose ``module.function`` name
contains it (any filter matches). Exit status: 0 when everything ran passed or
skipped, 1 on any failure, error or import error, or when nothing was collected.
"""

import argparse
import importlib
import inspect
import pkgutil
import sys
import time
import traceback
import unittest
from collections.abc import Callable
from dataclasses import dataclass, field
from enum import Enum
from types import ModuleType

import hw1peer.tests as package
from hw1peer.tests import Skip


class Outcome(Enum):
    PASS = "ok"
    FAIL = "FAIL"
    ERROR = "ERROR"
    SKIP = "skip"


@dataclass
class Result:
    name: str
    outcome: Outcome
    seconds: float = 0.0
    detail: str = ""


@dataclass
class Report:
    results: list[Result] = field(default_factory=list)

    def count(self, outcome: Outcome) -> int:
        return sum(r.outcome is outcome for r in self.results)

    @property
    def ok(self) -> bool:
        return bool(self.results) and not (self.count(Outcome.FAIL) or self.count(Outcome.ERROR))


def discover() -> list[str]:
    """Names of the test modules in this package, sorted."""
    return sorted(f"{package.__name__}.{info.name}" for info in pkgutil.iter_modules(package.__path__)
                  if info.name.startswith("test_") and not info.ispkg)


def tests_of(module: ModuleType) -> list[tuple[str, Callable[[], object]]]:
    """The module's own ``test_*`` functions in source order."""
    found = [(name, obj) for name, obj in vars(module).items()
             if name.startswith("test_") and inspect.isfunction(obj) and obj.__module__ == module.__name__]
    return sorted(found, key=lambda item: item[1].__code__.co_firstlineno)


def run_one(name: str, function: Callable[[], object]) -> Result:
    start = time.perf_counter()
    try:
        function()
    except (Skip, unittest.SkipTest) as skip:
        return Result(name, Outcome.SKIP, time.perf_counter() - start, str(skip))
    except AssertionError:
        return Result(name, Outcome.FAIL, time.perf_counter() - start, traceback.format_exc())
    except Exception:  # noqa: BLE001 - a test's crash is reported, not propagated
        return Result(name, Outcome.ERROR, time.perf_counter() - start, traceback.format_exc())
    return Result(name, Outcome.PASS, time.perf_counter() - start)


def wanted(name: str, filters: list[str]) -> bool:
    return not filters or any(f in name for f in filters)


def module_wanted(short: str, filters: list[str]) -> bool:
    """Whether a module's import failure is reported: a filter that matches
    the module name, or names a test inside it ("test_x.test_y")."""
    return not filters or any(f in short or f.startswith(short + ".") for f in filters)


def run(filters: list[str], verbose: bool, stop_first: bool) -> Report:
    report = Report()
    for module_name in discover():
        short = module_name.rsplit(".", 1)[1]
        try:
            module = importlib.import_module(module_name)
        except Exception:  # noqa: BLE001 - an import failure is one error, the rest still run
            if module_wanted(short, filters):
                report.results.append(Result(f"{short} (import)", Outcome.ERROR, 0.0, traceback.format_exc()))
                print(f"ERROR {short}: import failed", flush=True)
                if stop_first:
                    break
            continue
        for test_name, function in tests_of(module):
            name = f"{short}.{test_name}"
            if not wanted(name, filters):
                continue
            result = run_one(name, function)
            report.results.append(result)
            if verbose or result.outcome in (Outcome.FAIL, Outcome.ERROR):
                extra = f" ({result.detail})" if result.outcome is Outcome.SKIP and result.detail else ""
                print(f"{result.outcome.value:5} {name} [{result.seconds:.2f}s]{extra}", flush=True)
            if stop_first and result.outcome in (Outcome.FAIL, Outcome.ERROR):
                return report
    return report


def summarise(report: Report) -> str:
    seconds = sum(r.seconds for r in report.results)
    total = len(report.results)
    return (f"{total} test{'' if total == 1 else 's'}: {report.count(Outcome.PASS)} passed, "
            f"{report.count(Outcome.FAIL)} failed, {report.count(Outcome.ERROR)} errors, "
            f"{report.count(Outcome.SKIP)} skipped in {seconds:.1f}s")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m hw1peer.tests", description=__doc__.splitlines()[0])
    parser.add_argument("filters", nargs="*", help="run tests whose module.function name contains any of these")
    parser.add_argument("-v", "--verbose", action="store_true", help="print every test")
    parser.add_argument("-x", "--exitfirst", action="store_true", help="stop at the first failure or error")
    args = parser.parse_args(argv)
    report = run(args.filters, args.verbose, args.exitfirst)
    for result in report.results:
        if result.outcome in (Outcome.FAIL, Outcome.ERROR):
            print(f"\n==== {result.outcome.value}: {result.name}\n{result.detail}", end="")
    print("\n" + summarise(report) if report.results else "no tests collected")
    return 0 if report.ok else 1


if __name__ == "__main__":
    sys.exit(main())
