# Configuration ownership and portability

Reviewed 2026-10-07 against `78f4b73a` (v0.99.96.1), after pulling `origin/main`.
The audit below describes that baseline; source line references refer to the
reviewed revision. Implementation now adds [saved configurations](../configurations/README.md),
moves XIAO MQTT/I2C/display/input choices into overridable board defaults,
separates optional Bluetooth SDK policy into `presets/`, and extracts command
metadata into `System_CommandRegistry.h`. The larger architecture below remains
a proposal; it has not been introduced wholesale. Build verification does not
establish hardware qualification.

## Required behavior

Every board remains general-purpose. A user can select any feature for which
the assembled hardware, implemented backend, and available resources satisfy
the requirements. Named deployments are convenient presets and release
contracts; they do not define the board's possible uses.

An external display or sensor is an assembly choice. Absence of a built-in
peripheral is not sufficient reason to ban the feature. Supporting features
individually also does not guarantee enough resources to run all of them at
once.

## Classification

Three ownership categories are useful:

- **Hardware fact:** target, wiring, attached devices, flash/PSRAM geometry,
  power rails, and bus ownership. Board and assembly descriptions own these.
- **Constraint:** a feature or backend dependency, resource limit, peripheral
  conflict, or persisted release/layout requirement. Validation owns these.
- **User choice:** enabled applications, fitted optional peripherals, selected
  providers, operational defaults, and resource tradeoffs. Presets and explicit
  user overrides own these.

Keep a separate support status: implemented, qualified on hardware, or not yet
implemented/qualified. A missing backend is not a permanent silicon limit. A
disabled SDK stack describes the selected build, not the board's capabilities.

The table groups related rules instead of creating a second catalog of every
pin and macro. Links identify the actual current definitions.

