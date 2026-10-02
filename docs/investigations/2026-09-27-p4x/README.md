# ESP32-P4X-EYE feasibility study (2026-09-27)

The investigation that preceded HardwareOne's P4X-EYE support: can one
HardwareOne application run on the ESP32-P4X-EYE, and what would it take?
It recommended staying on ESP-IDF 5.5.5 instead of migrating to IDF 6 first,
identified the ESP-SR version conflict and the missing ESP-NOW forwarding in
ESP-Hosted, and laid out the five-step plan that the `experiments/p4_*`
milestones and the `p4x_eye` board then carried out.

| File | Contents |
|---|---|
| [REPORT.md](REPORT.md) | Findings, recommendation and plan |
| [code-audit.md](code-audit.md) | Code-level findings; links point at the audited commit `6ba1c6c` |
| [experiment-results.md](experiment-results.md) | Build and dependency-resolution experiments |
| [device-observations.md](device-observations.md) | Chip revisions, flash, PSRAM and factory firmware observed on the boards |
| [repository-verification.md](repository-verification.md) | State of the checkout during the study |

Kept as a historical record: statements about "the current code" describe the
tree as it was on 2026-09-27. Device MAC addresses and local paths have been
replaced with placeholders. The raw build and configure logs are kept outside
the repository.
