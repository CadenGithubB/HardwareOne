"""Typed, versioned records used by the OTA qualification runner."""

from __future__ import annotations

import dataclasses
import enum
from datetime import datetime, timezone


CHECKPOINT_FORMAT = "hardwareone-ota-qualification-checkpoint"
CHECKPOINT_VERSION = 1


class Outcome(str, enum.Enum):
    PASS = "PASS"
    FAIL = "FAIL"
    INCONCLUSIVE = "INCONCLUSIVE"
    SKIP = "SKIP"
    PROBATION_PENDING = "PROBATION_PENDING"


@dataclasses.dataclass(frozen=True)
class ScenarioStep:
    step_id: str
    action: str
    expected: str
    mutates_device: bool = False
    resume_boundary: bool = True

    def as_dict(self) -> dict[str, object]:
        return dataclasses.asdict(self)


@dataclasses.dataclass(frozen=True)
class Scenario:
    case_id: str
    title: str
    category: str
    fixture: str
    required_result: str
    destructive: bool
    execution: str
    prerequisites: tuple[str, ...]
    steps: tuple[ScenarioStep, ...]

    def as_dict(self) -> dict[str, object]:
        return {
            "caseId": self.case_id,
            "title": self.title,
            "category": self.category,
            "fixture": self.fixture,
            "requiredResult": self.required_result,
            "destructive": self.destructive,
            "execution": self.execution,
            "prerequisites": list(self.prerequisites),
            "steps": [step.as_dict() for step in self.steps],
        }


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


@dataclasses.dataclass
class Checkpoint:
    run_id: str
    case_id: str
    board: str
    layout: str
    image_sha256: str
    manifest_sha256: str
    last_completed_step: str = ""
    observed_component: str = "unknown"
    journal_phase: str = "unknown"
    journal_sequence: int = 0
    result_sequence: int = 0
    expected_transition: str = "none"
    outcome: str = ""
    updated_at: str = dataclasses.field(default_factory=utc_now)

    def as_dict(self) -> dict[str, object]:
        return {
            "format": CHECKPOINT_FORMAT,
            "formatVersion": CHECKPOINT_VERSION,
            "runId": self.run_id,
            "caseId": self.case_id,
            "board": self.board,
            "layout": self.layout,
            "imageSha256": self.image_sha256,
            "manifestSha256": self.manifest_sha256,
            "lastCompletedStep": self.last_completed_step,
            "observedComponent": self.observed_component,
            "journalPhase": self.journal_phase,
            "journalSequence": self.journal_sequence,
            "resultSequence": self.result_sequence,
            "expectedTransition": self.expected_transition,
            "outcome": self.outcome,
            "updatedAt": self.updated_at,
        }

    @classmethod
    def from_dict(cls, value: object) -> "Checkpoint":
        if not isinstance(value, dict):
            raise ValueError("checkpoint must be a JSON object")
        if (
            value.get("format") != CHECKPOINT_FORMAT
            or value.get("formatVersion") != CHECKPOINT_VERSION
        ):
            raise ValueError("unsupported qualification checkpoint format")
        strings = {
            name: value.get(key)
            for name, key in (
                ("run_id", "runId"),
                ("case_id", "caseId"),
                ("board", "board"),
                ("layout", "layout"),
                ("image_sha256", "imageSha256"),
                ("manifest_sha256", "manifestSha256"),
                ("last_completed_step", "lastCompletedStep"),
                ("observed_component", "observedComponent"),
                ("journal_phase", "journalPhase"),
                ("expected_transition", "expectedTransition"),
                ("outcome", "outcome"),
                ("updated_at", "updatedAt"),
            )
        }
        if not all(isinstance(item, str) for item in strings.values()):
            raise ValueError("checkpoint contains a malformed string field")
        if not strings["run_id"] or not strings["case_id"]:
            raise ValueError("checkpoint has no run/case identity")
        if len(strings["image_sha256"]) != 64 or len(strings["manifest_sha256"]) != 64:
            raise ValueError("checkpoint artifact digest is malformed")
        integers = {
            name: value.get(key)
            for name, key in (
                ("journal_sequence", "journalSequence"),
                ("result_sequence", "resultSequence"),
            )
        }
        if not all(
            isinstance(item, int) and not isinstance(item, bool) and item >= 0
            for item in integers.values()
        ):
            raise ValueError("checkpoint contains a malformed sequence")
        return cls(**strings, **integers)
