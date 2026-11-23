#pragma once
// ESP-NOW revoked list (SEC-6, 1.0 design section 5.1 B): the devices this
// board refuses until a super admin runs espnowunrevoke. The rules and the
// stored form are in System_ESPNow_RevokedCore.h; this module owns the table
// (PSRAM), its lock and /system/espnow/revoked.json.
//
// The list is loaded before ESP-NOW receive starts (initEspNow). A file that
// is there but cannot be read locks the list (fail closed): no new identity
// is stored and pairing is refused until a super admin acts
// (espnowunrevoke --all moves the file aside).
//
// Every query is cheap (a scan of at most 32 entries under a spinlock) and
// safe from any task, the RX path on espnow_task included.
#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include <stddef.h>
#include <stdint.h>

#include "System_ESPNow_RevokedCore.h"

// Reads revoked.json once per boot (later calls do nothing). cmd_exec / init.
void espnowRevokedLoad();

// True while revoked.json is there but could not be read (fail closed).
bool espnowRevokedLocked();

// A listed MAC (any key), a listed key (any MAC), or either (pub may be null).
bool espnowRevokedMac(const uint8_t mac[6]);
bool espnowRevokedKey(const uint8_t pub[32]);
bool espnowRevokedMacOrKey(const uint8_t mac[6], const uint8_t pub[32]);

// A copy of the entry that lists this MAC or key, for messages. False when none.
bool espnowRevokedLookup(const uint8_t mac[6], const uint8_t pub[32], espnowrevoke::Entry& out);

enum class RevokedChange : uint8_t { Added, Updated, Full, Removed, NotFound, SaveFailed, Locked };

// Adds (or refreshes) a revoke and writes revoked.json before returning. The
// table is changed only when the file was written. pub may be null (no key
// known for the MAC). cmd_exec.
RevokedChange espnowRevokedAdd(const uint8_t mac[6], const uint8_t pub[32], uint8_t meshSlot,
                               const char* name);

// Lifts the revoke `text` names (name, MAC or keyId prefix); *removed gets a
// copy of it. Writes the file first. cmd_exec.
RevokedChange espnowRevokedRemove(const char* text, espnowrevoke::Entry* removed);

// Lifts every revoke. A list locked because nothing could be read at
// start-up (no filesystem, no memory) is read again first; a file that was
// read and is unreadable is moved aside (its revokes are lifted with it).
// *count gets how many were listed. cmd_exec.
RevokedChange espnowRevokedClear(uint8_t* count, char* movedTo, size_t movedToLen);

// A copy of the whole table.
void espnowRevokedSnapshot(espnowrevoke::Table& out);

// The local date of a revoke time, or "an unknown date" while the clock was not set.
void espnowRevokedFormatDate(uint64_t epochMs, char* out, size_t outLen);

// ---- Implemented in System_ESPNow.cpp (they read the device registry) ----

// What a verified KEY_EX_HELLO / KEY_EX_REPLY may do with the key it presents
// (espnowrevoke::admit, rule A): the registry, the stored identity, the
// revoked list and the MAC's first-key window. espnow_task.
espnowrevoke::Admit espnowKeyExAdmit(const uint8_t mac[6], const uint8_t pub[32]);

// The first key of a registry entry was stored: its first-key window closes.
void espnowFirstKeyBound(const uint8_t mac[6]);

// A key exchange met a revoked key under `mac`. A registry entry that
// espnowpairsecure made for it and that is still waiting for its first key
// is rolled back (on cmd_exec), with a line naming the revoke. espnow_task.
void espnowPairRollbackRevoked(const uint8_t mac[6]);

#endif  // ENABLE_ESPNOW
