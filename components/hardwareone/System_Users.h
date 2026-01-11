/**
 * System_Users.h - User database and management
 * 
 * User CRUD operations, pending users, roles.
 * This header provides a cleaner naming convention for user management
 * functions that are implemented in System_User.cpp.
 */

#ifndef SYSTEM_USERS_H
#define SYSTEM_USERS_H

// Include the main header which has all definitions
#include "System_User.h"

// All user management functions are defined in System_User.h:
// - isAdminUser(), getUserIdByUsername(), getUserRole()
// - USERS_JSON_FILE
// - usernameExistsInUsersJson(), loadUsersFromFile()
// - resolvePendingUserCreationTimes(), writeBootAnchor(), cleanupOldBootAnchors()
// - loadAndIncrementBootSeq()
// - cmd_user_* commands, cmd_pending_list()

#endif // SYSTEM_USERS_H
