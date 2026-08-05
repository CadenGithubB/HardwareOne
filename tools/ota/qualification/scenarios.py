"""Reviewable staged-path contracts for STG-001 through STG-018."""

from __future__ import annotations

from .model import Scenario, ScenarioStep


COMMON_PREREQUISITES = (
    "explicit disposable-device acknowledgement",
    "unambiguous USB serial port and expected ESP32-S3 identity",
    "paired main/updater build audit for the selected board",
    "artifact board/layout/version/digest identity recorded",
    "pending terminal result reviewed before cleanup or acknowledgement",
)


def step(
    step_id: str,
    action: str,
    expected: str,
    *,
    mutates: bool = False,
) -> ScenarioStep:
    return ScenarioStep(step_id, action, expected, mutates)


def staged_case(
    number: int,
    title: str,
    fixture: str,
    required: str,
    steps: tuple[ScenarioStep, ...],
    *,
    destructive: bool = False,
    execution: str = "ordinary-firmware",
    extra_prerequisites: tuple[str, ...] = (),
) -> Scenario:
    return Scenario(
        case_id=f"STG-{number:03d}",
        title=title,
        category="staged",
        fixture=fixture,
        required_result=required,
        destructive=destructive,
        execution=execution,
        prerequisites=COMMON_PREREQUISITES + extra_prerequisites,
        steps=steps,
    )


