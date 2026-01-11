/**
 * System_Auth.h - Authentication and session management
 * 
 * Transport-agnostic authentication, login/logout, password handling.
 * This header provides a cleaner naming convention for auth-related types
 * and functions that are implemented in System_User.cpp.
 */

#ifndef SYSTEM_AUTH_H
#define SYSTEM_AUTH_H

// Include the main header which has all definitions
#include "System_User.h"

// All auth types and functions are defined in System_User.h:
// - CommandSource enum
// - AuthContext struct
// - tgRequireAuth(), tgRequireAdmin()
// - loginTransport(), logoutTransport()
// - isTransportAuthenticated(), getTransportUser(), isTransportAdmin()
// - hashUserPassword(), verifyUserPassword(), isValidUser()
// - cmd_login(), cmd_logout(), cmd_session_list(), cmd_session_revoke()

#endif // SYSTEM_AUTH_H
