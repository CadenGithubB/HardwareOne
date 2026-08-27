#ifndef G2_PAGE_USERS_H
#define G2_PAGE_USERS_H

// =============================================================================
// G2 glasses — "Users" App page (admin user manager)
// =============================================================================
// A tap-navigated user manager reached from the Config launcher (the row appears
// only for an admin pairer). Read-only display comes from /system/users/users.json;
// every mutation dispatches a real command through g2SubmitHijackCommand so the
// admin/rank gating in authorizeCommand + userMutationAllowed applies.
//
//   LIST     "<- Config" + "+ Add User" + one row per account ("alice (admin)")
//     DETAIL role + active/banned status + dynamically permitted actions
//       ROLE   "<- Back" + [X] guest/user/admin (+superadmin if pairer is super)
//       PASSWORD self change or reset-policy + admin reset, with confirmation
//       ACTIONS confirmed kick sessions / ban-unban / delete
//   ADD      keyboard: username -> role pick -> password -> confirm -> useradd
//
// Founder, higher-rank, and active-G2-owner protections are mirrored in the UI;
// the command layer remains the security backstop. Session/ban controls are
// shown only when ENABLE_HTTP_SERVER provides their real command handlers.

#include "System_BuildConfig.h"
#include <stddef.h>
#include <stdint.h>

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

// CLI direct-invocation stub required by the page registry's buildText slot.
void g2BuildUsersInfo(char* out, size_t cap);

// Render the user list. Called from the Config launcher tap dispatch.
void g2ShowUsersMenu();

// Tap dispatch from handleHijackMenuTap when gHijackPage == USERS.
void g2UsersHandleTap(uint32_t idx);

#else  // stubs when BLE / G2 disabled

inline void g2BuildUsersInfo(char* out, size_t cap) { if (out && cap > 0) out[0] = '\0'; }
inline void g2ShowUsersMenu() {}
inline void g2UsersHandleTap(uint32_t /*idx*/) {}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#endif  // G2_PAGE_USERS_H