SCENARIOS = (
    staged_case(
        1,
        "candidate part only",
        "valid-control image without manifest.part",
        "otastage refuses; installed main and journal remain safe",
        (
            step("baseline", "capture otastatus json and storage stats", "terminal/idle baseline is recorded"),
            step("upload", "upload only /system/ota/candidate.part", "exact candidate size is listed", mutates=True),
            step("stage", "run otastage confirm", "command refuses missing manifest.part", mutates=True),
            step("verify", "recapture status and boot identity", "no recovery arm, write, or candidate journal mutation"),
        ),
    ),
    staged_case(
        2,
        "manifest part only",
        "valid-control manifest without candidate.part",
        "otastage refuses before recovery or write",
        (
            step("baseline", "capture baseline", "installed main and journal identity recorded"),
            step("upload", "upload only /system/ota/manifest.part", "exact manifest size is listed", mutates=True),
            step("stage", "run otastage confirm", "command refuses missing candidate.part", mutates=True),
            step("verify", "recapture durable status", "no recovery arm or image write"),
        ),
    ),
    staged_case(
        3,
        "truncated candidate",
        "truncated-image",
        "size or digest validation refuses before recovery or write",
        (
            step("upload", "upload truncated candidate.part and its paired manifest.part", "both files are present", mutates=True),
            step("stage", "run otastage confirm", "candidate validation fails", mutates=True),
            step("verify", "compare journal/boot state with baseline", "main remains selected and no transaction is armed"),
        ),
    ),
    staged_case(
        4,
        "truncated manifest",
        "truncated-manifest",
        "parse, length, or signature validation refuses before write",
        (
            step("upload", "upload valid candidate.part and truncated manifest.part", "both files are present", mutates=True),
            step("stage", "run otastage confirm", "manifest validation fails", mutates=True),
            step("verify", "compare durable state with baseline", "no transaction is armed"),
        ),
    ),
    staged_case(
        5,
        "same-size binary and manifest mismatch",
        "same-size-digest-mismatch",
        "digest validation refuses before promotion and journal mutation",
        (
            step("upload", "upload changed same-size candidate and original valid manifest", "both parts have expected sizes", mutates=True),
            step("stage", "run otastage confirm", "digest mismatch is reported", mutates=True),
            step("verify", "inspect promoted paths and journal", "no REQUESTED transaction exists for the mismatched pair"),
        ),
    ),
    staged_case(
        6,
        "invalid detached manifest signature",
        "invalid-manifest-signature",
        "signature validation refuses before promotion and journal mutation",
        (
            step("upload", "upload valid image and signature-mutated manifest", "both parts have expected sizes", mutates=True),
            step("stage", "run otastage confirm", "RSA-PSS verification fails", mutates=True),
            step("verify", "inspect boot/journal state", "no recovery arm or image write"),
        ),
    ),
    staged_case(
        7,
        "wrong board identity",
        "wrong-board",
        "main rejects the signed artifact before write",
        (
            step("upload", "upload image and correctly lab-signed wrong-board manifest", "parts are present", mutates=True),
            step("stage", "run otastage confirm", "board mismatch is reported", mutates=True),
            step("verify", "inspect journal", "no candidate is journaled"),
        ),
    ),
    staged_case(
        8,
        "wrong layout identity",
        "wrong-layout",
        "main rejects the signed artifact before write",
        (
            step("upload", "upload image and correctly lab-signed wrong-layout manifest", "parts are present", mutates=True),
            step("stage", "run otastage confirm", "layout mismatch is reported", mutates=True),
            step("verify", "inspect journal", "no candidate is journaled"),
        ),
    ),
    staged_case(
        9,
        "unsupported data schema",
        "unsupported-schema",
        "schema policy refuses before write",
        (
            step("upload", "upload image and signed unsupported-schema manifest", "parts are present", mutates=True),
            step("stage", "run otastage confirm", "schema mismatch is reported", mutates=True),
            step("verify", "inspect durable state", "main remains selected"),
        ),
    ),
    staged_case(
        10,
        "minimum updater too new",
        "min-updater-too-new",
        "updater compatibility policy refuses before write",
        (
            step("upload", "upload image and signed too-new minimum-updater manifest", "parts are present", mutates=True),
            step("stage", "run otastage confirm", "minimum-updater mismatch is reported", mutates=True),
            step("verify", "inspect durable state", "recovery is not armed"),
        ),
    ),
    staged_case(
        11,
        "pending result blocks ordinary staging",
        "valid-control",
        "new normal staging is refused without replacing the pending result",
        (
            step("baseline", "establish or retain an unacknowledged terminal result", "resultPending is true", mutates=True),
            step("upload", "upload a valid .part pair", "parts are present", mutates=True),
            step("stage", "run otastage confirm", "command requires exact result review/acknowledgement", mutates=True),
            step("verify", "compare result identity", "result sequence/code/detail are unchanged"),
        ),
        destructive=True,
    ),
    staged_case(
        12,
        "valid pair stages",
        "valid-control",
        "exact pair is promoted and REQUESTED records its candidate identity",
        (
            step("baseline", "capture terminal/idle status and free space", "preconditions are safe"),
            step("upload", "upload exact candidate.part then manifest.part", "sizes match fixture metadata", mutates=True),
            step("stage", "run otastage confirm", "command reports staged version and size", mutates=True),
            step("verify", "run otastatus json and list /system/ota", "phase is requested and promoted pair replaces .part files"),
        ),
    ),
    staged_case(
        13,
        "valid staged apply",
        "valid-control",
        "factory revalidates/writes and exact candidate becomes a trial",
        (
            step("stage", "complete STG-012", "phase is requested", mutates=True),
            step("arm", "run otaupdate confirm after credential/power preflight", "recovery boot is durably armed", mutates=True),
            step("recover", "observe factory recovery revalidate and apply", "candidate identity matches the journal", mutates=True),
            step("trial", "observe main boot", "exact candidate is rollback-pending/trial running"),
        ),
        destructive=True,
        extra_prerequisites=("known-good direct recovery repair pair available",),
    ),
    staged_case(
        14,
        "probation succeeds",
        "valid-control",
        "durable success result matches the installed candidate",
        (
            step("trial", "begin from STG-013 exact trial", "trial identity is recorded"),
            step("wait", "observe uninterrupted probation and complete-loop heartbeats", "continuous health interval completes"),
            step("verify", "query otastatus json after mark-valid", "phase/result are succeeded and identity matches"),
        ),
        destructive=True,
    ),
    staged_case(
        15,
        "exact result acknowledgement",
        "valid-control",
        "only the exact current result sequence clears pending state",
        (
            step("baseline", "capture pending nonzero result sequence", "resultPending is true"),
            step("wrong", "run otaack with stale or adjacent sequence", "acknowledgement is refused and result stays pending", mutates=True),
            step("exact", "run otaack with exact sequence and confirm", "acknowledgement is committed", mutates=True),
            step("verify", "query status", "resultPending is false while result remains reviewable"),
        ),
    ),
    staged_case(
        16,
        "successful staged cleanup",
        "valid-control",
        "promoted OTA files are removed after success; unrelated files remain",
        (
            step("sentinel", "record unrelated file roster/hash before update", "sentinels are stable"),
            step("update", "complete STG-012 through STG-014", "success is durable", mutates=True),
            step("verify", "list /system/ota and recheck sentinels", "promoted files are gone and unrelated files are unchanged"),
        ),
        destructive=True,
    ),
    staged_case(
        17,
        "re-upload after interrupted part upload",
        "truncated-image then valid-control",
        "fresh exact pair stages without formatting or broad deletion",
        (
            step("interrupt", "terminate candidate.part upload after nonzero bytes", "upload fails or partial file remains", mutates=True),
            step("refuse", "run otastage confirm", "incomplete pair is refused", mutates=True),
            step("replace", "upload both exact valid .part files", "old partial content is replaced", mutates=True),
            step("stage", "run otastage confirm and inspect status", "valid exact candidate reaches REQUESTED", mutates=True),
        ),
        extra_prerequisites=("transport supports bounded upload interruption",),
    ),
    staged_case(
        18,
        "candidate mutation after stage before apply",
        "valid-control followed by same-size-digest-mismatch",
        "launch-time revalidation fails and no unrelated image executes",
        (
            step("stage", "complete STG-012", "valid candidate is REQUESTED", mutates=True),
            step("mutate", "replace one promoted file through the authorized test path", "stored pair no longer matches journal", mutates=True),
            step("apply", "run otaupdate confirm", "launch-time revalidation fails and terminal failure is recorded", mutates=True),
            step("verify", "inspect boot partition, journal, and installed version", "factory was not armed for the changed candidate"),
        ),
        destructive=True,
        extra_prerequisites=("review confirms promoted-path mutation is permitted by current admin file policy",),
    ),
)


_BY_ID = {scenario.case_id: scenario for scenario in SCENARIOS}
if len(_BY_ID) != 18:
    raise RuntimeError("staged qualification scenario IDs are not unique/complete")


def get_scenario(case_id: str) -> Scenario:
    try:
        return _BY_ID[case_id.upper()]
    except KeyError as exc:
        raise KeyError(f"unknown staged qualification case {case_id!r}") from exc
