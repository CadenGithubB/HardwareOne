#pragma once
#include <stdint.h>

// ============================================================================
// System_BootState — small persistent boot state in NVS
// ============================================================================
// The boot counter must (a) survive a full power loss and (b) advance on every
// single boot. Storing it in users.json meant the entire auth database was
// rewritten every boot just to bump one integer — one power cut in that window
// destroyed every login. NVS lives in its own flash partition with built-in
// wear leveling and atomic per-key writes, so the churn is physically isolated
// from users.json and settings.json: a bad write here can never touch either.
// See project_secret_loss_diagnosis_and_hardening.

// Initialize NVS. Idempotent and safe to call more than once (and before or
// after the WiFi stack does its own nvs_flash_init). Each accessor below calls
// this first, so an explicit call in setup() is optional insurance, not a
// prerequisite.
void bootStateInit();

// Read the persisted boot counter, increment it, persist, and return the new
// value. The first call on a fresh device returns 1.
uint32_t bootStateIncrementBootCount();

// Read-only accessor — no increment, no write. Returns 0 if never set.
uint32_t bootStateGetBootCount();

// Reset the persisted boot counter to 0. Diagnostics only.
void bootStateResetBootCount();

// Account-id high-water mark: the highest account id this device has ever
// issued. It lives here, outside users.json, so neither a factory reset (which
// deletes users.json) nor a restored older roster lets System_User hand an id
// out again while data keyed by that id may still exist. System_User owns it
// and serializes its updates under the filesystem lock.
//
// Reads the mark into out (0 when never set). False when NVS is unavailable or
// the read fails; out is then left unchanged.
bool bootStateGetAccountIdHighWater(uint32_t& out);

// Raises the mark to id when it is lower. True only when the stored mark is at
// least id afterwards (the write and the commit are checked).
bool bootStateRaiseAccountIdHighWater(uint32_t id);
