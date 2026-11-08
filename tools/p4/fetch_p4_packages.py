#!/usr/bin/env python3
"""Fetch the pristine ESP-DL and ESP-Hosted packages the P4 overrides start from.

Usage: python3 -B fetch_p4_packages.py      # with ESP-IDF 5.5.5 exported

prepare_esp_dl.py and prepare_esp_hosted.py patch the exact registry packages
dependencies.lock.esp32p4 pins. A checkout that has built for P4 before finds
them in managed_components/ or the component manager cache, but a fresh
machine has neither, and the project itself cannot fetch them: the top-level
CMakeLists.txt stops before project() until the overrides exist, and
components/hardwareone/idf_component.yml points both dependencies at the
override directories (override_path), so the component manager never
downloads them for this project.

For each package whose override is not staged yet and whose pristine copy is
not found locally, this writes a throwaway one-component project in
.sdk-overrides/registry-fetch/ (ignored by git) that depends on exactly the
pinned versions, and runs the component manager there:

  idf.py -C .sdk-overrides/registry-fetch -B .sdk-overrides/registry-fetch/build \
         -DIDF_TARGET=esp32p4 update-dependencies

That configures the throwaway project (no firmware is compiled) and leaves the
packages in .sdk-overrides/registry-fetch/managed_components/, where the
prepare scripts look; they still verify each tree against its pinned digest
before patching. Nothing is fetched when every package is already available.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))
import prepare_esp_dl  # noqa: E402
import prepare_esp_hosted  # noqa: E402

REPO = ROOT.parent.parent
PACKAGES = (prepare_esp_dl, prepare_esp_hosted)
FETCH_DIR = REPO / ".sdk-overrides/registry-fetch"


def registry_name(package) -> str:
    return package.NAME.replace("__", "/", 1)


def needed(packages=PACKAGES) -> list:
    """Packages with no staged override and no pristine copy found locally."""
    result = []
    for package in packages:
        if (package.REPO / ".sdk-overrides/esp32p4" / package.NAME).exists():
            continue
        try:
            package.find_source(None)
        except ValueError:
            result.append(package)
    return result


def write_project(work: Path, packages) -> None:
    (work / "main").mkdir(parents=True, exist_ok=True)
    (work / "CMakeLists.txt").write_text(
        "# Written by tools/p4/fetch_p4_packages.py: fetches pinned registry packages only.\n"
        "cmake_minimum_required(VERSION 3.16)\n"
        "set(COMPONENTS main)\n"
        "include($ENV{IDF_PATH}/tools/cmake/project.cmake)\n"
        "project(hw1_p4_registry_fetch)\n")
    (work / "main/CMakeLists.txt").write_text("idf_component_register()\n")
    lines = ["dependencies:"]
    for package in packages:
        lines.append(f'  {registry_name(package)}:\n    version: "=={package.VERSION}"')
    (work / "main/idf_component.yml").write_text("\n".join(lines) + "\n")


def fetch_command(work: Path, idf: Path) -> list[str]:
    return [sys.executable, str(idf / "tools/idf.py"), "-C", str(work), "-B", str(work / "build"),
            "-DIDF_TARGET=esp32p4", "update-dependencies"]


def manual_remedy(packages) -> str:
    steps = "; ".join(f"python3 -B tools/p4/{Path(p.__file__).name} --source DIR/{p.NAME}" for p in packages)
    names = ", ".join(f"{registry_name(p)} {p.VERSION}" for p in packages)
    return (f"Check access to components.espressif.com and rerun tools/p4/prepare_sdk.sh. Offline, "
            f"download {names} from components.espressif.com, unpack each and run: {steps}; "
            "then rerun tools/p4/prepare_sdk.sh")


def fetch(packages, idf: Path, work: Path = FETCH_DIR, run=subprocess.run) -> list[Path]:
    """Fetch the given packages into work/managed_components; return their verified sources."""
    if not packages:
        return []
    if not (idf / "tools/idf.py").is_file():
        raise ValueError(f"{idf}/tools/idf.py not found: export ESP-IDF 5.5.5 first")
    write_project(work, packages)
    names = ", ".join(f"{registry_name(p)} {p.VERSION}" for p in packages)
    print(f"Fetching {names} from the ESP Component Registry into {work}", flush=True)
    result = run(fetch_command(work, idf))
    sources, problems = [], []
    for package in packages:
        fetched = work / "managed_components" / package.NAME
        if not (fetched / "idf_component.yml").is_file():
            problems.append(f"the component manager left no {fetched}. " + manual_remedy([package]))
        elif package.tree_digest(fetched) != package.SOURCE_TREE_SHA256:
            problems.append(f"fetched {registry_name(package)} {package.VERSION} in {fetched} does not "
                            "match the pinned source digest; remove it and rerun, or "
                            + manual_remedy([package]))
        else:
            sources.append(fetched)
    if result.returncode and problems:
        raise ValueError(f"idf.py update-dependencies in {work} exited {result.returncode} (output above). "
                         + manual_remedy(packages))
    if problems:
        raise ValueError(problems[0])
    if result.returncode:
        # update-dependencies downloads first, then reconfigures the throwaway
        # project; a failure in that configure step does not affect the packages,
        # which have just been verified against their pinned digests.
        print(f"fetch_p4_packages: idf.py exited {result.returncode} after the download; the fetched "
              "packages match their pinned digests, so they are used", file=sys.stderr, flush=True)
    return sources


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--idf", type=Path, default=os.environ.get("IDF_PATH"),
                        help="the exported ESP-IDF 5.5.5; defaults to IDF_PATH")
    args = parser.parse_args()
    try:
        missing = needed()
        if not missing:
            print("ESP-DL and ESP-Hosted sources: staged or available locally, nothing to fetch")
            return
        if args.idf is None:
            raise ValueError("export ESP-IDF 5.5.5 (IDF_PATH) first")
        for source in fetch(missing, args.idf):
            print(f"Fetched pristine package: {source}")
    except (ValueError, OSError) as exc:
        parser.exit(1, f"fetch_p4_packages: {exc}\n")


if __name__ == "__main__":
    main()
