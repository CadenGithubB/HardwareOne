"""idf.py project extension: refuse a full-chip erase on a recovery-OTA build.

On the recovery-OTA layout the root CMakeLists.txt refuses the stock flash and
erase CMake targets (`flash`, `app-flash`, `erase-otadata`, ...), because they
would overwrite or remove the `factory` recovery updater. `idf.py erase-flash`
(and its old spelling `erase_flash`) never reaches a CMake target: idf.py runs
esptool's `erase_flash` itself (esp-idf/tools/idf_py_actions/serial_ext.py),
so that refusal did not run and the whole chip was erased, including the
recovery updater, NVS (the recovery credential) and littlefs (readiness G-65).

idf.py loads this file from the project directory and merges the actions it
returns over its own, so the two erase actions are wrapped here. The wrapper
configures the build directory the same way the stock action does, reads the
layout the directory was configured for (`hw1-layout-mode.txt`, written by the
root CMakeLists.txt) and refuses on a recovery-OTA layout unless
HW1_ALLOW_FULL_ERASE=1 is set.

Limits: the layout is that of the build directory, not of the attached device,
and a direct `esptool.py erase_flash` is outside idf.py and is not covered.

Host test: tools/ota/tests/test_idf_ext.py.
"""

from __future__ import annotations

import os
from typing import Any, Callable, Dict, Mapping, Optional

ERASE_ACTIONS = ("erase-flash", "erase_flash")
LAYOUT_MODE_FILE = "hw1-layout-mode.txt"
OVERRIDE_VARIABLE = "HW1_ALLOW_FULL_ERASE"


def layout_mode(build_dir: str) -> Optional[str]:
    """The layout mode the build directory was configured for, or None."""
    try:
        with open(os.path.join(build_dir, LAYOUT_MODE_FILE), encoding="utf-8") as handle:
            mode = handle.read().strip()
    except OSError:
        return None
    return mode or None


def is_recovery_layout(mode: Optional[str]) -> bool:
    """True for the modes that carry a `factory` recovery updater.

    The root CMakeLists.txt writes "ordinary", "recovery-ota",
    "deployment:<name>:<layout id>" (a recovery-OTA deployment) or
    "deployment:<name>:factory" (a factory-only deployment, no updater).
    """
    if mode is None or mode == "ordinary":
        return False
    if mode.startswith("deployment:"):
        return not mode.endswith(":factory")
    return True


def full_erase_refusal(build_dir: str, environ: Mapping[str, str]) -> Optional[str]:
    """The refusal text for a full erase from this build directory, or None."""
    mode = layout_mode(build_dir)
    if not is_recovery_layout(mode):
        return None
    if environ.get(OVERRIDE_VARIABLE, "") == "1":
        return None
    return (
        f"Refusing erase-flash: {build_dir} is a recovery-OTA build ({mode}).\n"
        "A full erase also removes the factory recovery updater, NVS (the recovery\n"
        "credential) and littlefs. Nothing boots until the device is provisioned\n"
        "again (docs/OTA_DEVICE_SETUP.md, steps 3 and 4).\n"
        "  Filesystem only:       littlefs-flash (encrypted-littlefs-flash on feathers3_fe)\n"
        "  Recovery updater only: factory-flash (encrypted-factory-flash on feathers3_fe)\n"
        f"  Erase everything:      set {OVERRIDE_VARIABLE}=1 and run the command again"
    )


def _guarded(stock: Callable[..., None]) -> Callable[..., None]:
    def erase_flash(action: str, ctx: Any, args: Any, **action_args: Any) -> None:
        from idf_py_actions.errors import FatalError
        from idf_py_actions.tools import ensure_build_directory

        # The stock action configures first as well; doing it here means a
        # fresh build directory has its layout file before the check.
        ensure_build_directory(args, ctx.info_name)
        refusal = full_erase_refusal(args.build_dir, os.environ)
        if refusal:
            raise FatalError(refusal)
        if is_recovery_layout(layout_mode(args.build_dir)):
            print(f"{OVERRIDE_VARIABLE}=1: erasing a recovery-OTA device, recovery updater included.")
        stock(action, ctx, args, **action_args)

    return erase_flash


def action_extensions(base_actions: Dict, project_path: str) -> Dict:
    stock_actions = base_actions.get("actions", {})
    guarded: Dict[str, Dict] = {}
    for name in ERASE_ACTIONS:
        stock = stock_actions.get(name)
        if stock and callable(stock.get("callback")):
            guarded[name] = dict(stock, callback=_guarded(stock["callback"]))
    return {"actions": guarded}
