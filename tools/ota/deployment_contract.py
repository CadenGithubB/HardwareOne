"""Load checked-in HardwareOne deployment contracts.

The source format is intentionally the same small KEY=VALUE file consumed by
cmake/HW1Deployment.cmake.  It is data, not a shell fragment: values are never
executed or expanded.
"""

from __future__ import annotations

from dataclasses import dataclass
import pathlib
import re


REPOSITORY = pathlib.Path(__file__).resolve().parents[2]
SELECTOR_RE = re.compile(r"^([a-z0-9_]+)/([a-z0-9_]+)$")
HEX_RE = re.compile(r"^0[xX][0-9A-Fa-f]+$")


@dataclass(frozen=True)
class DeploymentContract:
    selector: str
    deployment_id: str
    board_id: str
    target: str
    flash_size: str
    layout_id: str
    version_suffix: str
    partition_csv: pathlib.Path
    feature_header: pathlib.Path
    sdkconfig_defaults: pathlib.Path | None
    main_release_max: int
    updater_release_max: int
    flash_encryption: bool
    source: pathlib.Path

    @property
    def ota_slot_size(self) -> int:
        return partition_size(self.partition_csv, "ota_0")


def contract_path(selector: str) -> pathlib.Path:
    match = SELECTOR_RE.fullmatch(selector)
    if not match:
        raise ValueError(
            "deployment selector must be <family>/<board> using lowercase "
            "letters, numbers, and underscores"
        )
    family, board = match.groups()
    return REPOSITORY / "deployments" / family / "boards" / board / "contract.conf"


def _parse(path: pathlib.Path) -> dict[str, str]:
    if not path.is_file():
        raise ValueError(f"deployment contract not found: {path}")
    values: dict[str, str] = {}
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise ValueError(f"{path}:{number}: expected KEY=VALUE")
        key, value = line.split("=", 1)
        if not re.fullmatch(r"[A-Z][A-Z0-9_]*", key) or not value:
            raise ValueError(f"{path}:{number}: invalid contract record")
        if key in values:
            raise ValueError(f"{path}:{number}: duplicate {key}")
        values[key] = value
    return values


def load(selector: str) -> DeploymentContract:
    source = contract_path(selector)
    values = _parse(source)
    required = {
        "DEPLOYMENT_ID",
        "BOARD_ID",
        "TARGET",
        "FLASH_SIZE",
        "OTA_LAYOUT",
        "OTA_LAYOUT_ID",
        "OTA_VERSION_SUFFIX",
        "PARTITION_CSV",
        "FEATURE_HEADER",
        "MAIN_RELEASE_MAX",
        "UPDATER_RELEASE_MAX",
        "FLASH_ENCRYPTION",
    }
    # Keys a contract MAY declare. An sdkconfig overlay is the only supported
    # way for a deployment to state policy that contradicts its board file --
    # see cmake/HW1Deployment.cmake and the root CMakeLists for where it is
    # layered. Contracts that need no such override simply omit it.
    optional = {
        "SDKCONFIG_DEFAULTS",
    }
    missing = sorted(required - values.keys())
    unknown = sorted(values.keys() - required - optional)
    if missing or unknown:
        details = []
        if missing:
            details.append("missing " + ", ".join(missing))
        if unknown:
            details.append("unknown " + ", ".join(unknown))
        raise ValueError(f"{source}: " + "; ".join(details))

    family, board = selector.split("/", 1)
    if values["DEPLOYMENT_ID"] != family or values["BOARD_ID"] != board:
        raise ValueError(f"{source}: selector and deployment/board identity disagree")
    if values["OTA_LAYOUT"] != "1":
        raise ValueError(f"{source}: OTA_LAYOUT must be 1")
    if values["FLASH_ENCRYPTION"] not in {"0", "1"}:
        raise ValueError(f"{source}: FLASH_ENCRYPTION must be 0 or 1")
    for key in ("MAIN_RELEASE_MAX", "UPDATER_RELEASE_MAX"):
        if not HEX_RE.fullmatch(values[key]):
            raise ValueError(f"{source}: {key} must be hexadecimal")

    partition_csv = (REPOSITORY / values["PARTITION_CSV"]).resolve()
    feature_header = (REPOSITORY / values["FEATURE_HEADER"]).resolve()
    referenced = [partition_csv, feature_header]
    sdkconfig_defaults: pathlib.Path | None = None
    if "SDKCONFIG_DEFAULTS" in values:
        sdkconfig_defaults = (REPOSITORY / values["SDKCONFIG_DEFAULTS"]).resolve()
        referenced.append(sdkconfig_defaults)
    for required_file in referenced:
        if not required_file.is_file():
            raise ValueError(f"{source}: referenced file not found: {required_file}")

    contract = DeploymentContract(
        selector=selector,
        deployment_id=family,
        board_id=board,
        target=values["TARGET"],
        flash_size=values["FLASH_SIZE"],
        layout_id=values["OTA_LAYOUT_ID"],
        version_suffix="+" + values["OTA_VERSION_SUFFIX"].removeprefix("+"),
        partition_csv=partition_csv,
        feature_header=feature_header,
        sdkconfig_defaults=sdkconfig_defaults,
        main_release_max=int(values["MAIN_RELEASE_MAX"], 0),
        updater_release_max=int(values["UPDATER_RELEASE_MAX"], 0),
        flash_encryption=values["FLASH_ENCRYPTION"] == "1",
        source=source,
    )
    if contract.main_release_max > contract.ota_slot_size:
        raise ValueError(f"{source}: MAIN_RELEASE_MAX exceeds ota_0")
    return contract


def available() -> tuple[str, ...]:
    selectors: list[str] = []
    root = REPOSITORY / "deployments"
    if not root.is_dir():
        return ()
    for path in root.glob("*/boards/*/contract.conf"):
        selectors.append(f"{path.parents[2].name}/{path.parent.name}")
    return tuple(sorted(selectors))


def board_ids() -> tuple[str, ...]:
    """Every physical board named by at least one checked-in deployment.

    A board can reach the release tooling through a deployment contract alone,
    without a row in the legacy board-only OTA registries -- xiao_s3 is the
    first such board. The argparse --board choices are the union of the two,
    so a deployment board is spellable while a genuine typo is still rejected.
    """
    boards: set[str] = set()
    for selector in available():
        boards.add(selector.split("/", 1)[1])
    return tuple(sorted(boards))


def partition_size(path: pathlib.Path, name: str) -> int:
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        fields = [field.strip() for field in line.split(",")]
        if len(fields) >= 5 and fields[0] == name:
            return int(fields[4], 0)
    raise ValueError(f"{path} has no {name} partition")