| Rule family | Classification | Ownership / action |
| --- | --- | --- |
| Target, Arduino variant, flash size, PSRAM wiring/mode | Hardware facts | Keep board-specific. Validate target/board agreement; see [root build](../CMakeLists.txt#L156) and [QT Py board data](../boards/qtpy_esp32.defaults#L11). |
| GPIOs, I2C buses, rails, ADC divider, fuel gauge, UART pins | Hardware facts | Keep authoritative declarations in board/assembly data. [Board blocks](../components/hardwareone/System_BuildConfig.h#L984) and [EYE wiring](../components/hardwareone/System_Board_P4X_EYE.h#L11) already contain these. |
| XIAO base versus Sense expansion | Hardware/assembly choice | Resolve the attachment before deriving camera, PDM, and SD defaults. See [Sense selector and defaults](../components/hardwareone/System_BuildConfig.h#L205). |
| Network, web, and I2C feature levels; `CUSTOM_ENABLE_*` | User choices / bundle expansion | Treat levels as preset shorthand. Their meanings are in the [I2C](../components/hardwareone/System_BuildConfig.h#L514), [network](../components/hardwareone/System_BuildConfig.h#L670), and [web](../components/hardwareone/System_BuildConfig.h#L734) tables. |
| MQTT, HTTPS, automation, maps, games, G2/R1, test pages, inference choices | User choices | Enable independently of board identity, then validate actual prerequisites and budgets. [Application switches](../components/hardwareone/System_BuildConfig.h#L241). |
| XIAO forcing MQTT/I2C/display/input off | User/resource policy enforced as board identity | Remove the veto only as part of an explicit preset/override migration; [header](../components/hardwareone/System_BuildConfig.h#L454), [CMake copy](../components/hardwareone/CMakeLists.txt#L200). |
| P4 board profile enabling peripherals and disabling applications | Mixed hardware identity and user choices | Separate `HW_BOARD_P4X_EYE` from the preset. Preserve current selections as a selectable configuration; [profile](../boards/p4x_eye.features.h#L14). |
| QT Py SDK Bluetooth disabled | Resource choice | `CONFIG_BT_ENABLED=n` saves memory; it is not evidence that the chip lacks BLE. [Board policy](../boards/qtpy_esp32.defaults#L30). |
| App Bluetooth requires SDK Bluedroid; G2 requires BLE; R1 currently uses G2 stack | Implementation constraints | Keep dependencies. Distinguish an explicit contradictory request from an inherited preset default; [SDK gate](../components/hardwareone/System_BuildConfig.h#L820), [G2/R1 gates](../components/hardwareone/System_BuildConfig.h#L901). |
| HTTP needs the selected network path; pages/HTTPS need HTTP; MQTT needs WiFi; bond needs ESP-NOW | Implementation constraints | Centralize resolution. Current behavior mostly silently removes dependents; [HTTP](../components/hardwareone/System_BuildConfig.h#L691), [cross-feature rules](../components/hardwareone/System_BuildConfig.h#L826). |
| Display/input selection and optional encoder buttons | User/assembly choices plus backend constraints | Retain backend requirements, including I2C for Seesaw/ANO and the explicit EYE profile for its display/buttons. [Rules](../components/hardwareone/System_BuildConfig.h#L601). |
| I2C sensor startup queue | Derived infrastructure/resource rule | Enable only for consumers needing it. A fuel-gauge-only configuration should not acquire the optional sensor task; [rule](../components/hardwareone/System_BuildConfig.h#L657). |
| Camera/PDM availability, G2 audio source | Assembly facts plus provider choices | Separate having a source from choosing to compile/use it. G2 supplies audio without onboard PDM; [microphone composition](../components/hardwareone/System_BuildConfig.h#L284). |
| SD versus SDMMC, optional card mount | Assembly/provider choice plus constraints | Preserve internal filesystem independence and card/radio slot ownership; [selection](../components/hardwareone/System_BuildConfig.h#L1337), [slot wiring](../components/hardwareone/System_Board_P4X_EYE.h#L45). |
| Battery and NeoPixel automatic defaults | Hardware-derived user defaults plus validation | Keep opt-out. Fuel gauge needs I2C; incompatible battery backends and invalid pixel pins remain errors. [Battery](../components/hardwareone/System_BuildConfig.h#L1351), [NeoPixel](../components/hardwareone/System_BuildConfig.h#L1405). |
| Camera SCCB reservation, UART/console collision | Shared-resource constraints | Preserve across presets; [SCCB](../components/hardwareone/System_BuildConfig.h#L1453), [UART guard](../components/hardwareone/System_UartLink.cpp#L402). |
| Second I2C bus enable default and OLED bus preference | Hardware-derived user defaults | Bus pins/rail are facts; automatically enabling that bus or placing the display there is policy. [Rules](../components/hardwareone/System_BuildConfig.h#L1467). |
| ESP-SR, local STT, STT language model | User/provider choices plus backend/model constraints | Keep model/runtime and microphone prerequisites; inspect actual backend status rather than assuming every target implements local inference. [Selection](../components/hardwareone/System_BuildConfig.h#L340), [language-model dependency](../components/hardwareone/System_BuildConfig.h#L1567). |
| Dictation surface requirement | Current application composition | Today it requires audio and OLED/G2/web-sensors. This is not a speech engine hardware restriction; [rule](../components/hardwareone/System_BuildConfig.h#L1530). |
| QuickNote, hold action, note duration | User choices plus current composition/provider limits | New in this snapshot: QuickNote requires dictation, G2, and BLE; defaults on when those exist. Hold action is a preference; duration default varies with local/Pi transcription. [Rules](../components/hardwareone/System_BuildConfig.h#L1533). |
| LLM feature, onboard/CM5 providers, context length | User choices plus provider/resource constraints | Require a provider; CM5 requires an enabled UART link. Model/context fit is a runtime resource check. [Consistency rules](../components/hardwareone/System_BuildConfig.h#L1581), [PSRAM budget](../components/hardwareone/System_LLM_Model.cpp#L441). |
| Capture paths and icon-pack location | Operational/storage choices | Keep shared names; these are not board capabilities. [Paths](../components/hardwareone/System_BuildConfig.h#L280). |
| Partition layout, OTA identity, release size margin | Compatibility/security constraints plus budget policy | Physical capacity and a stricter release margin are distinct. Preserve existing stored-data boundaries; [layout selection](../CMakeLists.txt#L389), [example contract](../deployments/handheld/boards/p4x_eye/contract.conf#L4). |
| `feathers3_fe` and revision/security settings | Provisioning policy plus real hardware prerequisites | Same Feather hardware, separate explicit security/layout contract. Keep its isolated selection and provisioning guards; [profile](../boards/feathers3_fe.defaults#L3). |
| CMake source gates, compiler flags, profile precedence | Configuration mechanism | These should consume resolved decisions, not establish independent application policy. [Component parsing](../components/hardwareone/CMakeLists.txt#L35), [root parsing](../CMakeLists.txt#L389). |

## Confirmed portability problems

### 1. XIAO application policy overrides the user's selection

The header loads board and deployment overrides at lines 447–451, then forces
MQTT, I2C, display, and input off at lines 460–468. Its Sense and base wiring
still declare I2C pins 5/6. Component CMake repeats the veto based on
`HW_BOARD=xiao_s3`. Moving only one copy would leave inconsistent builds.

This restriction is justified in comments by the carrier's intended use and
flash headroom. Those are preset/budget decisions. Allowing a request is not
proof that a particular attached device works or that a large combination fits.

### 2. SDK settings can make a resource preference compulsory

QT Py's board defaults disable Bluetooth. When `HW_BOARD` is explicitly set in
the environment, as `build_board.sh` does, [root CMake](../CMakeLists.txt#L651)
reconciles saved sdkconfig against every key in the selected board file. A
manual change to that key is therefore not a reliable opt-in. App configuration
then silently turns BLE, G2, and R1 off when Bluedroid is absent.

Separate hardware/security invariants from feature SDK choices. An explicit
BLE request must either select its SDK dependencies or explain why it cannot
be built. Resource failure should name the exhausted budget.

Current XIAO defaults already [enable Bluetooth](../boards/xiao_s3.defaults#L46).
Older comments claiming they disable it remain in the root build and pocket
assistant configuration. They are not evidence of current behavior.

### 3. P4 identity and a large application preset share one file

`boards/p4x_eye.features.h` combines board identity with local display/input,
speech, QuickNote, networking, and applications disabled for the qualified
image. A later deployment can override those choices, unlike the XIAO veto.

Move identity/wiring to board data; preserve the current application as a
preset. P4's currently absent UART pin assignment still needs an assembly
definition before CM5 support can be claimed. Its SDMMC-off default records
pending card qualification, not missing SDMMC silicon.

### 4. Attachment defaults are resolved in the wrong order

Camera and PDM defaults use `XIAO_ESP32S3_SENSE_ENABLED` before override headers
load. A later override selecting the base XIAO leaves those defaults enabled
unless it also explicitly disables both features. The base board branch then
has no microphone pins. In addition, [component CMake](../components/hardwareone/CMakeLists.txt#L738)
and [root CMake](../CMakeLists.txt#L1221) define Sense selection for the whole S3
target family. Most non-XIAO branches ignore it, but it is the wrong ownership.

Resolve board and attachments first, requested features second, and derived
requirements last. Keep existing assembly selections explicit during migration.

### 5. Feature resolution is repeated and prevents profile reuse

The root build reads speech/model-location flags to choose partitions. Component
CMake reads literal header text to choose sources and repeats XIAO, BLE, G2,
display, and HTTP rules. The preprocessor resolves the actual application flags.
The CMake HTTP rule is deliberately coarser than the C++ custom-network rule;
extra guarded-empty sources are not by themselves a functional defect.

Release profiles duplicate complete literal selections to remain independent of
mutable defaults; see the [handheld profile explanation](../deployments/handheld/boards/p4x_eye/features.h#L10).
Partial literal overrides work, but the text parser prevents composing these
profiles through includes or expressions. Coverage scripts also
[edit and restore shared headers](../tools/build_coverage.sh#L49).
Use isolated build inputs and one authoritative rule set. Source selection may
remain conservative, but it must never omit an enabled implementation.

IDF resolves component requirements before all generated SDK symbols exist.
Any replacement must account for that phase ordering; simply preprocessing a
header that depends on a not-yet-generated `sdkconfig.h` is insufficient.

### 6. Some advertised options are incomplete backends

Preserve these distinctions in the configuration report:

- Local STT has a P4 implementation; other targets explicitly report unavailable
  in [System_STTLocal.cpp](../components/hardwareone/System_STTLocal.cpp#L6).
- The native DVP backend uses [XIAO wiring](../components/hardwareone/HAL_Camera_DVP.cpp#L175).
  Enabling its flag on another board does not supply a camera pin map.
- PDM audio requires declared [clock/data pins](../components/hardwareone/HAL_Audio.cpp#L22).
  The EYE CSI and SDMMC backends require their specific board profile.
- Generic ST7789 has [example pins](../components/hardwareone/HAL_Display.h#L99);
  ILI9341 is a [placeholder](../components/hardwareone/HAL_Display.h#L131).
- The header says onboard LLM requires S3+PSRAM and claims CMake excludes other
  targets, but the [actual CMake gate](../components/hardwareone/CMakeLists.txt#L633)
  checks only the feature flag. Other targets remain unverified by this audit.
- P4 hosted radio has operation-level limits, including no supported BLE TX-power
  setter in the pinned API; see [HAL_Bluetooth](../components/hardwareone/HAL_Bluetooth.cpp#L62).

These need explicit status or backend work, rather than blanket removal of guards.

### 7. QuickNote exposes a feature coupling and implicit preset growth

The new QuickNote compile gate requires dictation, G2, and BLE. The service
already accepts [web/local display callers](../components/hardwareone/System_QuickNote.cpp#L465)
and [PDM audio selection](../components/hardwareone/System_QuickNote.cpp#L736), but
also consults G2 native-voice state. Its current gate bundles the service,
storage, CLI, and lens page. Treat that as implementation coupling; removing
the G2 prerequisite would require a deliberate service/interface separation.

QuickNote uses the shared dictation provider, so it is not inherently P4-only.
[Provider readiness](../components/hardwareone/System_Dictation.cpp#L1040) still
depends on an available microphone and local backend or authenticated host.
The 60/30-second defaults are preferences; the remote recorder's
[30-second limit](../components/hardwareone/System_QuickNote.cpp#L124) is a backend
constraint. Disabling QuickNote must preserve unrelated hold-gesture handling.

Deployment feature headers did not change in the pulled update, but QuickNote
can enter their effective configurations through the new shared default or the
P4 overlay. Baselines must enumerate compiler-resolved features, including new
derived flags, rather than assuming a literal release profile fully pins them.

## Verification matrix without fixed board roles

These are proposed reusable test configurations, not new supported products or
claims that their firmware has already been built. Apply each workload to every
board/assembly with the required implementation, splitting workloads when a
measured resource budget requires it. A skipped combination must name the
missing backend, wiring, qualification, or budget evidence.

Board axis: `qtpy_esp32`, `feather_esp32_v2`, `feathers3`, `xiao_s3`, `p4x_eye`.
Add base/Sense and original FeatherS3/Series[D] assembly distinctions where they
change wiring. Keep `feathers3_fe` as a separate security/layout verification
variant. Generic/unknown fallback pins need an explicit assembly before they
can count as a hardware-qualified board.

| Reusable workload | What it proves / prerequisites |
| --- | --- |
| Minimal core | Every board compiles without optional apps, display, or radio application features; SDK-level size reduction is checked separately. |
| IP services and automation | WiFi backend, HTTP, MQTT, automation; exercise native and hosted radio. Include a smaller TLS-enabled variant if necessary. |
| External I2C and local UI | Declared usable bus/pins, selected sensor drivers, SSD1306 and Seesaw/ANO. Include XIAO after policy repair; EYE needs a supported external-bus assembly before this row applies. |
| BLE peripherals and audio | Native/hosted BLE, G2, R1, G2 microphone, dictation and QuickNote on/off; include a minimal QT Py BLE build after SDK policy repair. Provider availability is also checked at runtime. |
| Board media | Native DVP versus EYE CSI, onboard PDM versus G2 audio, camera/bus coexistence, SPI SD versus EYE SDMMC. Card qualification remains explicit. |
| Portable applications/providers | Rotate maps, games, CM5-only LLM, onboard LLM, ESP-SR, local STT; require each backend's actual SDK/model/memory support. Include P4 automation/MQTT to demonstrate override freedom. |
| Feature removal and conflicts | HTTP off, BLE/G2 off, each provider off, display/input/storage independence; verify required stubs and rejection of incompatible explicit requests. |
| Release/storage variants | Existing deployment contracts, OTA pair compatibility, encrypted variant, unchanged partition geometry/identities, and release margins. Builds do not imply flashing or provisioning. |

Keep test cost bounded through four layers:

1. **Fast rule checks:** use the real preprocessor for both directions of each
   important choice and its prerequisites. Cover boundary and conflict cases;
   avoid the full Boolean Cartesian product. Include QuickNote dependency and
   opt-out checks in the current snapshot.
2. **Shared behavior tests:** run existing host suites once per change, with
   relevant mocked backend variants. They do not establish physical RAM or timing.
3. **Build coverage:** configure/link at least a baseline per board, plus enough
   reusable workloads to cover every selected source/backend and known important
   interaction. Compare requested/effective features, SDK settings, sources,
   linker memory use, and fixed partition limits. Expand changed dimensions;
   a rotating feature may not remain permanently untested on a capable board.
4. **Hardware checks:** exercise each changed backend on the relevant board:
   boot, peripherals, memory/stack headroom under load, shared buses, and radio
   coexistence. A host test or successful link is not a hardware pass.

Use [existing host suites](../components/hardwareone/test/host/README.md),
[optional-peripheral rule tests](../components/hardwareone/test/host/test_optional_peripheral_config.py),
[HTTP rule tests](../components/hardwareone/test/host/test_http_feature_gates.py),
[OTA contract tests](../tools/ota/tests/test_deployment_contract.py), and
[compiler-derived build manifests](../tools/gen_build_info.py#L11).
No new universal module framework is needed for this audit's recommendations.

## Configuration creator and build workflow

The configuration creator and this cleanup should share one set of feature
rules. Lean presets remain useful; the user can change their selections to fit
the device's intended use. There is no requirement to enable every feature on
every board or to make every possible combination fit.

Existing pieces found in this checkout:

| Piece | Current responsibility | Integration role |
| --- | --- | --- |
| [build_board.sh](../tools/build_board.sh) | Builds one board in its own directory, using the shared editable feature defaults plus the board profile. | Reuse board/SDK selection and build checks; add isolation by saved configuration as well as board. |
| [build_deployment.sh](../tools/build_deployment.sh) | Builds a named deployment, enforces release limits, and assembles factory or signed OTA artifacts. | Keep it responsible for established release/layout contracts. A feature selection alone should not invent a new release identity. |
| [HW1Deployment.cmake](../cmake/HW1Deployment.cmake) and [deployment_contract.py](../tools/ota/deployment_contract.py) | Consume the same contract naming a feature header, optional SDK fragment, partitions, and release limits. | Reuse the existing representation and loaders where applicable. Separate ordinary configuration selection from release packaging. |
| [gen_build_info.py](../tools/gen_build_info.py) | Reports compiler-resolved features and build information. | Extend reporting to connect the saved choices to the produced firmware and measured capacity. |
| [feature_size_sweep.sh](../tools/feature_size_sweep.sh) and [map_feature_cost.py](../tools/map_feature_cost.py) | Measure feature-removal deltas and attribute linked size. | Supply advisory cost information. Final linked size remains authoritative; overlapping dependencies make estimates non-additive. |

Repository and tracked-history searches did not identify a separate unfinished
feature-selection/configuration creator. The user's earlier tool still needs
to be identified before choosing its UI or replacing it. The tools above build
or inspect existing selections; they do not provide that missing authoring step.

The intended end-to-end workflow is:

1. Select a board/assembly and start from a preset or existing saved configuration.
2. Choose features and providers. Explain required dependencies and incompatible
   choices using the same rules used by the build.
3. Save a named configuration containing the explicit choices and the preset
   revision or resolved selection. Newly added default-on features must not
   silently change a previously saved configuration.
4. Resolve that input into build-local feature and SDK inputs, preserving the
   selected storage/security contract. Keep generated files out of shared
   editable headers. Each configuration/board pair gets its own SDK/build state.
5. Compile through the existing build pipeline. Report actual application size,
   partition headroom, static memory use, and any mismatch with the saved choices.
   Runtime heap/stack headroom still requires device measurements.
6. Run configuration checks against those same saved inputs and generated
   outputs. Tests should consume the configurations the user actually builds.

The creator should be a thin client of shared configuration rules. Avoid adding
another independent dependency table in a script or UI. A practical first
integration should load, customize, save, and rebuild one existing preset;
prove two configurations of the same board remain isolated; and preserve the
old build commands while they migrate to the shared path. A failed fit should
report the exceeded budget and let the user revise their choices.

## Implementation order and completion criteria

1. **Expose requested versus effective configuration.** Extend existing build
   reporting with origins and reasons for changes. Capture SDK/feature/source
   and layout baselines. Synthetic host configurations and actual device SDK
   results must be reported separately.
2. **Separate board facts from application presets.** Keep existing behavior
   explicitly selectable. Introduce isolated feature overrides usable with any
   board without requiring a new OTA/deployment identity. Move XIAO exclusions
   and QT Py's memory-saving BLE choice into presets; split P4 identity from
   application selection. Correct attachment-resolution order.
3. **Consolidate resolution.** Keep existing `ENABLE_*` names as the application
   interface while eliminating duplicated rule implementations. Preserve early
   IDF requirement discovery. A user-requested unsupported combination gets a
   specific error/status; an automatic default can resolve off with a reason.
4. **Automate the matrix incrementally.** Reuse the existing suites and isolated
   board build directories; replace source-mutating coverage scripts. Add actual
   hardware qualification only for the combinations whose backends changed.

The first portability change is complete when XIAO can request an external I2C
configuration or MQTT without a board veto, the legacy configuration remains
reproducible, compiler/source/SDK decisions agree, affected firmware links within
its budget, and the relevant hardware path has been checked. This does not require
simultaneously implementing missing display, inference, or camera backends.

Preserve storage/security contracts throughout: changing feature choices must
not silently repartition deployed devices or select a provisioning mode.
Measure success by removed policy copies, fewer files needed to add a feature
or board, explained incompatibilities, and demonstrated coverage.

## Evidence collected

Source review covered the shared feature header, component/root CMake, board
and deployment profiles, relevant SDK defaults, backend guards, and existing
verification tools. Current checks and targeted host-preprocessor observations
are recorded below. They are not full ESP-IDF builds.

| Probe | Observed result |
| --- | --- |
| XIAO: request MQTT=1, I2C level=4, display=1, input=1 through the deployment override hook | All four resolve to 0; declared I2C pins remain 5/6. |
| FeatherS3: same request | Resolves to 1, 4, 1, 1 respectively. |
| QT Py: request BLE/G2/R1, synthetic SDK omits Bluetooth/Bluedroid | All three resolve to 0. |
| QT Py: same request, synthetic SDK supplies Bluetooth/Bluedroid | All three resolve to 1; this proves header resolution only. |
| P4: board preset | MQTT/automation/maps resolve to 0. |
| P4: override those three requests to 1 | All three resolve to 1; EYE identity remains 1. |
| XIAO: override only Sense attachment to 0 | Base board name selected, but camera and PDM flags remain 1 and `MIC_CLK_PIN` is undefined. |
| P4: explicitly disable QuickNote | QuickNote resolves to 0 and the default hold action to 0. |
| P4: explicitly enable QuickNote while disabling G2 | Rejected with the specific QuickNote prerequisite error. |

The probes use `c++ -E -P`, the real `System_BuildConfig.h`, temporary override
headers via `HW1_DEPLOYMENT_CONFIG_HEADER`, the real P4 profile where applicable,
and minimal synthetic `sdkconfig.h` inputs. Unrelated LLM/CM5 requests were
disabled for the cross-board probes. Generated firmware SDK settings, binaries,
performance, and device behavior still require the proposed build/hardware checks.

Validation rerun against `78f4b73a`:

- Nine targeted preprocessor probes passed, including the expected rejection.
- `python3 -B components/hardwareone/test/host/test_http_feature_gates.py`:
  100 preprocessor profiles passed.
- `python3 -B components/hardwareone/test/host/test_optional_peripheral_config.py`:
  nine tests passed (some cover multiple configurations).
- No complete ESP-IDF builds, full host-suite run, device flashing, or physical
  qualification were performed for this documentation-only audit.
