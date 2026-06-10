# IDF 6.0 + ESP32-P4 Roadmap

This folder collects the docs for the firmware's **forward path**: migrating off the
current **ESP-IDF 5.5.1** baseline (app v0.95.5) toward **ESP-IDF 6.0**, and the actual
goal behind that — adding **ESP32-P4X (silicon rev 3.x)** support.

## Read in this order

1. **[IDF6_MIGRATION_FEASIBILITY.md](IDF6_MIGRATION_FEASIBILITY.md)** — *Can we move 5.5.1 → 6.0?*
   A per-breaking-change exposure scan of this codebase. **Verdict: deferred** — blocked on
   arduino-esp32 **4.0** (the IDF-6.0 line) reaching *stable* with `esp-sr`. Our own code is
   nearly clean; the one real item is mbedTLS-4.0 → libsodium. Includes the P4 dependency chain.
2. **[ESP32_P4_PORT_ASSESSMENT.md](ESP32_P4_PORT_ASSESSMENT.md)** — *Should/how do we add ESP32-P4X?*
   **Verdict: deferred** — the real blocker is **ESP-NOW-over-esp-hosted** (the P4's C6 radio
   exposes zero ESP-NOW RPCs), which is independent of the IDF/Arduino versions.
3. **[I2C_MASTER_MIGRATION_PLAN.md](I2C_MASTER_MIGRATION_PLAN.md)** — ✅ **done (v0.95.5)**. The
   completed 5.3.1 → 5.5.1 upgrade that moved Arduino's `Wire` onto the `i2c_master` driver.
   Kept here as the **template** for how the 6.0 migration will run (bump IDF + Arduino, fix
   breakages, HW-revalidate both boards).

## One-line status (2026-06)

| Goal | Gate | Status |
|---|---|---|
| IDF 5.5.1 + `i2c_master` | — | ✅ shipped (v0.95.5) |
| IDF 6.0 | arduino-esp32 4.0 stable + esp-sr | ⏳ alpha only (`4.0.0-alpha1`, IDF 6.0.1) |
| ESP32-P4X (rev 3.x) | ESP-NOW over esp-hosted | 🔴 no RPCs upstream; needs host-side proxy |

The chain is **sequential** — P4X needs IDF 6.0 needs Arduino 4.0 — but the *hardest* link
is the **last** one (ESP-NOW), which no upstream IDF/Arduino release fixes.
