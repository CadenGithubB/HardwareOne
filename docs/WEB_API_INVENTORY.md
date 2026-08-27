# Web Interface API Inventory

> Generated reference for the HardwareOne firmware HTTP web interface (`esp-http-server` / httpd).
> Every registered route is enumerated below with its method, auth level, request parameters,
> response shape, purpose, and a maturity/observation note. Use the **Maturity** column and the
> [Improvement Candidates](#improvement-candidates) section to scope the modernization project.

## Overview

- **Total endpoints:** 112 across 16 functional groups
- **By kind:** api 78, page 25, asset 4, options 3, stream 2
- **By auth:** session 88, public 16, admin 6, special 2
- **By maturity:** adequate 58, mature 47, barebones 7

**Legend**

- **Kind** — `page` HTML UI · `api` JSON/data · `stream` MJPEG/SSE/chunked · `asset` icon/binary · `options` CORS preflight
- **Auth** — `public` no login · `session` logged-in user · `admin` admin user only · `special` recovery-mode / external-flow
- **Maturity** — `mature` solid · `adequate` works, minor gaps · `barebones` minimal/stale · `stale` outdated · `broken-or-unknown`

## Functional Groups

- [Authentication, Accounts & Admin](#group-auth) — 15 endpoints
- [System, Dashboard & Core](#group-system) — 13 endpoints
- [Settings](#group-settings) — 5 endpoints
- [File System](#group-files) — 8 endpoints
- [CLI & Logging](#group-cli) — 5 endpoints
- [Automations](#group-automations) — 3 endpoints
- [Sensors, Camera & Media](#group-sensors) — 11 endpoints
- [LLM (on-device language model)](#group-llm) — 11 endpoints
- [Maps & GPS](#group-maps) — 5 endpoints
- [ESP-NOW Mesh](#group-espnow) — 5 endpoints
- [Bond (paired-device control channel)](#group-bond) — 14 endpoints
- [Battery & Power](#group-battery) — 2 endpoints
- [Bluetooth, Speech & MQTT pages](#group-connectivity) — 4 endpoints
- [Games & DarkRoom](#group-games) — 2 endpoints
- [Edge Impulse / ML](#group-ml) — 2 endpoints
- [Backup, Restore & Recovery (Migration Tool)](#group-migration) — 7 endpoints
- [Cross-Cutting Observations](#cross-cutting-observations)
- [Improvement Candidates](#improvement-candidates)
- [Modernization Backlog](#modernization-backlog)

## Master Index

| # | Method | Path | Group | Kind | Auth | Maturity |
|---|--------|------|-------|------|------|----------|
| 1 | GET | [`/`](#get) | Authentication, Accounts & Admin | page | public | mature |
| 2 | GET | [`/login`](#get-login) | Authentication, Accounts & Admin | page | public | mature |
| 3 | POST | [`/login`](#post-login) | Authentication, Accounts & Admin | api | public | mature |
| 4 | GET | [`/login/setsession`](#get-login-setsession) | Authentication, Accounts & Admin | page | special | adequate |
| 5 | GET | [`/logout`](#get-logout) | Authentication, Accounts & Admin | page | public | mature |
| 6 | GET | [`/register`](#get-register) | Authentication, Accounts & Admin | page | public | mature |
| 7 | POST | [`/register/submit`](#post-register-submit) | Authentication, Accounts & Admin | api | public | adequate |
| 8 | GET | [`/account/password-change`](#get-account-password-change) | Authentication, Accounts & Admin | page | session | mature |
| 9 | POST | [`/account/password-change`](#post-account-password-change) | Authentication, Accounts & Admin | api | session | mature |
| 10 | GET | [`/api/sessions`](#get-api-sessions) | Authentication, Accounts & Admin | api | admin | mature |
| 11 | GET | [`/api/admin/sessions`](#get-api-admin-sessions) | Authentication, Accounts & Admin | api | admin | mature |
| 12 | GET | [`/api/admin/pending`](#get-api-admin-pending) | Authentication, Accounts & Admin | api | admin | adequate |
| 13 | POST | [`/api/admin/approve`](#post-api-admin-approve) | Authentication, Accounts & Admin | api | admin | adequate |
| 14 | POST | [`/api/admin/reject`](#post-api-admin-reject) | Authentication, Accounts & Admin | api | admin | adequate |
| 15 | GET | [`/api/notice`](#get-api-notice) | Authentication, Accounts & Admin | api | session | mature |
| 16 | GET | [`/dashboard`](#get-dashboard) | System, Dashboard & Core | page | session | adequate |
| 17 | GET | [`/api/ping`](#get-api-ping) | System, Dashboard & Core | api | public | mature |
| 18 | GET | [`/api/system`](#get-api-system) | System, Dashboard & Core | api | session | adequate |
| 19 | GET | [`/api/buildconfig`](#get-api-buildconfig) | System, Dashboard & Core | api | session | mature |
| 20 | GET | [`/api/devices`](#get-api-devices) | System, Dashboard & Core | api | session | adequate |
| 21 | GET | [`/api/events`](#get-api-events) | System, Dashboard & Core | stream | session | adequate |
| 22 | GET | [`/api/output`](#get-api-output) | System, Dashboard & Core | api | session | **REMOVED 2026-07-19** |
| 23 | POST | [`/api/output/temp`](#post-api-output-temp) | System, Dashboard & Core | api | session | **REMOVED 2026-07-19** |
| 24 | GET | [`/api/icon`](#get-api-icon) | System, Dashboard & Core | asset | public | adequate |
| 25 | GET | [`/icons/test`](#get-icons-test) | System, Dashboard & Core | page | session | barebones |
| 26 | GET | [`/favicon.ico`](#get-faviconico) | System, Dashboard & Core | asset | public | mature |
| 27 | GET | [`/apple-touch-icon.png`](#get-apple-touch-iconpng) | System, Dashboard & Core | asset | public | mature |
| 28 | GET | [`/apple-touch-icon-precomposed.png`](#get-apple-touch-icon-precomposedpng) | System, Dashboard & Core | asset | public | mature |
| 29 | GET | [`/settings`](#get-settings) | Settings | page | session | adequate |
| 30 | GET | [`/api/settings`](#get-api-settings) | Settings | api | session | adequate |
| 31 | GET | [`/api/settings/schema`](#get-api-settings-schema) | Settings | api | session | adequate |
| 32 | GET | [`/api/user/settings`](#get-api-user-settings) | Settings | api | session | adequate |
| 33 | POST | [`/api/user/settings`](#post-api-user-settings) | Settings | api | session | adequate |
| 34 | GET | [`/files`](#get-files) | File System | page | session | mature |
| 35 | GET | [`/api/files/list`](#get-api-files-list) | File System | api | session | mature |
| 36 | GET | [`/api/files/stats`](#get-api-files-stats) | File System | api | session | mature |
| 37 | POST | [`/api/files/create`](#post-api-files-create) | File System | api | session | adequate |
| 38 | GET | [`/api/files/view`](#get-api-files-view) | File System | api | session | mature |
| 39 | GET | [`/api/files/read`](#get-api-files-read) | File System | api | session | mature |
| 40 | POST | [`/api/files/write`](#post-api-files-write) | File System | api | session | mature |
| 41 | POST | [`/api/files/upload`](#post-api-files-upload) | File System | api | session | mature |
| 42 | GET | [`/cli`](#get-cli) | CLI & Logging | page | session | mature |
| 43 | POST | [`/api/cli`](#post-api-cli) | CLI & Logging | api | session | mature |
| 44 | POST | [`/api/cli/batch`](#post-api-cli-batch) | CLI & Logging | api | session | mature |
| 45 | GET | [`/api/cli/logs`](#get-api-cli-logs) | CLI & Logging | api | session | adequate |
| 46 | GET | [`/logging`](#get-logging) | CLI & Logging | page | session | mature |
| 47 | GET | [`/automations`](#get-automations) | Automations | page | session | adequate |
| 48 | GET | [`/api/automations`](#get-api-automations) | Automations | api | session | adequate |
| 49 | GET | [`/api/automations/export`](#get-api-automations-export) | Automations | api | session | adequate |
| 50 | GET | [`/sensors`](#get-sensors) | Sensors, Camera & Media | page | session | adequate |
| 51 | GET | [`/api/sensors`](#get-api-sensors) | Sensors, Camera & Media | api | session | adequate |
| 52 | GET | [`/api/sensors/status`](#get-api-sensors-status) | Sensors, Camera & Media | api | session | mature |
| 53 | GET | [`/api/sensors/remote`](#get-api-sensors-remote) | Sensors, Camera & Media | api | session | adequate |
| 54 | GET | [`/api/sensors/camera/status`](#get-api-sensors-camera-status) | Sensors, Camera & Media | api | session | barebones |
| 55 | GET | [`/api/sensors/camera/frame`](#get-api-sensors-camera-frame) | Sensors, Camera & Media | api | session | adequate |
| 56 | GET | [`/api/sensors/camera/stream`](#get-api-sensors-camera-stream) | Sensors, Camera & Media | stream | session | adequate |
| 57 | GET | [`/api/recordings`](#get-api-recordings) | Sensors, Camera & Media | api | session | adequate |
| 58 | GET | [`/api/recordings/file`](#get-api-recordings-file) | Sensors, Camera & Media | api | session | adequate |
| 59 | GET | [`/api/videos`](#get-api-videos) | Sensors, Camera & Media | api | session | adequate |
| 60 | GET | [`/api/videos/file`](#get-api-videos-file) | Sensors, Camera & Media | api | session | adequate |
| 61 | GET | [`/llm`](#get-llm) | LLM (on-device language model) | page | session | adequate |
| 62 | GET | [`/api/llm/status`](#get-api-llm-status) | LLM (on-device language model) | api | session | mature |
| 63 | GET | [`/api/llm/models`](#get-api-llm-models) | LLM (on-device language model) | api | session | adequate |
| 64 | POST | [`/api/llm/load`](#post-api-llm-load) | LLM (on-device language model) | api | session | adequate |
| 65 | POST | [`/api/llm/unload`](#post-api-llm-unload) | LLM (on-device language model) | api | session | mature |
| 66 | POST | [`/api/llm/generate`](#post-api-llm-generate) | LLM (on-device language model) | api | session | adequate |
| 67 | POST | [`/api/llm/stop`](#post-api-llm-stop) | LLM (on-device language model) | api | session | mature |
| 68 | GET | [`/api/llm/result`](#get-api-llm-result) | LLM (on-device language model) | api | session | mature |
| 69 | GET | [`/api/llm/chat/turns`](#get-api-llm-chat-turns) | LLM (on-device language model) | api | session | adequate |
| 70 | POST | [`/api/llm/chat/retry`](#post-api-llm-chat-retry) | LLM (on-device language model) | api | session | adequate |
| 71 | POST | [`/api/llm/chat/clear`](#post-api-llm-chat-clear) | LLM (on-device language model) | api | session | adequate |
| 72 | GET | [`/maps`](#get-maps) | Maps & GPS | page | session | adequate |
| 73 | GET | [`/api/maps/features`](#get-api-maps-features) | Maps & GPS | api | session | barebones |
| 74 | GET | [`/api/waypoints`](#get-api-waypoints) | Maps & GPS | api | session | adequate |
| 75 | POST | [`/api/waypoints`](#post-api-waypoints) | Maps & GPS | api | session | barebones |
| 76 | GET | [`/api/gps/tracks`](#get-api-gps-tracks) | Maps & GPS | api | session | adequate |
| 77 | GET | [`/espnow`](#get-espnow) | ESP-NOW Mesh | page | session | adequate |
| 78 | GET | [`/api/espnow/messages`](#get-api-espnow-messages) | ESP-NOW Mesh | api | session | mature |
| 79 | GET | [`/api/espnow/remotecap`](#get-api-espnow-remotecap) | ESP-NOW Mesh | api | session | adequate |
| 80 | GET | [`/api/espnow/remotemanifest`](#get-api-espnow-remotemanifest) | ESP-NOW Mesh | api | session | barebones |
| 81 | GET | [`/api/espnow/metadata`](#get-api-espnow-metadata) | ESP-NOW Mesh | api | session | adequate |
| 82 | GET | [`/bond`](#get-bond) | Bond (paired-device control channel) | page | session | mature |
| 83 | GET | [`/api/bond/status`](#get-api-bond-status) | Bond (paired-device control channel) | api | session | mature |
| 84 | POST | [`/api/bond/stream`](#post-api-bond-stream) | Bond (paired-device control channel) | api | session | adequate |
| 85 | POST | [`/api/bond/exec`](#post-api-bond-exec) | Bond (paired-device control channel) | api | session | adequate |
| 86 | POST | [`/api/bond/role`](#post-api-bond-role) | Bond (paired-device control channel) | api | session | mature |
| 87 | POST | [`/api/bond/cli/batch`](#post-api-bond-cli-batch) | Bond (paired-device control channel) | api | session | mature |
| 88 | POST | [`/api/bond/settings/sync`](#post-api-bond-settings-sync) | Bond (paired-device control channel) | api | session | mature |
| 89 | GET | [`/api/bond/settings/schema`](#get-api-bond-settings-schema) | Bond (paired-device control channel) | api | session | mature |
| 90 | POST | [`/api/bond/settings/schema/sync`](#post-api-bond-settings-schema-sync) | Bond (paired-device control channel) | api | session | mature |
| 91 | GET | [`/api/bond/settings`](#get-api-bond-settings) | Bond (paired-device control channel) | api | session | mature |
| 92 | GET | [`/api/bond/paired-devices`](#get-api-bond-paired-devices) | Bond (paired-device control channel) | api | session | mature |
| 93 | GET | [`/api/bond/fs/list`](#get-api-bond-fs-list) | Bond (paired-device control channel) | api | session | adequate |
| 94 | GET | [`/api/bond/fs/stat`](#get-api-bond-fs-stat) | Bond (paired-device control channel) | api | session | adequate |
| 95 | GET | [`/api/bond/fs/get`](#get-api-bond-fs-get) | Bond (paired-device control channel) | api | session | adequate |
| 96 | GET | [`/battery`](#get-battery) | Battery & Power | page | session | adequate |
| 97 | GET | [`/api/battery/status`](#get-api-battery-status) | Battery & Power | api | session | adequate |
| 98 | GET | [`/bluetooth`](#get-bluetooth) | Bluetooth, Speech & MQTT pages | page | session | adequate |
| 99 | GET | [`/speech`](#get-speech) | Bluetooth, Speech & MQTT pages | page | session | adequate |
| 100 | GET | [`/mqtt`](#get-mqtt) | Bluetooth, Speech & MQTT pages | page | session | adequate |
| 101 | GET | [`/api/mqtt/status`](#get-api-mqtt-status) | Bluetooth, Speech & MQTT pages | api | session | adequate |
| 102 | GET | [`/games`](#get-games) | Games & DarkRoom | page | session | adequate |
| 103 | GET | [`/darkroom`](#get-darkroom) | Games & DarkRoom | page | session | mature |
| 104 | POST | [`/api/ei/organize`](#post-api-ei-organize) | Edge Impulse / ML | api | session | adequate |
| 105 | GET | [`/api/edgeimpulse/detect`](#get-api-edgeimpulse-detect) | Edge Impulse / ML | api | session | adequate |
| 106 | POST | [`/api/backup`](#post-api-backup) | Backup, Restore & Recovery (Migration Tool) | api | admin | mature |
| 107 | POST | [`/api/restore`](#post-api-restore) | Backup, Restore & Recovery (Migration Tool) | api | special | adequate |
| 108 | OPTIONS | [`/api/backup`](#options-api-backup) | Backup, Restore & Recovery (Migration Tool) | options | public | mature |
| 109 | OPTIONS | [`/api/restore`](#options-api-restore) | Backup, Restore & Recovery (Migration Tool) | options | public | mature |
| 110 | OPTIONS | [`/api/ping`](#options-api-ping) | Backup, Restore & Recovery (Migration Tool) | options | public | mature |
| 111 | GET | [`/`](#get) | Backup, Restore & Recovery (Migration Tool) | page | public | barebones |
| 112 | GET | [`/api/ping`](#get-api-ping) | Backup, Restore & Recovery (Migration Tool) | api | public | mature |

---

## Endpoint Detail

<a id="group-auth"></a>

### Authentication, Accounts & Admin

This functional area manages HTTP web-interface endpoints for authentication, account management, and administrator session/user approval. It includes login/logout, registration, password change, session listing, and admin user approval/denial workflows. All handlers implement role-based access control via the WEB_AUTH_OR_RETURN macro (redirects unauthenticated users to /login) or explicit isAdminUser() checks for admin-only endpoints.

<a id="get"></a>

#### `GET /`

**Handler:** `handleRoot` · **Source:** `components/hardwareone/WebServer_Server.cpp:3185` · **Kind:** page · **Auth:** public · **Maturity:** mature

Entry point that redirects to the dashboard.

- **Auth detail:** No auth macro. Unconditional 302 redirect to /dashboard.
- **Parameters:** _none_
- **Request body:** none
- **Response:** HTTP 302 redirect; no body. Location: /dashboard
- **Observations:** Trivial redirect — all authenticated access gates at /dashboard via WEB_AUTH_OR_RETURN.

<a id="get-login"></a>

#### `GET /login`

**Handler:** `handleLogin` · **Source:** `components/hardwareone/WebServer_Server.cpp:3240` · **Kind:** page · **Auth:** public · **Maturity:** mature

Render the HTML login form. Shows brute-force lockout message if IP is rate-limited.

- **Auth detail:** No auth macro. Line 3254 checks isAuthed() and redirects authed users to /dashboard; unauthenticated see login form. Line 3245 enforces IP ban before rendering.
- **Parameters:** _none_
- **Request body:** none
- **Response:** text/html. Login form page with user/password fields, error message (if any), logout reason (if stored for IP).
- **Observations:** Public endpoint. Checks IP ban first (early exit 403). Authed users skip form and redirect to dashboard. Displays logout reason from prior session termination. No params.

<a id="post-login"></a>

#### `POST /login`

**Handler:** `handleLogin` · **Source:** `components/hardwareone/WebServer_Server.cpp:3240` · **Kind:** api · **Auth:** public · **Maturity:** mature

Authenticate user via username/password credentials. Enforces brute-force rate limiting, session creation, and password-change enforcement.

- **Auth detail:** No auth macro. Public form submission endpoint. IP ban check at line 3245 returns 403 if banned.
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `username` | body-form | yes | string | Username for login. Form field parsed via extractFormField(body, 'username') and URL-decoded. Empty username triggers error. |
  | `password` | body-form | yes | string | Password for login. Form field parsed via extractFormField(body, 'password'). Empty password triggers error. |

- **Request body:** application/x-www-form-urlencoded. Fields: username, password. Content-Length required.
- **Response:** On success: HTTP 303 redirect to /dashboard (or /account/password-change if user must change password). Sets Set-Cookie: session=<sid>; HttpOnly; SameSite=Strict; [Secure on HTTPS]. On failure: text/html login form with error message and attempts-remaining warning (if 2 or fewer attempts left before lockout).
- **Observations:** Rate limiting: tiered lockout at 3/5/10+ failed attempts within 5-minute window (per IP). Failed login counts tracked in sLoginAttempts static array. sTotalFailedLogins monotonic counter exposed to OLED. Audit logging via logCommandExecution. Session reuse: setSession() reuses existing sessions from same IP; multi-device login from different IPs immediately revokes prior sessions and stores logout reason. Passwords are redacted in audit logs (fakeCmd used). Clear logout reason on success. Macro recordLoginAttempt() called on both success and failure.

<a id="get-login-setsession"></a>

#### `GET /login/setsession`

**Handler:** `handleLoginSetSession` · **Source:** `components/hardwareone/WebServer_Server.cpp:3425` · **Kind:** page · **Auth:** special · **Maturity:** adequate

Secondary login endpoint for SSO/external auth flows. Sets session from gSessUser global and verifies cookie installation before redirecting.

- **Auth detail:** Public endpoint (no auth check). Checks global gSessUser — if empty, redirects to /login. gSessUser is populated by external mechanisms (e.g., OAuth/OIDC providers) and cleared after session is set.
- **Parameters:** _none_
- **Request body:** none
- **Response:** text/html. Custom page with JavaScript that: (1) checks for session cookie, (2) if found, redirects to /dashboard, (3) if not found after 1-second wait, retries and redirects to /dashboard or /login on cookie-still-missing. Also emits console.log for debug.
- **Observations:** Intended for systems that set gSessUser (e.g., OAuth callback handler) and then redirect client here. The JavaScript waits for browser to install the Set-Cookie response and does a second check — this is a workaround for browser timing on cookie application. No parameters. Clears gSessUser after use (line 3438).

<a id="get-logout"></a>

#### `GET /logout`

**Handler:** `handleLogout` · **Source:** `components/hardwareone/WebServer_Server.cpp:3226` · **Kind:** page · **Auth:** public · **Maturity:** mature

Logout the current user. Revokes session, clears session cookie, and stores logout reason.

- **Auth detail:** No auth macro. Public endpoint but calls clearSession() which extracts SID from cookie (if present) and revokes it.
- **Parameters:** _none_
- **Request body:** none
- **Response:** HTTP 302 redirect to /login. Location header set, plain text body 'Logged out'.
- **Observations:** Public but safe — clearSession() handles missing cookie gracefully. Uses writeSessionCookie(req, String()) to set Max-Age=0. Stores fixed logout reason 'You have been logged out successfully.' for the IP. No auth check allows even unauthenticated users to call it (no-op).

<a id="get-register"></a>

#### `GET /register`

**Handler:** `handleRegisterPage` · **Source:** `components/hardwareone/WebServer_Server.cpp:3471` · **Kind:** page · **Auth:** public · **Maturity:** mature

Render the account registration request form. Users enter credentials and request admin approval.

- **Auth detail:** No auth macro. Public registration form page.
- **Parameters:** _none_
- **Request body:** none
- **Response:** text/html. HTML form with fields: username, password, confirm_password. Submit button target is POST /register/submit. Also includes 'Back to Sign In' link.
- **Observations:** Public page, no auth required. Form is straightforward: 3 fields, submit via POST to /register/submit. No validation on GET side (validation in POST handler).

<a id="post-register-submit"></a>

#### `POST /register/submit`

**Handler:** `handleRegisterSubmit` · **Source:** `components/hardwareone/WebServer_Server.cpp:3508` · **Kind:** api · **Auth:** public · **Maturity:** adequate

Submit a new user account request for admin approval. Delegates to userrequest CLI command (line 3574) for consistent validation and auditing.

- **Auth detail:** No auth macro. Public form submission. Executes via makeWebAuthCtx() for audit logging but does not require authentication (unauthenticated context).
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `username` | body-form | yes | string | Desired username. URL-decoded. Validated for non-empty. |
  | `password` | body-form | yes | string | Desired password. NOT URL-decoded (line 3526). Validated for non-empty and must match confirm_password. |
  | `confirm_password` | body-form | yes | string | Password confirmation. Must match password field exactly. |

- **Request body:** application/x-www-form-urlencoded. Fields: username, password, confirm_password.
- **Response:** text/html. On success: 'Request Submitted' page with success message and button to return to /login. On failure (missing fields, password mismatch, or userrequest command fails): error page with specific error message and 'Try Again' link.
- **Observations:** Validation: empty field check (line 3529), password mismatch check (line 3550), then delegates to executeUnifiedWebCommand(req, ctx, 'userrequest ...', out). Success determined by checking for 'Request submitted for' in command output (line 3579). AuthContext created with makeWebAuthCtx() for audit purposes but user is unauthenticated (no gSessUser). Entire form is URL-decoded except password (intentional asymmetry per line 3526). Password requirements and username collision detection handled by userrequest command, not here.

<a id="get-account-password-change"></a>

#### `GET /account/password-change`

**Handler:** `handlePasswordChangePage` · **Source:** `components/hardwareone/WebServer_Server.cpp:2352` · **Kind:** page · **Auth:** session · **Maturity:** mature

Render password change form for authenticated users. Enforces mandatory password change for new/admin-created accounts before accessing other app pages.

- **Auth detail:** WEB_AUTH_OR_RETURN(req, ctx) at line 2353. Authenticated users only.
- **Parameters:** _none_
- **Request body:** none
- **Response:** text/html. Password change form page with fields: currentPassword, newPassword, confirmPassword. On GET, calls streamPageWithContent() which may redirect to /dashboard if user doesn't need to change password.
- **Observations:** Authentication required (session cookie). streamPageWithContent() enforces password-change requirement: if user doesn't need change, redirects to /dashboard (line 2135). Only allows access to password-change page if either (a) user must change or (b) is viewing the password-change page itself (line 2127). Uses page streaming (streamPasswordChangeContent function) rather than form submission.

<a id="post-account-password-change"></a>

#### `POST /account/password-change`

**Handler:** `handlePasswordChangePage` · **Source:** `components/hardwareone/WebServer_Server.cpp:2352` · **Kind:** api · **Auth:** session · **Maturity:** mature

Process password change request. Validates current password, enforces new password constraints, and revokes all other sessions for the user across all transports (web, serial, display).

- **Auth detail:** WEB_AUTH_OR_RETURN(req, ctx) at line 2353. Authenticated users only.
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `currentPassword` | body-form | yes | string | Current password for verification. Form field parsed and URL-decoded. |
  | `newPassword` | body-form | yes | string | New password. Form field parsed and URL-decoded. Validated by userChangePasswordCore(). |
  | `confirmPassword` | body-form | yes | string | Password confirmation. Form field parsed and URL-decoded. Must match newPassword. |

- **Request body:** application/x-www-form-urlencoded. Fields: currentPassword, newPassword, confirmPassword. Max 4096 bytes.
- **Response:** On success: HTTP 303 redirect to /dashboard with Cache-Control: no-cache. On failure: text/html password-change form re-rendered with error message. Error message extracted from userChangePasswordCore() result string (strips 'Error: ' prefix).
- **Observations:** Uses ExecIdentityGuard to install web AuthContext into TLS so userChangePasswordCore() can call currentExecUser() and revokeUserSessions() with proper context. Success determined by absence of 'Error' prefix in result (line 2414). revokeUserSessions() skips the current session (ctx.sid + SOURCE_WEB) so user stays logged into current browser tab. Passwords are URL-decoded (line 3389-3391). Memory allocation via ps_alloc(total_len + 1, PreferPSRAM, 'http.pwchg') minimizes heap fragmentation.

<a id="get-api-sessions"></a>

#### `GET /api/sessions`

**Handler:** `handleSessionsList` · **Source:** `components/hardwareone/WebServer_Server.cpp:2742` · **Kind:** api · **Auth:** admin · **Maturity:** mature

List all active sessions. Admin-only view of all logged-in users.

- **Auth detail:** WEB_AUTH_OR_RETURN(req, ctx) at line 2744. Then explicit isAdminUser(ctx.user) check (line 2745). Admin-only.
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json. { "success": true, "sessions": [ { "sid": "<token>", "user": "<username>", "createdAt": <epoch ms>, "lastSeen": <epoch ms>, "expiresAt": <epoch ms>, "ip": "<ip>", "current": <bool> }, ... ] }
- **Observations:** Admin-only. Uses buildAllSessionsJson() helper to iterate gSessions array and serialize. Converts boot-relative milliseconds to epoch milliseconds for JavaScript Date() consumption. currentSid marked with 'current': true. IPs shown as '-' if empty.

<a id="get-api-admin-sessions"></a>

#### `GET /api/admin/sessions`

**Handler:** `handleAdminSessionsList` · **Source:** `components/hardwareone/WebServer_Server.cpp:2766` · **Kind:** api · **Auth:** admin · **Maturity:** mature

Admin sessions list (identical to /api/sessions). May be used by admin UI as an explicit admin endpoint.

- **Auth detail:** WEB_AUTH_OR_RETURN(req, ctx) at line 2767. Then explicit isAdminUser(ctx.user) check (line 2768). Admin-only.
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json. { "success": true, "sessions": [ { "sid": "<token>", "user": "<username>", "createdAt": <epoch ms>, "lastSeen": <epoch ms>, "expiresAt": <epoch ms>, "ip": "<ip>", "current": <bool> }, ... ] }
- **Observations:** Duplicate of /api/sessions but routed separately. Both endpoints call buildAllSessionsJson(). No additional parameters or filtering.

<a id="get-api-admin-pending"></a>

#### `GET /api/admin/pending`

**Handler:** `handleAdminPending` · **Source:** `components/hardwareone/WebServer_Server.cpp:4385` · **Kind:** api · **Auth:** admin · **Maturity:** adequate

List pending user account requests awaiting admin approval.

- **Auth detail:** WEB_AUTH_OR_RETURN(req, ctx) at line 4386. Then explicit isAdminUser(ctx.user) check (line 4388). Admin-only.
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json. { "success": true, "pending": [ { "username": "<name>", "timestamp": <ms or integer> }, ... ] }
- **Observations:** Reads /system/users/pending_users.json and sanitizes output: explicitly excludes password field (line 4413) before serializing. Uses VFS::openGuarded() and passes caller's AuthContext for [PERM] audit logging. Falls back to empty pending array on file-not-found or JSON error. JSON response buffer guarded to avoid alloc contention.

<a id="post-api-admin-approve"></a>

#### `POST /api/admin/approve`

**Handler:** `handleAdminApproveUser` · **Source:** `components/hardwareone/WebServer_Server.cpp:4432` · **Kind:** api · **Auth:** admin · **Maturity:** adequate

Approve a pending user account request. Transitions user from pending to active.

- **Auth detail:** WEB_AUTH_OR_RETURN(req, ctx) at line 4433. Then explicit isAdminUser(ctx.user) check (line 4434). Admin-only.
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `username` | body-form | yes | string | Username of pending user to approve. Form field parsed and URL-decoded. Empty username rejected with error. |

- **Request body:** application/x-www-form-urlencoded. Field: username.
- **Response:** application/json. On success: { "success": true }. On failure: { "success": false, "error": "<error message>" }.
- **Observations:** Calls approvePendingUserInternal(username, err) helper. Error string returned in JSON if approval fails. Delegates user state transitions to separate helper function.

<a id="post-api-admin-reject"></a>

#### `POST /api/admin/reject`

**Handler:** `handleAdminDenyUser` · **Source:** `components/hardwareone/WebServer_Server.cpp:4474` · **Kind:** api · **Auth:** admin · **Maturity:** adequate

Reject/deny a pending user account request. Removes user from pending list.

- **Auth detail:** WEB_AUTH_OR_RETURN(req, ctx) at line 4475. Then explicit isAdminUser(ctx.user) check (line 4477). Admin-only.
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `username` | body-form | yes | string | Username of pending user to reject. Form field parsed and URL-decoded. Empty username rejected with error. |

- **Request body:** application/x-www-form-urlencoded. Field: username.
- **Response:** application/json. On success: { "success": true }. On failure: { "success": false, "error": "<error message>" }.
- **Observations:** Calls denyPendingUserInternal(username, err) helper. Symmetric to approve endpoint. Error string returned in JSON.

<a id="get-api-notice"></a>

#### `GET /api/notice`

**Handler:** `handleNotice` · **Source:** `components/hardwareone/WebServer_Server.cpp:2871` · **Kind:** api · **Auth:** session · **Maturity:** mature

Dequeue one notice from the session's notice ring. Used by web UI to receive SSE revoke notices, session invalidation messages, etc.

- **Auth detail:** Manual auth check at line 2879: calls isAuthed(req, user). Returns 401 JSON if unauthenticated. Not using WEB_AUTH_OR_RETURN macro (custom error handling for JSON).
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json. { "success": true, "notice": "<message>" }. On auth failure: { "success": false, "error": "Authentication required" } (HTTP 401).
- **Observations:** Authenticated users only. Dequeues via sseDequeueNotice() (line 2892) which pops from circular ring. If notice starts with '[revoke]', immediately clears the session (line 2896) and sets Max-Age=0 cookie. Notice message JSON-escaped to prevent injection. Message is limited to fit in 256-byte buffer (line 2903). No params; purely stateful dequeue.


<a id="group-system"></a>

### System, Dashboard & Core

System, Dashboard & Core functionality. Provides the main dashboard page, system status API, device registry, build configuration discovery, output control, real-time event streaming via Server-Sent Events (SSE), embedded icon serving, and browser favicon responses. Core endpoints include authenticated page views and JSON APIs for system monitoring, configuration, and runtime state management.

<a id="get-dashboard"></a>

#### `GET /dashboard`

**Handler:** `handleDashboard` · **Source:** `components/hardwareone/WebServer_Server.cpp:2337` · **Kind:** page · **Auth:** session · **Maturity:** adequate

Render the main dashboard page for authenticated users with real-time system status display.

- **Auth detail:** WEB_AUTH_OR_RETURN macro requires authenticated session; tgRequireAuth checks session validity
- **Parameters:** _none_
- **Request body:** none
- **Response:** text/html; chunked. Complete HTML page with navigation, CSS, and embedded JavaScript.
- **Observations:** Page handler calls streamPageWithContent with streamDashboardContent callback. Dashboard page fetches data from /api/system and /api/events (SSE) endpoints. Uses WEB_AUTH_OR_RETURN macro for authentication. No parameters accepted.

<a id="get-api-ping"></a>

#### `GET /api/ping`

**Handler:** `handlePing` · **Source:** `components/hardwareone/WebServer_Server.cpp:3193` · **Kind:** api · **Auth:** public · **Maturity:** mature

Health check and device discovery endpoint for external tools and migration utilities. Returns device fingerprint, firmware version, and configuration state.

- **Auth detail:** No authentication macro; handler is PUBLIC. CORS headers set (Access-Control-Allow-Origin: *) for migration tool access.
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json. JSON object with keys: ok (boolean), hostname (string), mac (string), fingerprint (string), firmwareVersion (string), acceptingRestore (boolean), https (boolean, only when ENABLE_HTTPS).
- **Observations:** No parameters accepted. Returns raw device identifiers (MAC, fingerprint) without authentication—this is intentional for discovery. CORS enabled for cross-origin access. Includes acceptingRestore flag indicating whether firmware restoration is in progress. https field conditionally included based on ENABLE_HTTPS build flag.

<a id="get-api-system"></a>

#### `GET /api/system`

**Handler:** `handleSystemStatus` · **Source:** `components/hardwareone/WebServer_Server.cpp:2984` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Return comprehensive system information for dashboard and monitoring. Includes memory stats, connectivity status, discovered I2C devices, and sensor state.

- **Auth detail:** WEB_AUTH_OR_RETURN macro requires authenticated session.
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json. Calls buildSystemInfoJson() to populate document, then serializes to static 4096-byte buffer. Includes system info (uptime, memory, CPU stats), connectivity details (WiFi, Bluetooth), device list (I2C devices), sensors, and runtime state.
- **Observations:** Uses static 4KB buffer to avoid allocation overhead. serializeJson silently truncates if response exceeds buffer—this is a silent failure mode for large device lists. buildSystemInfoJson conditionally includes device list (size unbounded by MAX_DEVICES, ~50B per device). Dashboard likely polls this endpoint regularly.

<a id="get-api-buildconfig"></a>

#### `GET /api/buildconfig`

**Handler:** `handleBuildConfig` · **Source:** `components/hardwareone/WebServer_Server.cpp:2705` · **Kind:** api · **Auth:** session · **Maturity:** mature

Discover available hardware and software features compiled into this firmware build. Allows UI to conditionally show/hide feature-specific controls.

- **Auth detail:** WEB_AUTH_OR_RETURN macro requires authenticated session.
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json with Cache-Control: max-age=3600. JSON object with boolean flags for each compiled feature: camera, microphone, bluetooth, g2glasses, mqtt, espnow, edgeimpulse, espsr, automation, gps, imu, thermal, tof, gamepad, apds, fmradio, rtc, presence.
- **Observations:** Hardcoded JSON output with static buffer (512 bytes). Each feature maps to a compile-time flag (ENABLE_* macros). Cache control header set to 1 hour since build config never changes at runtime. No parameters. No error conditions—if auth passes, response always succeeds.

<a id="get-api-devices"></a>

#### `GET /api/devices`

**Handler:** `handleDeviceRegistryGet` · **Source:** `components/hardwareone/WebServer_Server.cpp:2683` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Return list of all I2C-attached devices (sensors, peripherals) with their properties. Used by web UI to display hardware inventory.

- **Auth detail:** WEB_AUTH_OR_RETURN macro requires authenticated session.
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json, no-cache. Calls buildDeviceRegistryJson() (System_I2C.cpp) to populate document, then serializes to static 4096-byte buffer. Returns JSON object describing all discovered I2C devices and their properties.
- **Observations:** Shares implementation with CLI 'devicefile' command—states it is a 'single source of truth' byte-compatible schema. Static 4KB buffer. Falls back to empty object {} on allocation failure. Cache-Control: no-cache. No parameters.

<a id="get-api-events"></a>

#### `GET /api/events`

**Handler:** `handleEvents` · **Source:** `components/hardwareone/WebServer_Events.cpp:144` · **Kind:** stream · **Auth:** session · **Maturity:** adequate

Real-time Server-Sent Events (SSE) stream for live updates to dashboard and other pages. Sends sensor status, system state changes, and application events. Connection is kept-alive; client retries if disconnected.

- **Auth detail:** Uses makeWebAuthCtx + tgRequireAuth; returns ESP_OK (closes stream) if auth fails.
- **Parameters:** _none_
- **Request body:** none
- **Response:** text/event-stream with keep-alive. SSE stream: initial ':ok' comment + 'retry: <ms>' directive, then 'event: sensor-status' (JSON sensor snapshot), 'event: system' (JSON system snapshot), 'event: <typeName>' (for typed enqueued events like espnow-rx). Stream holds open for ~600ms to deliver notices and events in batch, then closes.
- **Observations:** Complex session-binding logic: sseBindSession binds the HTTP connection to a session entry, enqueues sensor + system snapshots, and holds the connection open to deliver all queued notices (up to 8) and events (up to 8) in one batch before closing. Retry interval is 1000ms if notice pending, else 5000ms. No query parameters. gSessions global is used to find / update the session entry. Potential bug: serializeJson silently truncates system JSON if exceeds 4096-byte buffer, producing invalid JSON on dashboard.

<a id="get-api-output"></a>

#### `GET /api/output`

**REMOVED 2026-07-19** with the output-channels trim: no first-party consumer existed (settings UI uses the schema/command path; companion app has no references), and the persisted web/display/g2 lanes it reported were removed after an audit showed delivery never honored them. Historical description follows.

**Handler:** `handleOutputGet` · **Source:** `components/hardwareone/WebServer_Server.cpp:2788` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Query current output routing configuration—both persisted settings and current runtime state. Distinguishes between stored preferences and temporary overrides.

- **Auth detail:** WEB_AUTH_OR_RETURN macro requires authenticated session.
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json. JSON object with shape: {success: true, persisted: {serial, web, display, g2}, runtime: {serial, web, display, g2}}. Each flag is integer 0 or 1.
- **Observations:** Returns both persisted settings (gSettings.outSerial, etc.) and runtime flags (gOutputFlags bitmask). g2 output only included when ENABLE_BLUETOOTH && ENABLE_G2_GLASSES. Inconsistent error handling: success always true, never conditional on query params (none accepted). No parameters.

<a id="post-api-output-temp"></a>

#### `POST /api/output/temp`

**REMOVED 2026-07-19** with the output-channels trim: no first-party consumer, and two of the three commands it issued (outweb/outdisplay) were removed as decorative. Runtime lane control remains available via the CLI (`outserial ... temp`, `outg2`, `outble`). Historical description follows.

**Handler:** `handleOutputTemp` · **Source:** `components/hardwareone/WebServer_Server.cpp:2813` · **Kind:** api · **Auth:** session · **Maturity:** barebones

Temporarily override output routing (runtime only, not persisted). Changes are routed through unified command system (outserial/outweb/outdisplay commands with 'temp' flag) for auditability.

- **Auth detail:** WEB_AUTH_OR_RETURN macro requires authenticated session.
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `serial` | body-form | no | integer (0 or 1) | Temporary runtime override for serial output routing |
  | `web` | body-form | no | integer (0 or 1) | Temporary runtime override for web output routing |
  | `display` | body-form | no | integer (0 or 1) | Temporary runtime override for display output routing |

- **Request body:** application/x-www-form-urlencoded: serial=0/1&web=0/1&display=0/1. Each parameter optional; missing parameters are ignored.
- **Response:** application/json. JSON object: {success: true, runtime: {serial, web, display}}. Each flag is integer 0 or 1.
- **Observations:** Form body is read with httpd_req_recv in a loop (max 256 bytes). Each provided key-value pair is parsed with httpd_query_key_value and executed as a separate CLI command through executeUnifiedWebCommand for audit trail. No atomicity guarantee if parsing fails mid-stream. Params not provided default to -1 and are skipped. Responds with current runtime state after applying changes.

<a id="get-api-icon"></a>

#### `GET /api/icon`

**Handler:** `handleIconGet` · **Source:** `components/hardwareone/WebServer_Server.cpp:4234` · **Kind:** asset · **Auth:** public · **Maturity:** adequate

Serve embedded 32x32 PNG icons for use in the web UI. Icons are stored in program memory (PROGMEM) as compiled-in resources.

- **Auth detail:** No authentication; PUBLIC endpoint. No WEB_AUTH* macro.
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `name` | query | yes | string | Icon name to retrieve (e.g., 'folder', 'file_code', 'download') |
  | `debug` | query | no | string (0/1/t/T/y/Y) | Optional debug flag; if present and truthy, adds X-Icon-* response headers |

- **Request body:** none
- **Response:** image/png with Cache-Control: public, max-age=86400. Raw PNG binary data from embedded icon resource. When debug=1, also sets X-Icon-Name, X-Icon-Size, X-Icon-Status response headers.
- **Observations:** Calls findEmbeddedIcon() to locate icon by name. If not found, returns 404 with text 'Icon not found'. If found, allocates temporary buffer, copies PNG data from PROGMEM, sends in single response (Safari-safe: no chunking for binary). Icons are always tiny (32x32), so malloc+memcpy is acceptable. Debug mode exposes icon size via header. No validation of icon name—arbitrary string lookup.

<a id="get-icons-test"></a>

#### `GET /icons/test`

**Handler:** `handleIconTestPage` · **Source:** `components/hardwareone/WebServer_Server.cpp:4310` · **Kind:** page · **Auth:** session · **Maturity:** barebones

Admin/debugging page to browse and preview all embedded icons in the system. Useful for verifying icon availability and name discovery.

- **Auth detail:** WEB_AUTH_OR_RETURN macro requires authenticated session.
- **Parameters:** _none_
- **Request body:** none
- **Response:** text/html; charset=utf-8. Complete HTML page with inline CSS and JavaScript. Displays all embedded icons in a grid layout with filtering and size controls (32px, 64px, 128px). Each icon shown with name and dimensions.
- **Observations:** Iterates EMBEDDED_ICONS[] array (read from PROGMEM) and generates an <img> tag for each icon using /api/icon?name=<iconName>. Client-side JavaScript allows dynamic filtering by icon name and resizing. Inline styling (no external CSS). Uses inlining to construct HTML String (potential for large page if icon count is high).

<a id="get-faviconico"></a>

#### `GET /favicon.ico`

**Handler:** `handleBrowserIcon` · **Source:** `components/hardwareone/WebServer_Server.cpp:4697` · **Kind:** asset · **Auth:** public · **Maturity:** mature

Standard favicon endpoint. Returns 204 to suppress browser error logs when favicon is not available. No favicon file served.

- **Auth detail:** No authentication; PUBLIC endpoint. Handler is empty stub returning 204 No Content.
- **Parameters:** _none_
- **Request body:** none
- **Response:** 204 No Content (empty body).
- **Observations:** Stub handler returns 204 No Content immediately. Same handler is reused for /apple-touch-icon.png and /apple-touch-icon-precomposed.png. This is a common pattern to silence browser requests for these assets without serving actual files.

<a id="get-apple-touch-iconpng"></a>

#### `GET /apple-touch-icon.png`

**Handler:** `handleBrowserIcon` · **Source:** `components/hardwareone/WebServer_Server.cpp:4697` · **Kind:** asset · **Auth:** public · **Maturity:** mature

Standard iOS/macOS touch icon endpoint. Returns 204 to suppress browser error logs when icon is not available. No icon file served.

- **Auth detail:** No authentication; PUBLIC endpoint. Handler is empty stub returning 204 No Content.
- **Parameters:** _none_
- **Request body:** none
- **Response:** 204 No Content (empty body).
- **Observations:** Stub handler returns 204 No Content immediately. Shared with /favicon.ico handler. Prevents browser console noise for missing Apple touch icons.

<a id="get-apple-touch-icon-precomposedpng"></a>

#### `GET /apple-touch-icon-precomposed.png`

**Handler:** `handleBrowserIcon` · **Source:** `components/hardwareone/WebServer_Server.cpp:4697` · **Kind:** asset · **Auth:** public · **Maturity:** mature

Legacy iOS touch icon endpoint. Returns 204 to suppress browser error logs when precomposed icon is not available. No icon file served.

- **Auth detail:** No authentication; PUBLIC endpoint. Handler is empty stub returning 204 No Content.
- **Parameters:** _none_
- **Request body:** none
- **Response:** 204 No Content (empty body).
- **Observations:** Stub handler returns 204 No Content immediately. Shared with /favicon.ico and /apple-touch-icon.png handler. Covers all three common browser touch-icon requests with a single stub.


<a id="group-settings"></a>

### Settings

The Settings functional area provides HTTP endpoints for retrieving and updating system and user-specific configuration. The /settings page endpoint renders an authenticated HTML interface that loads settings data dynamically via /api/settings (current values) and /api/settings/schema (metadata for form generation). The /api/user/settings endpoints allow authenticated users to retrieve and update their personal preferences. All endpoints require session authentication via the WEB_AUTH_OR_RETURN macro. The architecture separates data APIs (JSON) from page rendering, enabling the web UI to dynamically build forms based on schema metadata without hardcoding field definitions.

<a id="get-settings"></a>

#### `GET /settings`

**Handler:** `handleSettingsPage` · **Source:** `components/hardwareone/WebServer_Server.cpp:2344` · **Kind:** page · **Auth:** session · **Maturity:** adequate

Render the settings page UI. Authenticated users see a complete HTML page with a settings form that fetches configuration via /api/settings API endpoint.

- **Auth detail:** Uses WEB_AUTH_OR_RETURN macro (line 2345) which calls makeWebAuthCtx + tgRequireAuth; returns ESP_OK on auth failure (user is redirected to /login)
- **Parameters:** _none_
- **Request body:** none
- **Response:** text/html - HTML page with full layout including navigation, header, footer, and settings UI using streamPageWithContent. The page calls JavaScript to load /api/settings and /api/settings/schema for dynamic content rendering.
- **Observations:** Page-level handler; the actual settings data is fetched by client-side JavaScript via /api/settings and /api/settings/schema. Enforces one-time password change redirect (redirects to /account/password-change if user must change password). streamPageWithContent sets Content-Type: text/html and suspends mesh activity during page generation.

<a id="get-api-settings"></a>

#### `GET /api/settings`

**Handler:** `handleSettingsGet` · **Source:** `components/hardwareone/WebServer_Server.cpp:2444` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Return current system settings as JSON for dynamic UI rendering. Includes user info and available features.

- **Auth detail:** Uses WEB_AUTH_OR_RETURN macro (line 2445) which calls makeWebAuthCtx + tgRequireAuth; returns ESP_OK on auth failure (no response body)
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json - JSON object with keys: {success: bool, settings: {all registered system settings with values}, user: {username, isAdmin}, features: {adminSessions, userApprovals, adminControls, sensorConfig, bluetooth, espnow}}. WiFi passwords are excluded from the web API response (excludeWifiPasswords=true passed to buildSettingsJsonDoc).
- **Observations:** Uses shared JSON response buffer (gJsonResponseBuffer) with mutex guard (JsonBufferGuard). Passwords excluded from response for security. Features object exposes boolean flags for feature availability (adminSessions always true for session-authed users, others static). Settings come from buildSettingsJsonDoc which collects from all registered settings modules. Cache-Control: no-cache header set.

<a id="get-api-settings-schema"></a>

#### `GET /api/settings/schema`

**Handler:** `handleSettingsSchema` · **Source:** `components/hardwareone/WebServer_Server.cpp:2501` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Provide metadata schema for all system settings to enable dynamic form rendering on the client.

- **Auth detail:** Uses WEB_AUTH_OR_RETURN macro (line 2502) which calls makeWebAuthCtx + tgRequireAuth; returns ESP_OK on auth failure
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json - Settings schema metadata for dynamic UI rendering. Built by buildSettingsSchemaJson() which returns structured metadata about each setting (groups, fields, types, validation rules, display hints). Includes 'count' field for module count.
- **Observations:** Uses shared JSON response buffer with mutex guard. buildSettingsSchemaJson is called (same builder used by bond-peer schema transfer). Debug logging shows serialization status. Cache-Control: no-cache header set. Timeout logic for buffer acquisition.

<a id="get-api-user-settings"></a>

#### `GET /api/user/settings`

**Handler:** `handleUserSettingsGet` · **Source:** `components/hardwareone/WebServer_Server.cpp:2546` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Retrieve user-specific preferences and settings (e.g., theme, UI preferences) for the authenticated user.

- **Auth detail:** Uses WEB_AUTH_OR_RETURN macro (line 2547) which calls makeWebAuthCtx + tgRequireAuth; returns ESP_OK on auth failure
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json - JSON object with keys: {success: bool, userId: uint32, mustChangePassword: bool, settings: {user-specific settings object}}. Password fields removed from response (password and gamepad_password keys stripped). Returns error objects on failure: {success: false, error: 'user_not_found'|'read_failed'}.
- **Observations:** Per-user settings loaded via loadUserSettings(userId) after username lookup via getUserIdByUsername(). Removes sensitive fields (password, gamepad_password, mustChangePassword) from response before serialization. Cache-Control: no-cache header. Error responses use standard {success, error} envelope.

<a id="post-api-user-settings"></a>

#### `POST /api/user/settings`

**Handler:** `handleUserSettingsSet` · **Source:** `components/hardwareone/WebServer_Server.cpp:2604` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Update user-specific settings for the authenticated user. Supports JSON patch-style merge (only provided fields are updated).

- **Auth detail:** Uses WEB_AUTH_OR_RETURN macro (line 2605) which calls makeWebAuthCtx + tgRequireAuth; returns ESP_OK on auth failure
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `content_len (implicit)` | body-json | yes | integer | HTTP Content-Length header; validated to be > 0 and <= 4096 bytes |

- **Request body:** application/json - JSON patch object with arbitrary user-setting fields to merge. Example: {"theme": "dark", "notifications": true}. Password fields are rejected (attempting to set password or gamepad_password returns 400 Bad Request with error='password_not_allowed').
- **Response:** application/json - On success: {success: true}. On error: {success: false, error: 'user_not_found'|'invalid_content_length'|'oom'|'recv_failed'|'invalid_json'|'password_not_allowed'|'write_failed'}. HTTP status 400 Bad Request for JSON/content-length errors.
- **Observations:** Content length validated (0 < len <= 4096). Reads body via httpd_req_recv with retry on HTTPD_SOCK_ERR_TIMEOUT. JSON deserialized with ArduinoJson. Password field checks are explicit (patch['password'].isNull() + patch['gamepad_password'].isNull()) to prevent storing unhashed passwords. Calls mergeAndSaveUserSettings(userId, patch) for atomic merge+save. Debug logging shows theme or patch key count.


<a id="group-files"></a>

### File System

The File System functional area provides HTTP endpoints for browsing, uploading, downloading, editing, and managing files on the device's LittleFS and SD card filesystems. The primary page (/files) serves an interactive file manager UI, while the JSON API (/api/files/*) handles CRUD operations, storage statistics, and streaming file I/O with role-based access control and admin-only path restrictions. All endpoints enforce session-based authentication; some additionally check admin role via isAdminUser() for operations on protected paths. File operations route through a guarded VFS layer (VFS::openGuarded, canImport, isAdminOnlyPath) that enforces permission rules, blocks sensitive file extensions, prevents path traversal, and respects both user identity and role-based permissions.

<a id="get-files"></a>

#### `GET /files`

**Handler:** `handleFilesPage()` · **Source:** `components/hardwareone/WebServer_Server.cpp:3168` · **Kind:** page · **Auth:** session · **Maturity:** mature

Render the Files Manager page — a browser-based UI for browsing, uploading, editing, and downloading files on the device filesystem.

- **Auth detail:** WEB_AUTH_OR_RETURN macro enforces session authentication; redirects to /login if not authenticated.
- **Parameters:** _none_
- **Request body:** none
- **Response:** text/html; HTML page with embedded JavaScript for file management. Streams base page template + streamFilesContent, which includes file browser scripts, HTML structure, editor modal, storage stats UI, and JavaScript initialization code calling /api/files/list and /api/files/stats to populate the UI.
- **Observations:** Page calls multiple API endpoints in embedded JavaScript: /api/files/list (to populate file browser), /api/files/stats (to display storage usage bar), /api/files/read (to load file content for editing), /api/files/write (to save edited files). Also calls /api/files/view for reading raw files. The page includes bonded-device file browser support via window.BondFs. Pretty-JSON and raw-JSON display modes for .json files. No pagination or filtering parameters on the page itself; all filtering happens client-side via API responses.

<a id="get-api-files-list"></a>

#### `GET /api/files/list`

**Handler:** `handleFilesList()` · **Source:** `components/hardwareone/WebServer_Server.cpp:3639` · **Kind:** api · **Auth:** session · **Maturity:** mature

List files and directories in a given path with permission indicators. Permissions (canRead, canWrite, canDelete) reflect what the authenticated user can do in that directory.

- **Auth detail:** WEB_AUTH_OR_RETURN macro enforces session authentication. Additionally, isAdminOnlyPath() + isAdminUser() check gates admin-only paths; non-admin users get 403 Forbidden for such paths.
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `path` | query | no | string (URL-encoded path) | Directory path to list. Defaults to '/'. URL-decoded to interpret %2F as '/' and %20 as space. Admin-only paths (determined by isAdminOnlyPath()) return 403 if caller is not admin. |

- **Request body:** none
- **Response:** application/json; returns {"success":true,"dirPerms":{"canRead":bool,"canWrite":bool,"canDelete":bool},"files":[{"name":string,"type":"file"|"dir","size":number,"modified":number_epoch},{...}]} on success; {"success":false,"error":"..."} on failure (e.g., filesystem not ready, admin required, invalid path).
- **Observations:** Calls shared buildFilesListJson() — single source of truth for {success,dirPerms,files[]} envelope used by both web and CLI/BLE commands. Permissions vary by role (admin gets more rights). hideAdminPaths flag controls visibility of admin-only paths for non-admin users. No pagination implemented; response contains all files in the directory.

<a id="get-api-files-stats"></a>

#### `GET /api/files/stats`

**Handler:** `handleFilesStats()` · **Source:** `components/hardwareone/WebServer_Server.cpp:3678` · **Kind:** api · **Auth:** session · **Maturity:** mature

Get storage statistics (total, used, free bytes and usage percentage) for a given path. Automatically selects SD card stats if path is under /sd/ and SD is available; otherwise returns internal LittleFS stats.

- **Auth detail:** WEB_AUTH_OR_RETURN macro enforces session authentication.
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `path` | query | no | string (URL-encoded path) | Path to query storage stats for (determines whether to use SD card or internal LittleFS stats). Defaults to '/'. URL-decoded using urlDecode() before processing. |

- **Request body:** none
- **Response:** application/json; returns {"success":true,"total":number,"used":number,"free":number,"usagePercent":number} on success; {"success":false,"error":"..."} on failure (e.g., filesystem not ready, SD card not available for SD paths).
- **Observations:** Calls shared buildFilesStatsJson() — single source of truth for envelope and SD-availability logic. Percent is already computed (0-100). No authentication gates per-path stats; any authenticated user can query stats for any path.

<a id="post-api-files-create"></a>

#### `POST /api/files/create`

**Handler:** `handleFilesCreate()` · **Source:** `components/hardwareone/WebServer_Server.cpp:3705` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Create a new file or folder on the filesystem. For folders, routes through 'mkdir' CLI command; for files, routes through 'filecreate' CLI command for consistent validation.

- **Auth detail:** WEB_AUTH_OR_RETURN macro enforces session authentication. Additionally, isAdminOnlyPath() + isAdminUser() check gates creation in admin-only paths; non-admin users get 403 Forbidden.
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `name` | body-form | yes | string (URL-encoded) | File or folder name to create (e.g. 'config.json' or 'myfolder'). URL-decoded to interpret %20 as space and %2F as '/'. Leading '/' is stripped. Path constructed as '/' + name. Required; request fails with error if missing. |
  | `type` | body-form | yes | string ('folder' \| any other value treated as file extension) | Type of object to create: 'folder' creates a directory via 'mkdir' command; any other value (e.g. 'json', 'txt') is treated as a file extension and appended to name if not already present (e.g. name='config', type='json' -> path='/config.json'). Required. |

- **Request body:** application/x-www-form-urlencoded; name=<urlencoded name>&type=<type>
- **Response:** application/json; returns {"success":true} on success; {"success":false,"error":"<message>"} on failure (e.g., 'Name required', 'Admin required', 'File already exists', or error from mkdir/filecreate CLI commands).
- **Observations:** Delegates to unified web command executor (executeUnifiedWebCommand) for mkdir/filecreate, which means error messages are sourced from the CLI implementation. Admin-only path check prevents non-admin users from creating files in protected directories. No direct filesystem syscalls — all I/O routes through CLI commands for consistency with BLE/web-CLI. File creation does not verify if file already exists before attempting creation.

<a id="get-api-files-view"></a>

#### `GET /api/files/view`

**Handler:** `handleFileView()` · **Source:** `components/hardwareone/WebServer_Server.cpp:3795` · **Kind:** api · **Auth:** session · **Maturity:** mature

Render a file in the browser with type-aware handling: JSON files get pretty-print UI with syntax highlighting, images are displayed directly, audio files stream with browser player controls, binary .hwmap files download, and text files stream as text/plain.

- **Auth detail:** Manual AuthContext setup + tgRequireAuth() call (equivalent to WEB_AUTH_OR_RETURN macro). Additionally, isAdminOnlyPath() + isAdminUser() check gates admin-only paths; non-admin users get 403 Forbidden. VFS::openGuarded() enforces role-based read permissions via the permission rule table.
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `name` | query | yes | string (URL-encoded path) | File path to view. URL-decoded using full hex-escape (%NN) and '+' -> ' ' decoding. Required; request returns 'Invalid filename' if missing. |
  | `mode` | query | no | string ('pretty' \| 'raw') | Display mode for .json files: 'pretty' (default) for pretty-printed with syntax highlighting; 'raw' for minified JSON. Ignored for non-.json files. |

- **Request body:** none
- **Response:** Content-Type varies by file type: text/html (for .json files with pretty/raw toggle UI); image/* (for .jpg/.png/.gif/.bmp/.webp/.ico/.svg); audio/* (for .wav/.mp3 with Content-Disposition and Accept-Ranges headers); application/octet-stream (for .hwmap binary downloads); text/plain (default for other files). Streamed chunked in 4KB buffers. For .json files, returns HTML page with embedded toggle buttons (Pretty/Raw) and <pre> block containing JSON content.
- **Observations:** Special handling for .json (HTML wrapper with pretty-print mode), images (.jpg/.png/.gif/.bmp/.webp/.ico/.svg), audio (.wav/.mp3 with seekable playback support), and binary downloads (.hwmap). Sensor polling is paused during streaming to prevent I2C contention. Streaming uses 4KB PSRAM buffers for throughput. Audio files <5 MB are read into memory for seekable playback (Content-Length + Accept-Ranges); larger files stream chunked (no seek). No rate-limiting; large files can consume substantial memory and streaming time. Admin-only path check is redundant with VFS::openGuarded() permission enforcement but provides clearer error message.

<a id="get-api-files-read"></a>

#### `GET /api/files/read`

**Handler:** `handleFileRead()` · **Source:** `components/hardwareone/WebServer_Server.cpp:1584` · **Kind:** api · **Auth:** session · **Maturity:** mature

Read and download raw file content as plain text. Used by the web UI to load file content into the editor modal (/api/files/read in the editFile() JavaScript).

- **Auth detail:** Manual AuthContext setup + tgRequireAuth() call (equivalent to WEB_AUTH_OR_RETURN macro). Additionally, isAdminOnlyPath() + isAdminUser() check gates admin-only paths; non-admin users get 403 Forbidden. VFS::openGuarded() enforces role-based read permissions and blocks sensitive-extension files.
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `name` | query | yes | string (URL-encoded path) | File path to read. URL-decoded to interpret %2F as '/' and %20 as space. Required; request returns 'Invalid filename' if missing. |

- **Request body:** none
- **Response:** text/plain; charset=utf-8; raw file content streamed in 512-byte chunks. Returns plain text error messages ('No filename specified', 'Invalid filename', 'File not found', 'Forbidden: admin required') on error.
- **Observations:** Sensor polling is paused during streaming to prevent I2C contention. VFS::openGuarded() enforces role-based read permissions and rejects sensitive-extension files (.bin/.crt/.key/.pem/.credentials/.elf/.hex). Path normalization rejects '..' traversal attempts. No Content-Length header; response is chunked. No rate-limiting on file size.

<a id="post-api-files-write"></a>

#### `POST /api/files/write`

**Handler:** `handleFileWrite()` · **Source:** `components/hardwareone/WebServer_Server.cpp:1683` · **Kind:** api · **Auth:** session · **Maturity:** mature

Write (create or overwrite) file content. Reads content from form body, URL-decodes it, and writes to the specified path. Post-save hooks (automations.json sanitization) are triggered after successful write.

- **Auth detail:** Manual AuthContext setup + tgRequireAuth() call (equivalent to WEB_AUTH_OR_RETURN macro). Additionally, isAdminOnlyPath() + isAdminUser() check gates admin-only paths; non-admin users get 403 Forbidden. VFS::openGuarded() enforces role-based write permissions and blocks sensitive-extension files.
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `name` | body-form | yes | string (URL-encoded path) | File path to write. URL-decoded to interpret %2F as '/' and %20 as space. Required; request fails with error 'Name required' if missing. |
  | `content` | body-form | yes | string (URL-encoded text) | File content to write. URL-decoded to interpret %20 as space, %0A as newline, %0D as carriage return, %2F as '/', %3A as ':', %2C as ',', %7B as '{', %7D as '}', %22 as '"', %5B as '[', %5D as ']', %25 as '%'. Limit 150 KB per request. |

- **Request body:** application/x-www-form-urlencoded; name=<urlencoded path>&content=<urlencoded text>. Content-Length must be between 1 and 150 KB.
- **Response:** application/json; returns {"success":true} on success; {"success":false,"error":"<message>"} on failure (e.g., 'Name required', 'Filesystem not initialized', 'Invalid content length', 'Writes to this path are not allowed', 'Write failed (short write)', 'Admin required').
- **Observations:** 150 KB limit enforced; requests exceeding this size are rejected. Content is read into PSRAM if available. Short write detection (write() returns fewer bytes than requested) terminates with error. Post-save hooks run runFileWritePostSaveHooks() for automations.json file only. VFS::openGuarded() blocks writes to sensitive-extension files (.bin/.crt/.key/.pem/.credentials/.elf/.hex/.jpg/.png/.gif). Path normalization rejects '..' traversal. No rate-limiting on write frequency.

<a id="post-api-files-upload"></a>

#### `POST /api/files/upload`

**Handler:** `handleFileUpload()` · **Source:** `components/hardwareone/WebServer_Server.cpp:1836` · **Kind:** api · **Auth:** session · **Maturity:** mature

Upload and stream file content to the device with chunked parsing. Supports both text (URL-encoded) and binary (base64-encoded) uploads. Automatically creates parent directories. Post-save hooks (automations.json, maps organization) are triggered after successful upload.

- **Auth detail:** Manual AuthContext setup + tgRequireAuth() call (equivalent to WEB_AUTH_OR_RETURN macro). Additionally, isAdminOnlyPath() + isAdminUser() check gates admin-only paths; non-admin users get 403 Forbidden. canImport() check gates per-role import permissions and sensitive-extension blocks.
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `path` | body-form | yes | string (URL-encoded file path) | Destination file path for upload. URL-decoded during streaming. Parent directories are auto-created if they don't exist. |
  | `binary` | body-form | yes | string ('0' \| '1') | Encoding mode: '0' for URL-encoded text content; '1' for URL-encoded base64-encoded binary content. Single digit expected. |
  | `content` | body-form | yes | string (URL-encoded or base64-encoded data) | File content. If binary=0, URL-decoded to bytes. If binary=1, URL-decoded to base64 characters, then base64-decoded to bytes. Size checked against available storage space; requests exceeding 90% of free space are rejected. |

- **Request body:** application/x-www-form-urlencoded; path=<urlencoded path>&binary=<0|1>&content=<urlencoded or base64-urlencoded data>. Streamed in 4KB chunks; total Content-Length is validated against available storage.
- **Response:** application/json; returns {"success":true} on success; {"success":false,"error":"<message>"} on failure (e.g., 'Filesystem not initialized', 'SD card not available', 'File too large (est X MB). Free: Y MB', 'Invalid path', 'Insufficient storage space', 'Write failed (short write)', 'Memory allocation failed', 'Recv error').
- **Observations:** Streaming parser with stateful URL-decoding and base64-decoding to avoid loading entire file into memory. PSRAM-allocated 4KB recv/output buffers for throughput. Storage space check: estimated file size = (Content-Length * 3) / 4 (base64 + URL-encoding overhead); rejects if estimated size > 90% of free space. Free space tracking during write prevents filling the filesystem. Short write (write() returns fewer bytes than requested) triggers automatic cleanup and error response. Parent directory auto-creation uses guarded VFS calls with user's identity. Post-save hooks: runFileWritePostSaveHooks() for automations.json (shared with /api/files/write), map organization for .hwmap files, and legacy waypoints reorganization. Sensor polling paused during upload to prevent I2C contention. No rate-limiting; can upload large files without throttling.


<a id="group-cli"></a>

### CLI & Logging

The CLI & Logging functional area provides web-based interfaces for interactive command execution and data/system logging configuration. The /cli page delivers a browser-based terminal emulator that polls /api/cli/logs every 500ms for real-time command output and executes single or batched commands via /api/cli and /api/cli/batch. The /logging page manages sensor data capture (CSV/track/text formats, configurable interval, file rotation) and system debug logging (per-category flag filtering) via CLI command wrappers, backed by the unified command execution layer. All endpoints enforce session-based authentication and rate-limit/broadcast command execution to prevent resource exhaustion and maintain UI coherence across concurrent sessions.

<a id="get-cli"></a>

#### `GET /cli`

**Handler:** `handleCLIPage` · **Source:** `components/hardwareone/WebServer_Server.cpp:3132` · **Kind:** page · **Auth:** session · **Maturity:** mature

Serves the CLI (Command Line Interface) web page with terminal emulator UI for executing commands interactively.

- **Auth detail:** WEB_AUTH_OR_RETURN macro checks isAuthed via session cookie; unauthenticated users see login page via tgRequireAuth.
- **Parameters:** _none_
- **Request body:** none
- **Response:** text/html - HTML page with embedded JavaScript for CLI UI, log polling, and command execution.
- **Observations:** The page calls /api/cli/logs (polled every 500ms), /api/cli (execute command), and /api/cli/batch (bonded device CLI support). Includes local storage for command history and ANSI code stripping. For bonded device CLI, delegates to window.BondFs.exec via shared bond session token.

<a id="post-api-cli"></a>

#### `POST /api/cli`

**Handler:** `handleCLICommand` · **Source:** `components/hardwareone/WebServer_Server.cpp:3004` · **Kind:** api · **Auth:** session · **Maturity:** mature

Execute a single CLI command synchronously; output is captured, optionally returned in response, and broadcast to SSE clients.

- **Auth detail:** Checks tgRequireAuth via makeWebAuthCtx; returns 401 JSON via sendAuthRequiredResponse if not authenticated.
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `cmd` | body-form | yes | string (URL-encoded) | The CLI command to execute (URL-decoded before processing). |
  | `validate` | body-form | no | string ('1'\|'true') | If set, validates command syntax only without executing; output not logged or broadcast. |
  | `capture` | body-form | no | string ('1') | If set to '1', returns raw command output in response; resets CLI state to CLI_NORMAL after response. |

- **Request body:** application/x-www-form-urlencoded with fields: cmd (required), validate (optional), capture (optional).
- **Response:** text/plain - Raw command output. HTTP status: 200 success, 400 bad request (unknown/empty command), 403 forbidden (admin required), 429 rate limited (>50ms between commands).
- **Observations:** Rate limits to 50ms minimum between commands. Redacts sensitive data (passwords, session IDs) before logging/broadcast. Handles graceful server shutdown during command (e.g. closewifi). Broadcasts to all sessions except the origin session (via gBroadcastSkipSessionIdx). validate=1 prevents logging and execution; capture=1 prevents interactive help mode persistence. HTTP status codes distinguish error types for API consumers.

<a id="post-api-cli-batch"></a>

#### `POST /api/cli/batch`

**Handler:** `handleCliBatch` · **Source:** `components/hardwareone/WebServer_Server.cpp:4703` · **Kind:** api · **Auth:** session · **Maturity:** mature

Execute multiple CLI commands in sequence and return all outputs in a single response (reduces round-trips for polling/batch operations).

- **Auth detail:** Checks tgRequireAuth via makeWebAuthCtx; returns 401 JSON via sendAuthRequiredResponse if not authenticated.
- **Parameters:** _none_
- **Request body:** application/json - {"commands": [string, string, ...]} - Array of command strings to execute sequentially.
- **Response:** application/json - {"ok": true, "count": N, "results": [output0, output1, ...]} - JSON array of text outputs, one per input command.
- **Observations:** Treats empty/whitespace-only commands as producing empty output string. Executes up to content_len 32768 bytes total; rejects oversized bodies. Broadcasts output for each command to all sessions except origin (via gBroadcastSkipSessionIdx). Suspends mesh activity during batch (gMeshActivitySuspended = true). Bails out if HTTP server is destroyed mid-batch (e.g. closewifi command). Notably, this endpoint is NOT treated as a user 'interaction' for idle-logout purposes (requestIsInteraction returns false for POST /api/cli/batch) so passive polling does not extend idle timeouts.

<a id="get-api-cli-logs"></a>

#### `GET /api/cli/logs`

**Handler:** `handleLogs` · **Source:** `components/hardwareone/WebServer_Server.cpp:2909` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Fetch current CLI output log buffer (command outputs, system messages, etc.) for display on CLI page.

- **Auth detail:** Checks tgRequireAuth via makeWebAuthCtx; returns 401 JSON via sendAuthRequiredResponse if not authenticated.
- **Parameters:** _none_
- **Request body:** none
- **Response:** text/plain - Raw log text snapshot from gWebMirror circular buffer (size: gWebMirrorCap, typically ~64KB or configured runtime size).
- **Observations:** High-frequency endpoint polled every 500ms by CLI page. Uses zero-copy snapshotTo() to avoid heap fragmentation. Allocates response buffer from PSRAM (preferPSRAM) to avoid main heap pressure. Initialization of gWebMirror is deferred until first request. Debug logging via [LOGS_DEBUG] tags. No pagination or filtering; returns entire current buffer state.

<a id="get-logging"></a>

#### `GET /logging`

**Handler:** `handleLoggingPage` · **Source:** `components/hardwareone/WebServer_Server.cpp:3174` · **Kind:** page · **Auth:** session · **Maturity:** mature

Serves the Logging page UI for managing and viewing both sensor data logging and system debug logging.

- **Auth detail:** WEB_AUTH_OR_RETURN macro checks isAuthed via session cookie; unauthenticated users see login page via tgRequireAuth.
- **Parameters:** _none_
- **Request body:** none
- **Response:** text/html - HTML page for sensor logging and system logging configuration, status display, and log file browser/viewer.
- **Observations:** The page calls /api/cli (single commands), /api/cli/batch (bulk status queries: 'time', 'sensorlog status', 'log status'), and /api/settings (to check user.isAdmin for log type toggle). CLI commands executed: sensorlog {start|stop|autostart|format|maxsize|rotations|sensors|status}, log {start|stop|autostart|status}, time. Page uses collapsible sections (Sensor Logging, System Logging, Log Viewer). File browser uses shared window.BondFs for bonded device logs (master only). Status parsing via regex to extract file, interval, format, max size, rotations, sensors, and last write time. Auto-start toggle via CLI command without explicit response validation. Configuration application loops through 4 sequential sensorlog commands. System logging has separate status/config sections for file path, category tags, and debug message flag checkboxes (0x-prefixed bitmask values).


<a id="group-automations"></a>

### Automations

The Automations functional area provides a web interface for managing scheduled task automations. The system supports multiple trigger types (at-time, after-delay, interval, on-boot) with sensor-based conditions and conditional logic. Users can create, edit, delete, enable/disable automations, and import/export them as JSON. All endpoints require authentication (session cookie) and return JSON responses for API endpoints. The HTML page endpoint calls the /api/automations and /api/automations/export endpoints via JavaScript, along with the /api/cli endpoint for command execution (automation add, delete, enable, disable, run, trigger).

<a id="get-automations"></a>

#### `GET /automations`

**Handler:** `handleAutomationsPage` · **Source:** `components/hardwareone/WebServer_Server.cpp:3142` · **Kind:** page · **Auth:** session · **Maturity:** adequate

Serves the automations management page HTML, allowing users to create, edit, delete, and manage scheduled automations through a web UI.

- **Auth detail:** Requires WEB_AUTH_OR_RETURN macro which checks session cookie and redirects unauthenticated users to /login
- **Parameters:** _none_
- **Request body:** none
- **Response:** text/html; full HTML page with embedded JavaScript. Includes form for creating automations, import/export sections, and a table to display existing automations. JavaScript calls /api/automations and /api/cli endpoints.
- **Observations:** Page is server-side rendered and embeds large JavaScript code blocks (WebPage_Automations.h spans ~1700 lines). The page JavaScript makes AJAX calls to /api/automations to fetch automation list, /api/cli for creating/modifying automations via CLI commands, /api/automations/export for downloading backups, and hw.fetchJSON/hw.fetchText for API calls. Secondary triggers use v1 schema normalization within JS. Import/export functionality is tightly coupled to the CLI command system.

<a id="get-api-automations"></a>

#### `GET /api/automations`

**Handler:** `handleAutomationsGet` · **Source:** `components/hardwareone/WebServer_Server.cpp:3150` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Returns the complete list of configured automations in JSON format for the web UI to display and manage.

- **Auth detail:** Requires WEB_AUTH_OR_RETURN macro which checks session cookie and returns ESP_OK (no response body) if auth fails
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json; raw automations.json file content. Expected structure: {"automations": [{id, name, enabled, type, trigger, commands, condition, triggerMode, bootDelayMs, runAtBoot, ...}]}. Response includes sanitization side-effect: if duplicate IDs are detected, they are removed and persisted back to automations.json (best-effort).
- **Observations:** Handler reads from AUTOMATIONS_JSON_FILE, performs in-situ sanitization of duplicate IDs as a side-effect, and sends the full file content. Error responses use {"success":false,"error":"..."}. No pagination, filtering, or per-automation fetch support. The sanitization side-effect (writeAutomationsJsonAtomic) is implicit and could cause confusion if the file is modified during fetch.

<a id="get-api-automations-export"></a>

#### `GET /api/automations/export`

**Handler:** `handleAutomationsExport` · **Source:** `components/hardwareone/WebServer_Server.cpp:4532` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Exports automations as JSON files for backup or sharing. Supports single-automation export by ID or bulk export of all automations with timestamped filenames.

- **Auth detail:** Requires WEB_AUTH_OR_RETURN macro which checks session cookie
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `id` | query | no | integer | Automation ID to export as a single file. If omitted, exports all automations as a bulk backup. |

- **Request body:** none
- **Response:** application/json with Content-Disposition: attachment header. If id param is present: single automation JSON object (e.g., {id, name, trigger, commands, ...}); filename derived from automation name with sanitized spaces/slashes. If id param omitted: full automations.json array; filename is automations-backup-YYYY-MM-DD_HH-MM-SS.json.
- **Observations:** Uses string parsing (extractArrayByKey, extractArrayItem, parseJsonInt/String) rather than full JSON deserialization for single-automation lookup. File naming sanitizes automation names by replacing spaces/slashes with underscores. No validation of automation ID existence in bulk-export path—simply returns the full JSON even if no IDs match. Timestamp uses localtime() with strftime, which requires system time to be set.


<a id="group-sensors"></a>

### Sensors, Camera & Media

The Sensors, Camera & Media functional area provides comprehensive HTTP endpoints for real-time sensor data retrieval, camera live streaming and image capture, microphone audio recording playback, and remote ESP-NOW peer sensor aggregation. The `/sensors` page displays an interactive UI for all connected sensors (thermal, ToF, IMU, GPS, RTC, presence detection, FM radio, gamepad input, ANO encoder, APDS color/proximity/gesture, camera, and microphone). Multiplexed JSON API endpoints enable frontend polling of individual sensor readings at configurable cadences. The camera subsystem supports still-frame JPEG capture and continuous MJPEG streaming with FPS control and single-session locking. Microphone and video recording endpoints support playback/download with path validation and memory-efficient chunked streaming. All endpoints are session-authenticated except where compilation disabled (fallback stubs return appropriate 200/disabled-flag responses)."

<a id="get-sensors"></a>

#### `GET /sensors`

**Handler:** `handleSensorsPage` · **Source:** `components/hardwareone/WebPage_Sensors.cpp:70` · **Kind:** page · **Auth:** session · **Maturity:** adequate

Serves the main sensors web page with interactive UI for displaying and monitoring connected sensors (thermal, ToF, IMU, gamepad, GPS, RTC, presence, FM radio, camera, microphone, etc.). The page polls sensor data via API endpoints and displays real-time readings.

- **Auth detail:** WEB_AUTH_OR_RETURN macro redirects unauthenticated users to /login via sendAuthRequiredResponse
- **Parameters:** _none_
- **Request body:** none
- **Response:** text/html; full HTML page with sensors UI (stream-based chunked response). Includes CSS for sensor cards, navigation, and JavaScript that calls /api/sensors/status and /api/sensors?sensor=<type> endpoints for data polling
- **Observations:** HTML page that streams content chunked via streamPageWithContent() helper. Frontend JS calls /api/sensors/status for status flags, /api/sensors?sensor=thermal|tof|imu|input|gps|presence|rtc|fmradio|camera|microphone for individual sensor data, /api/sensors/camera/status for camera state, /api/recordings for mic recordings list, /api/videos for video list. Page is conditional on ENABLE_WEB_SENSORS compile flag; disabled sensors show placeholder cards with 'not compiled' message. No pagination or filtering — displays all sensors at once.

<a id="get-api-sensors"></a>

#### `GET /api/sensors`

**Handler:** `handleSensorData` · **Source:** `components/hardwareone/WebPage_Sensors.cpp:79` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Multiplexed JSON endpoint that returns real-time data for a specific sensor type. Used by frontend sensor cards to poll individual sensor readings on a cadence (e.g., every 100-500ms). Each sensor type has its own JSON schema and data structure.

- **Auth detail:** WEB_AUTH_OR_RETURN macro enforces authentication; unauthenticated requests get 401/JSON via sendAuthRequiredResponse
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `sensor` | query | yes | string | Sensor type to fetch data for. Supported values: 'thermal', 'tof', 'imu', 'input' (gamepad), 'anoencoder', 'fmradio', 'camera', 'microphone', 'presence', 'gps', 'rtc'. Case-sensitive. |

- **Request body:** none
- **Response:** application/json. Response varies by sensor type: thermal: {schema:1, ok:bool, seq:int, mn:float, mx:float, w:int, h:int, data:[int...]}; tof: sensor-specific JSON with object detection data; imu: {schema:1, ok:bool, accelX:float, accelY:float, accelZ:float, gyroX:float, gyroY:float, gyroZ:float, ...}; input (gamepad): {val:1, x:int, y:int, buttons:uint32} or {val:0, error:string}; anoencoder: {val:1, pos:int, axis:int, buttons:uint32} or {val:0, error:string}; fmradio: {schema:1, ok:bool, frequency:float, rssi:int, ...}; camera/microphone: {enabled:bool, ...} from status builders; presence: {ambientTemp:float, presenceValue:int, motionValue:int, presenceDetected:bool, motionDetected:bool}; gps: {fix:bool, quality:int, satellites:int, latitude:float, longitude:float, altitude:float, speed:float, angle:float, time:string, date:string}; rtc: {year:int, month:int, day:int, dayOfWeek:string, hour:int, minute:int, second:int, temperature:float}. All types return {error:string} if not compiled, not connected, or no data available.
- **Observations:** Intentionally omits CORS headers (see lines 82-90); W3C CORS spec forbids Access-Control-Allow-Origin:* on credentialed responses. Matches pattern used by /api/sensors/status, /api/devices, /api/cli which also omit CORS. Heavy use of compile-time conditionals (ENABLE_THERMAL_SENSOR, ENABLE_TOF_SENSOR, etc.). Uses stack-allocated buffers (ToF, IMU, gamepad, FM radio) and shared PSRAM buffer (thermal) with mutex for efficiency. GPS and RTC queries perform time calculations client-side (UTC→local via gSettings.tzOffsetMinutes). No request validation on sensor parameter — invalid sensor type returns generic error. Thermal sensor data falls back to ArduinoJson path if shared buffer unavailable. No rate limiting visible.

<a id="get-api-sensors-status"></a>

#### `GET /api/sensors/status`

**Handler:** `handleSensorsStatusWithUpdates` · **Source:** `components/hardwareone/WebServer_Server.cpp:2943` · **Kind:** api · **Auth:** session · **Maturity:** mature

Returns current sensor subsystem status: which sensors are enabled, which are compiled, SD card availability, camera/microphone recording state, Edge Impulse model state. Used by dashboard to poll sensor configuration changes and determine which sensor cards to display. Optionally includes needsRefresh flag if the session was flagged for a UI refresh notification.

- **Auth detail:** WEB_AUTH_OR_RETURN macro enforces authentication; falls back to handleSensorsStatusStub (WebServer_Server.cpp:2330) when ENABLE_WEB_SENSORS is not compiled, which returns {sensorsDisabled:true} without auth
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json. {seq:int, thermalEnabled:bool, tofEnabled:bool, imuEnabled:bool, apdsColorEnabled:bool, apdsProximityEnabled:bool, apdsGestureEnabled:bool, inputEnabled:bool, pwmDriverConnected:bool, gpsEnabled:bool, fmRadioEnabled:bool, rtcEnabled:bool, presenceEnabled:bool, thermalCompiled:bool, tofCompiled:bool, imuCompiled:bool, inputCompiled:bool, apdsCompiled:bool, gpsCompiled:bool, rtcCompiled:bool, presenceCompiled:bool, fmradioCompiled:bool, servoCompiled:bool, cameraEnabled:bool, cameraStreaming:bool, cameraRecording:bool, cameraCompiled:bool, sdAvailable:bool, sdWritable:bool, micEnabled:bool, micRecording:bool, micCompiled:bool, eiEnabled:bool, eiModelLoaded:bool, eiCompiled:bool, needsRefresh:bool (optional)}
- **Observations:** Shared handler with handleSensorsStatusStub fallback. Uses buildSensorStatusJson() to generate base JSON from global state variables (gThermalEnabled, gTofEnabled, etc.). If session has needsRefresh flag set, appends needsRefresh:true to base JSON using chunked transfer to avoid stack buffer overflow. Clears needsRefresh flag after sending so next request omits it. Conditionally includes compile-time features (servo, RTC, presence, FM radio, camera, microphone, Edge Impulse). sdAvailable vs sdWritable distinction allows UI to gate 'requires SD write' features separately from 'can list files' features. seq field allows polling for changes (increment on sensor connect/disconnect events).

<a id="get-api-sensors-remote"></a>

#### `GET /api/sensors/remote`

**Handler:** `handleRemoteSensors` · **Source:** `components/hardwareone/WebPage_Sensors.cpp:577` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Lists remote ESP-NOW peer sensors and fetches data from them. Acts as a gateway to sensor data on wireless peers (other ESP32 boards communicating via ESP-NOW protocol). Allows central hub to aggregate sensor readings from multiple remote devices.

- **Auth detail:** WEB_AUTH_OR_RETURN macro enforces authentication
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `device` | query | no | string | MAC address of remote ESP-NOW device (URL-encoded, e.g., 'E8%3A9F%3A...'). When paired with 'sensor' param, returns specific sensor data from that device. Omit to get list of all remote devices. |
  | `sensor` | query | no | string | Sensor type to fetch from the remote device (e.g., 'temperature', 'humidity'). Only meaningful when paired with 'device' param. |

- **Request body:** none
- **Response:** application/json. When device+sensor specified: {device_specific_sensor_data}. When neither specified: {enabled:bool, devices:[{mac:string, name:string, sensors:[...]}...]} representing all remote ESP-NOW devices and their available sensors. If ENABLE_ESPNOW is not compiled, returns {enabled:false, devices:[]}.
- **Observations:** Conditional on ENABLE_ESPNOW compile flag. Reports runtime ESP-NOW state (isEspNowInitialized()) not just compile flag, so UI can distinguish 'feature not compiled' from 'compiled but not initialized yet'. Performs URL-decoding of MAC address since browser sends percent-encoded MAC (E8%3A9F:... format). Calls getRemoteSensorsListJSON() and getRemoteSensorDataJSON() helper functions. Debug logging includes 120-char truncation of JSON snippets. No pagination on device list — returns all peers at once.

<a id="get-api-sensors-camera-status"></a>

#### `GET /api/sensors/camera/status`

**Handler:** `handleCameraStatus` · **Source:** `components/hardwareone/WebPage_Sensors.cpp:644` · **Kind:** api · **Auth:** session · **Maturity:** barebones

Returns camera subsystem status: enabled flag, streaming state, resolution, and PSRAM availability. Used by frontend to show camera availability and current stream state in the sensor status panel.

- **Auth detail:** WEB_AUTH_OR_RETURN macro enforces authentication
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json. {enabled:bool, connected:bool, streaming:bool, model:string, width:int, height:int, psram:bool} from buildCameraStatusJson(), or {enabled:false, compiled:false} if camera not compiled
- **Observations:** Simple status passthrough; no camera control. Delegates to buildCameraStatusJson() from System_Camera_DVP.cpp. Returns 501 Not Implemented if ENABLE_CAMERA_SENSOR is not compiled.

<a id="get-api-sensors-camera-frame"></a>

#### `GET /api/sensors/camera/frame`

**Handler:** `handleCameraFrame` · **Source:** `components/hardwareone/WebPage_Sensors.cpp:658` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Returns a single JPEG frame from the camera for thumbnail/preview display. Used by frontend to show a still image snapshot without starting a full stream.

- **Auth detail:** AuthContext created and tgRequireAuth() called explicitly (not macro); unauthenticated requests get 401 via sendAuthRequiredResponse
- **Parameters:** _none_
- **Request body:** none
- **Response:** image/jpeg binary blob (JPEG frame data) with Content-Disposition inline. On error (camera disabled, capture failed, not compiled): text/plain error message with status 503, 500, or 501
- **Observations:** Calls captureFrame(size_t* outLen) to get current frame data. Frame is freed after send. Returns 503 if camera enabled=false, 500 if capture failed, 501 if not compiled. No streaming — one-shot request per frame. Explicit authentication check at line 662.

<a id="get-api-sensors-camera-stream"></a>

#### `GET /api/sensors/camera/stream`

**Handler:** `handleCameraStream` · **Source:** `components/hardwareone/WebPage_Sensors.cpp:704` · **Kind:** stream · **Auth:** session · **Maturity:** adequate

Streams live MJPEG video feed from camera for real-time preview. Used by video player on frontend to display continuous video in <img> or <video> tag.

- **Auth detail:** WEB_AUTH_OR_RETURN macro enforces authentication
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `fps-implicit-from-settings` | header | no | int | FPS cap is read from gSettings.cameraStreamFps (1-20, default 5), not passed as query param but configured per session settings. |

- **Request body:** none
- **Response:** multipart/x-mixed-replace; boundary=frame. Each part is a JPEG frame with Content-Length header. Stream continues indefinitely until client disconnects or camera is disabled. Sets Cache-Control: no-cache, must-revalidate and Access-Control-Allow-Origin:*. Tail markers (\r\n) between frames.
- **Observations:** Single-stream lock enforces only one active MJPEG client at a time (keyed by session SID or IP, with 5s timeout for stale owners). Takeover semantics: new request bumps s_streamGen so old loop exits. Heartbeat (s_streamLastBeat) helps detect disconnected clients. Sets cameraStreaming=true while active, clears on exit. FPS configurable via gSettings.cameraStreamFps (1-20), converted to delay with 50ms min, 2000ms max. Frame capture runs in conditional loop, retries with 100ms delay if captureFrame fails. Chunked response (httpd_resp_send_chunk) with boundary markers per MJPEG spec. Edge Impulse integration: if continuous inference running, enforces additional minimum interval between frames (half of edgeImpulseIntervalMs, minimum 200ms). Explicit authentication check at line 705.

<a id="get-api-recordings"></a>

#### `GET /api/recordings`

**Handler:** `handleMicRecordingsList` · **Source:** `components/hardwareone/WebPage_Sensors.cpp:850` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Lists all microphone recordings available on the device (WAV files in /sd/recordings or /recordings). Used by frontend to populate recording list in playback UI.

- **Auth detail:** WEB_AUTH_OR_RETURN macro enforces authentication
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json. {count:int, files:[{name:string, size:int}, ...]} or {count:0, files:[], error:"not_compiled"} if ENABLE_MICROPHONE_SENSOR not set
- **Observations:** Conditional on ENABLE_MICROPHONE_SENSOR. Wraps a single getRecordingsList() call in ExecIdentityGuard(ctx) to install authenticated web user's identity for the duration, allowing read access to /sd/recordings with user's permissions (not system bypass). Parses 'name:size,name:size' format from getRecordingsList(), deriving `count` from that same parse. No pagination — returns all recordings at once. **Perf note (2026-08-25):** this previously also called a separate getRecordingCount(), doubling the directory walk; measured 1218 ms for 46 recordings, now 714 ms. That function has been deleted — do not reintroduce a count-only pass. Enumerating a directory fopen()s every file in it (~11 ms/entry), and httpd is single-task, so this blocks every other request for its duration. The browser-side poll is now gated on the recordings panel being open (System_Microphone_Web.h).

<a id="get-api-recordings-file"></a>

#### `GET /api/recordings/file`

**Handler:** `handleMicRecordingFile` · **Source:** `components/hardwareone/WebPage_Sensors.cpp:904` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Serves a single microphone recording file for playback in browser audio player. Used by frontend to fetch WAV file for on-page playback.

- **Auth detail:** WEB_AUTH_OR_RETURN macro enforces authentication
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `name` | query | yes | string | Filename of the recording to serve (e.g., 'rec_001.wav'). No path separators or '..' allowed (validated). |

- **Request body:** none
- **Response:** audio/wav binary blob (WAV file data) with Content-Length and Content-Disposition headers. On error: text/plain error with status 400 (bad filename), 404 (not found), 500 (allocation/read failed), or 501 (not compiled)
- **Observations:** Path validation prevents directory traversal (rejects '/' and '..'). Checks both /sd/recordings (SD card) and /recordings (LittleFS) paths. Allocates PSRAM buffer (8MB available) for file content (max recording ~60s @ 16kHz = ~1.9MB). Falls back to regular malloc for smaller files. Slurps entire file into memory before sending (inefficient for large files, but recordings capped at 60s). Content-Length header required for browser audio seeking. Conditional on ENABLE_MICROPHONE_SENSOR.

<a id="get-api-videos"></a>

#### `GET /api/videos`

**Handler:** `handleVideoRecordingsList` · **Source:** `components/hardwareone/System_Camera_Video.cpp:504` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Lists all video recordings (AVI files) stored on SD card. Used by frontend to show available video files for playback/download.

- **Auth detail:** WEB_AUTH_OR_RETURN macro enforces authentication
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json. {count:int, sdAvailable:bool, files:[{name:string, size:int}, ...]}
- **Observations:** Not gated on ENABLE_CAMERA_SENSOR — lists existing videos independent of whether camera recording is currently enabled. Degrades gracefully: empty list if SD card unavailable. Parses 'name:size' newline-separated format from getVideoRecordingsList(). No pagination — returns all videos at once. sdAvailable flag used by frontend to gate download features.

<a id="get-api-videos-file"></a>

#### `GET /api/videos/file`

**Handler:** `handleVideoRecordingFile` · **Source:** `components/hardwareone/System_Camera_Video.cpp:544` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Serves a single video recording file for playback or download. Used by frontend to stream large AVI files to browser video player or trigger download.

- **Auth detail:** WEB_AUTH_OR_RETURN macro enforces authentication
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `name` | query | yes | string | Filename of the video to serve (e.g., 'video_20250101_120000.avi'). No path separators or '..' allowed (validated). |

- **Request body:** none
- **Response:** video/x-msvideo binary stream (AVI file data) chunked (no Content-Length due to chunked encoding). Content-Disposition: attachment. On error: text/plain with status 503 (SD unavailable), 400 (bad filename), 404 (not found), 500 (allocation failed), or 501 (not compiled)
- **Observations:** Path validation prevents directory traversal (rejects '/' and '..'). Checks SD card availability first (returns 503 if unavailable). Uses chunked transfer encoding (httpd_resp_send_chunk) with 8KB chunks instead of slurping whole file (AVI files can be very large, exceeding PSRAM if buffered). Intentionally omits Content-Length header because chunked encoding is incompatible with declared total length; browsers handle chunked downloads correctly. File opened via VFS::openGuarded with system auth scope. Not conditional on ENABLE_CAMERA_SENSOR (degrades gracefully if no videos exist).


<a id="group-llm"></a>

### LLM (on-device language model)

The LLM functional area provides HTTP endpoints for on-device language model management and chat interactions. It backs the GET /llm web page which displays a chat UI. The endpoints include model listing and loading, text generation with async streaming polls, chat history retrieval, retry/clear operations, and status monitoring. All endpoints require user authentication via WEB_AUTH_OR_RETURN macro (session-based; redirects unauthenticated requests to /login). No endpoints in this group require admin privileges.

<a id="get-llm"></a>

#### `GET /llm`

**Handler:** `handleLLMPage` · **Source:** `components/hardwareone/WebPage_LLM.cpp:402` · **Kind:** page · **Auth:** session · **Maturity:** adequate

Serves the LLM chat web interface. Displays model status, available models selector, conversation history, input textarea, and advanced parameter controls (temperature, sentence limit, repetition penalty).

- **Auth detail:** WEB_AUTH_OR_RETURN(req, ctx) at line 403 — redirects unauthenticated users to /login; tgRequireAuth calls sendAuthRequiredResponse() which sends 302 redirect.
- **Parameters:** _none_
- **Request body:** none
- **Response:** text/html; full HTML page with embedded CSS and JavaScript. The page calls /api/llm/status, /api/llm/models, /api/llm/load, /api/llm/unload, /api/llm/generate, /api/llm/stop, /api/llm/result, and /api/cli endpoints to manage LLM state and execute generation.
- **Observations:** HTML/JavaScript page design: the page HTML is streamed inline via streamBeginHtml/streamEndHtml helpers. The JS code immediately calls /api/llm/status and /api/llm/models on load to populate the UI. Chat UI uses async polling pattern via /api/llm/result for streamed responses. Retry button and 'Do:' command mode are rendered client-side. No pagination or filtering visible in the page logic.

<a id="get-api-llm-status"></a>

#### `GET /api/llm/status`

**Handler:** `handleLLMStatus` · **Source:** `components/hardwareone/WebPage_LLM.cpp:59` · **Kind:** api · **Auth:** session · **Maturity:** mature

Returns current LLM state: whether a model is loaded, generation in progress, or error. Includes model architecture and performance metrics (tokens/sec, PSRAM usage).

- **Auth detail:** WEB_AUTH_OR_RETURN(req, ctx) at line 60 — returns 302 redirect if unauthenticated.
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json; single JSON object with keys: state (string: UNLOADED|LOADING|READY|GENERATING|ERROR), model (string: model file path), params (string: dim x n_layers x n_heads), psramKB (number: PSRAM used in KB), tokPerSec (float: tokens per second from last generation), lastTokens (number: token count from last generation), error (string: error message if state==ERROR), dim (number: model dimension), layers (number: layer count), heads (number: attention heads), kvHeads (number: KV cache heads), vocab (number: vocabulary size), seqLen (number: sequence length), ctxUsed (number: context used from last turn), ctxMax (number: max context for this session), arch (string: GPT-2 or Llama), quant (string: INT8 or FP32).
- **Observations:** Uses fixed-size stack buffer (448 bytes) for JSON serialization. Uses snprintf to assemble response — not streaming. The state enum is hand-translated to strings; arch and quant are hardcoded binary choices (1→GPT-2, 1→INT8). No validation of returned values against actual state; trusts llmGetStatus() output directly. Called on every page load and after each generation to update UI.

<a id="get-api-llm-models"></a>

#### `GET /api/llm/models`

**Handler:** `handleLLMModels` · **Source:** `components/hardwareone/WebPage_LLM.cpp:96` · **Kind:** api · **Auth:** session · **Maturity:** adequate

List all available LLM .bin model files accessible to the device.

- **Auth detail:** WEB_AUTH_OR_RETURN(req, ctx) at line 97 — returns 302 redirect if unauthenticated.
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json; array of model objects returned by llmListModels(). Based on page JavaScript (line 319), each object has keys: path (string: full model path for use in /api/llm/load), name (string: display name), size (number: bytes), storage (string: 'sd' for SD card or empty/null for internal).
- **Observations:** Calls llmListModels() which returns a String (likely JSON array). The handler does not validate or transform the response — it passes it through directly. Page JS expects {path, name, size, storage} shape per element but handler makes no schema guarantees. No pagination or filtering. Page code extracts storage type and prepends '[SD]' to display name for SD-sourced models (line 321).

<a id="post-api-llm-load"></a>

#### `POST /api/llm/load`

**Handler:** `handleLLMLoad` · **Source:** `components/hardwareone/WebPage_LLM.cpp:108` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Load a LLM model from storage into memory. Validates the model path for security, resolves bare filenames, and initiates model loading with optional context size override.

- **Auth detail:** WEB_AUTH_OR_RETURN(req, ctx) at line 109 — returns 302 redirect if unauthenticated.
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `model` | body-json | yes | string | Model filename or full path (e.g., 'model.bin', '/system/llm/model.bin', '/sd/llm/model.bin'). If bare filename, defaults to /system/llm/. If full path, must start with /system/llm/ or /sd/llm/. |
  | `max_ctx` | body-json | no | number | Maximum context length in tokens (0-2048). If omitted or out of range, defaults to gSettings.llmMaxContext. Clamped to [0, 2048]. |

- **Request body:** application/json; e.g., {"model": "model.bin", "max_ctx": 64}. Body size limit: 256 bytes.
- **Response:** application/json; success response: {"ok": true}. Failure response: {"ok": false, "error": "<message>"}. Error messages include: 'Bad request' (body read failed), 'Invalid JSON' (JSON parse error), 'No model specified' (missing model field), 'Invalid model path' (path not under allowed directories), or the LLMStatus.errorMsg if load fails (e.g., 'File not found', 'PSRAM exhausted').
- **Observations:** Path validation is strict: bare filenames map to /system/llm/; absolute paths must start with /system/llm/ or /sd/llm/ or request is rejected. max_ctx clamping happens before llmLoadModel() call (lines 128-129). Uses readPostBody() helper with stack buffer (256 bytes) — if body >= 256 bytes, load fails silently. PSRAM_JSON_DOC macro used for ArduinoJson document. On error, returns LLMStatus.errorMsg; no structured error codes. Page JS calls this with model path from dropdown and max_ctx=64 hardcoded (line 618).

<a id="post-api-llm-unload"></a>

#### `POST /api/llm/unload`

**Handler:** `handleLLMUnload` · **Source:** `components/hardwareone/WebPage_LLM.cpp:165` · **Kind:** api · **Auth:** session · **Maturity:** mature

Unload the currently loaded LLM model and free its PSRAM.

- **Auth detail:** WEB_AUTH_OR_RETURN(req, ctx) at line 166 — returns 302 redirect if unauthenticated.
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json; {"ok": true}. No error cases documented.
- **Observations:** Simple no-op-safe handler — always returns {ok:true} regardless of state. Calls llmUnload() which handles the case of no model being loaded. Page JS shows system message 'Model unloaded' after calling this (line 633).

<a id="post-api-llm-generate"></a>

#### `POST /api/llm/generate`

**Handler:** `handleLLMGenerate` · **Source:** `components/hardwareone/WebPage_LLM.cpp:189` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Start async text generation in the background. Returns a session ID immediately; client polls GET /api/llm/result?session=<id> to retrieve streamed output.

- **Auth detail:** WEB_AUTH_OR_RETURN(req, ctx) at line 190 — returns 302 redirect if unauthenticated.
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `prompt` | body-json | yes | string | The text prompt to generate from. Cannot be empty. |
  | `max_tokens` | body-json | no | number | Maximum tokens to generate. Falls back to gSettings.llmMaxTokens if omitted. |
  | `temperature` | body-json | no | number | Sampling temperature (lower = more deterministic). Falls back to gSettings.llmTemperature if omitted. |
  | `top_p` | body-json | no | number | Nucleus sampling parameter. Falls back to gSettings if omitted. |
  | `mirostat2` | body-json | no | boolean | Enable Mirostat v2 sampling. |
  | `mirostat_tau` | body-json | no | number | Mirostat target entropy. |
  | `mirostat_eta` | body-json | no | number | Mirostat learning rate. |
  | `rep_penalty` | body-json | no | number | Repetition penalty. Falls back to gSettings if omitted. |
  | `rep_window` | body-json | no | number | Repetition penalty window size. |
  | `sentence_limit` | body-json | no | number | Stop after N sentences (0=off). |
  | `hard_cap` | body-json | no | number | Hard token limit (overrides max_tokens for emergency stop). |
  | `dyn_temp` | body-json | no | boolean | Enable dynamic temperature. |
  | `suppress` | body-json | no | array | Legacy field (ignored since chat-module migration). Previously array of prior answers to suppress; now POST /api/llm/chat/retry handles retry semantics server-side. |

- **Request body:** application/json; e.g., {"prompt": "Q: What is...\nA:", "temperature": 0.5, "top_p": 0.8, "sentence_limit": 2, "rep_penalty": 1.3, "rep_window": 32}. Body size limit: 2048 bytes.
- **Response:** application/json; success response: {"ok": true, "session": <number>}. Failure response: {"ok": false, "error": "<message>"}. Error messages: 'model not ready', 'bad request' (body read failed), 'invalid JSON' (JSON parse error), 'empty prompt' (missing or empty prompt field), 'busy or failed to start' (generation queue full or engine error).
- **Observations:** Implements async generation pattern — endpoint returns immediately with session ID; actual generation runs in background. Uses readPostBody() with stack buffer (2048 bytes). Parameter override detection uses is<T>() checks (lines 218-228) — omitted fields treated as sentinel (INT32_MIN, NaN) and resolved by chatResolveParams(). Legacy 'suppress' field is parsed but ignored (line 236 comment explains chat-module migration). Page JS builds body with temperature, top_p, sentence_limit, rep_penalty, rep_window (lines 348-355); hardcap and sentence_limit=0 set for 'Do:' mode (line 356). Comments note sentiment that client should use POST /api/llm/chat/retry for new code (line 232). Per-request overrides merge with gSettings defaults in chatBeginTurn().

<a id="post-api-llm-stop"></a>

#### `POST /api/llm/stop`

**Handler:** `handleLLMStop` · **Source:** `components/hardwareone/WebPage_LLM.cpp:176` · **Kind:** api · **Auth:** session · **Maturity:** mature

Abort in-progress text generation.

- **Auth detail:** WEB_AUTH_OR_RETURN(req, ctx) at line 177 — returns 302 redirect if unauthenticated.
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json; {"ok": true}. No error cases documented.
- **Observations:** Simple handler — calls llmStop() then returns ok:true. Page JS calls this when user clicks the Stop button (line 606). No structured feedback on whether generation was actually in progress; always succeeds.

<a id="get-api-llm-result"></a>

#### `GET /api/llm/result`

**Handler:** `handleLLMPoll` · **Source:** `components/hardwareone/WebPage_LLM.cpp:257` · **Kind:** api · **Auth:** session · **Maturity:** mature

Poll for streamed text generation output. Clients repeatedly call this with the session ID and incrementing offset to consume the generated response incrementally.

- **Auth detail:** WEB_AUTH_OR_RETURN(req, ctx) at line 258 — returns 302 redirect if unauthenticated.
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `session` | query | no | number | Session ID from POST /api/llm/generate. Queries to mismatch session return {done:true, stale:true} to signal client to stop polling. Omitting or passing 0 is treated as no session. |
  | `offset` | query | no | number | Byte offset into the stream buffer. Returns only new text since this offset. Omitting defaults to 0. |

- **Request body:** none
- **Response:** application/json; {"done": boolean, "len": number, "text": string, "stale": boolean (optional, only present if true)}. 'done' is true when generation completed and no more data follows. 'len' is the total byte length of the stream buffer. 'text' is the chunk of generated text since byte 'offset' (empty if no new bytes). 'stale' is present and true only when the queried session doesn't match the current generation or generation has ended.
- **Observations:** Implements chunked polling architecture. Each call reads up to 512 bytes from the chat module's stream buffer (line 287-288). Done semantics: generator marks done=true when it finishes; this endpoint then returns done:true when both !chatIsGenerating() and offset has caught up (lines 290-293). Stale detection: if session ID doesn't match current chatGetSessionId() or generation already finished, returns {text:"", done:true, stale:true} (line 282) to signal client to abandon polling. Uses PSRAM_JSON_DOC for response serialization. Page JS polls with 150ms interval (line 391), increments offset by j.text.length each call (line 401).

<a id="get-api-llm-chat-turns"></a>

#### `GET /api/llm/chat/turns`

**Handler:** `handleLLMChatTurns` · **Source:** `components/hardwareone/WebPage_LLM.cpp:310` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Retrieve full conversation history. Called by page on load to restore prior turns that now live in firmware (not browser JS).

- **Auth detail:** WEB_AUTH_OR_RETURN(req, ctx) at line 311 — returns 302 redirect if unauthenticated.
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json; streaming response — chunked JSON array of turn objects. Each turn object: {"role": "user" | "assistant", "text": string (up to LLM_CHAT_TURN_MAX_BYTES), "tokens": number, "tokPerSec": float, "streaming": boolean}. Chunking: array start '[' and end ']' sent separately; entries comma-separated. Per-turn JSON is serialized via ArduinoJson PSRAM_JSON_DOC to avoid allocating large contiguous buffers.
- **Observations:** Streaming architecture to avoid large allocations — sends array framing '[' and ']' separately, then streams each turn as individual JSON string + comma (lines 319-343). Iterates chatGetTurnCount() turns via chatReadTurn() and chatGetTurnInfo(). Each turn text is capped at LLM_CHAT_TURN_MAX_BYTES (defined in System_LLMChat.h, not visible in this file). Per-turn serialization uses PSRAM_JSON_DOC; data comes from chat module's turn storage. Role mapped to string (USER→'user', else→'assistant', line 330). tokPerSec is divided by 10 during storage (line 333: tokensPerSecX10 / 10.0f) to preserve single-digit precision. No pagination — returns ALL turns up to turn count limit.

<a id="post-api-llm-chat-retry"></a>

#### `POST /api/llm/chat/retry`

**Handler:** `handleLLMChatRetry` · **Source:** `components/hardwareone/WebPage_LLM.cpp:352` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Regenerate the last assistant response with optional per-call parameter overrides. Replaces legacy 'suppress' array from POST /api/llm/generate.

- **Auth detail:** WEB_AUTH_OR_RETURN(req, ctx) at line 353 — returns 302 redirect if unauthenticated.
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `max_tokens` | body-json | no | number | Override max tokens for this retry. Falls back to gSettings if omitted. |
  | `temperature` | body-json | no | number | Override temperature for this retry. Falls back to gSettings if omitted. |
  | `top_p` | body-json | no | number | Override top_p for this retry. Falls back to gSettings if omitted. |
  | `hard_cap` | body-json | no | number | Override hard token cap for this retry. Falls back to gSettings if omitted. |

- **Request body:** application/json; optional, e.g., {"temperature": 0.7, "max_tokens": 256}. Empty body is valid (line 358). Body size limit: 2048 bytes.
- **Response:** application/json; success response: {"ok": true, "session": <number>}. Failure response: {"ok": false, "error": "no prior turn or busy"}.
- **Observations:** Firmware-owned retry semantics — server suppresses prior answer internally via chatRetryLast() instead of client maintaining suppress list. Optional body parsing (lines 358-369) — empty body triggers default settings fallback. Per-call parameter overrides use same is<T>() detection as /api/llm/generate. Returns session ID like /api/llm/generate so client can poll /api/llm/result. Page JS calls this when user clicks Retry button; currently passes only config params from UI (temp, rep_penalty, etc. omitted — lines 348-355 show the full set but retry body is not constructed client-side, only chatRetryLast() is called, so overrides must come from default settings or be hardcoded in the firmware). Comments note this is the new path vs. legacy 'suppress' field (line 350).

<a id="post-api-llm-chat-clear"></a>

#### `POST /api/llm/chat/clear`

**Handler:** `handleLLMChatClear` · **Source:** `components/hardwareone/WebPage_LLM.cpp:383` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Wipe the conversation history. Clears all stored turns from the chat module.

- **Auth detail:** WEB_AUTH_OR_RETURN(req, ctx) at line 384 — returns 302 redirect if unauthenticated.
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json; success response: {"ok": true}. Failure response: {"ok": false, "error": "busy — stop first"}. Returns busy error if generation is in progress.
- **Observations:** Gated by chatClear() which enforces generation-idle check (line 385) — returns false if model is generating, causing error response. No parameters. Simple one-shot operation. Page could expose this as 'Clear conversation' button but currently no UI calls it (not visible in WebPage_LLM.h JavaScript).


<a id="group-maps"></a>

### Maps & GPS

The Maps & GPS functional area provides HTTP endpoints for interactive map viewing, GPS track management, and waypoint management on the device. It includes a canvas-based map viewer supporting multi-layer rendering (roads, water, buildings, parks, rail/transit), real-time GPS tracking with live capture, historical track loading/validation, and persistent waypoint storage tied to loaded maps. The page handler serves HTML + embedded JavaScript that calls the API endpoints; the API endpoints return JSON for map metadata, feature information, GPS tracks (live or file-based), and waypoint CRUD operations.

<a id="get-maps"></a>

#### `GET /maps`

**Handler:** `handleMapsPage` · **Source:** `components/hardwareone/WebPage_Maps.cpp:475` · **Kind:** page · **Auth:** session · **Maturity:** adequate

Renders the interactive maps UI page with map viewer, GPS track management, and waypoint editor. The page fetches data from sibling API endpoints dynamically.

- **Auth detail:** Checks WEB_AUTH_OR_RETURN(req, ctx) at line 476 which calls tgRequireAuth(ctx); redirects unauthenticated to /login
- **Parameters:** _none_
- **Request body:** none
- **Response:** text/html - Complete HTML page with embedded canvas (map-canvas), control buttons (zoom, rotate, reset, GPS center, search, waypoint add, maps/layers panels), three-column info panels (GPS Tracks, Transit Routes, Waypoints), and 2000+ lines of JavaScript (parseHWMap, renderMap, feature rendering, layer control, waypoint management, track loading, search, etc.). Page calls /api/maps/features, /api/waypoints, /api/gps/tracks endpoints from JS.
- **Observations:** Large embedded JS (765 lines in WebPage_Maps.h); handles map binary parsing (HWMAP v6 format with feature LOD thresholds), multi-layer canvas rendering with rotation/zoom, waypoint mode toggle, live GPS tracking UI. The page HTML is fragmented across multiple lines with hardcoded CSS/JS; UI complexity is high (modals, file browser integration via getFileBrowserScript(), color pickers, search dialog). Page makes calls to /api/cli (mapload, mapunload commands), /api/files/view (map file fetch), /api/waypoints (CRUD), /api/gps/tracks (live/file tracks). No visible input validation issues on the page side, but waypoint JSON parsing relies on untrusted data from /api/waypoints.

<a id="get-api-maps-features"></a>

#### `GET /api/maps/features`

**Handler:** `handleMapFeaturesAPI` · **Source:** `components/hardwareone/WebPage_Maps.cpp:58` · **Kind:** api · **Auth:** session · **Maturity:** barebones

Returns metadata about the currently loaded map on the device, including filename, feature counts, and optional place-name list. Called by the maps page to populate map info panel and enable search-by-name.

- **Auth detail:** Checks WEB_AUTH_JSON_OR_RETURN(req, ctx) at line 59 which calls tgRequireAuth(ctx) and sets Content-Type: application/json; redirects unauthenticated with 401/JSON
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json - Object with keys: mapName (string, filename of loaded map or empty), hasNames (boolean, true if map has name entries), featureCount (integer, total features in map header), nameCount (integer, count of place name entries), names (array of strings, only present if nameCount > 0, contains escaped place names from map)
- **Observations:** Responds with {error: 'No map loaded'} if MapCore::hasValidMap() returns false. JSON building is manual string concatenation (lines 69-98) with basic quote escaping (line 90 replaces " with \"). No pagination for large name lists; if a map has thousands of names they all serialize into one response. Place names come directly from map binary (no sanitization shown), risk of oversized response if map has huge name table.

<a id="get-api-waypoints"></a>

#### `GET /api/waypoints`

**Handler:** `handleWaypointsAPI` · **Source:** `components/hardwareone/WebPage_Maps.cpp:504` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Fetch list of all waypoints for the currently loaded map, including coordinates, notes, associated files, and which waypoint is the current navigation target.

- **Auth detail:** Checks WEB_AUTH_OR_RETURN(req, ctx) at line 506 which calls tgRequireAuth(ctx); redirects unauthenticated to /login
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json - Object with keys: success (boolean), mapName (string, filename of currently loaded map or empty), count (integer, active waypoint count), max (integer, MAX_WAYPOINTS constant), target (integer, index of selected target waypoint or -1), deviceMapLoaded (boolean, true if map is valid), waypoints (array of objects, only if map loaded; each object has: name, lat, lon, notes, fileCount, files [array of file paths if fileCount > 0]). If no map loaded, returns minimal response with deviceMapLoaded:false, count:0, waypoints:[].
- **Observations:** Uses JsonBufferGuard (Mutex timeout at line 510-514) to serialize access; returns 503 if mutex held. Iterates all MAX_WAYPOINTS slots (line 538); only includes waypoints where WaypointManager::getWaypoint(i) returns non-null. Couples map selection state to waypoint retrieval — switching maps mid-session via /api/cli mapload changes the waypoint context. No discoverability of max name length (11) or max notes length (255) or file count limit (MAX_WAYPOINT_FILES).

<a id="post-api-waypoints"></a>

#### `POST /api/waypoints`

**Handler:** `handleWaypointsAPI` · **Source:** `components/hardwareone/WebPage_Maps.cpp:504` · **Kind:** api · **Auth:** session · **Maturity:** barebones

Create, delete, modify, or select waypoints. The 'add' action adds a new waypoint and optionally sets notes. 'goto' sets the navigation target. 'clear' clears the current target. All mutating operations delegate to executeUnifiedWebCommand() which runs CLI-style commands through the command registry.

- **Auth detail:** Checks WEB_AUTH_OR_RETURN(req, ctx) at line 506 which calls tgRequireAuth(ctx); redirects unauthenticated to /login
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `action` | body-form | yes | string | Operation to perform. Supported values: 'add', 'rename', 'set_notes', 'clear_all', 'delete', 'goto', 'clear'. Each branch executes a corresponding command via executeUnifiedWebCommand(). |
  | `name` | body-form | no | string | Waypoint name for 'add' or 'rename' actions. Max 11 characters (not enforced server-side, truncation occurs in JS). Required for 'add'. |
  | `lat` | body-form | no | number | Latitude (degrees) for 'add' action. Required for 'add'. |
  | `lon` | body-form | no | number | Longitude (degrees) for 'add' action. Required for 'add'. |
  | `notes` | body-form | no | string | Notes/description for 'add' or 'set_notes' actions. Max 255 characters (not enforced server-side). Optional for 'add'. |
  | `index` | body-form | no | integer | Waypoint index (0-based slot in waypoint array) for 'rename', 'set_notes', 'delete', 'goto' actions. Required for these operations. |

- **Request body:** application/x-www-form-urlencoded - Form fields: action, name, lat, lon, notes, index. Parser implements manual URL decoding (lines 586-612): splits on '&', then '='; handles '+' as space and %XX escapes. Allocates 512-byte PSRAM buffer for body (line 567); returns 400/503 if receive fails or malloc fails.
- **Response:** application/json - Object with keys: success (boolean), error (string, only if success=false), index (integer, only on successful 'add', echoes the assigned waypoint index). For 'add': returns assigned index if command output starts with 'Added waypoint '; follows up with a second command to set notes if provided. For other actions: returns {success:true} or {success:false, error:<cmd output or generic message>}. Unknown action returns {success:false, error:'Unknown action'}.
- **Observations:** Command parsing is indirect: 'add' becomes 'waypoint add <lat> <lon> <name>', parsed/executed by CLI layer. Post body size limit is 511 bytes (line 573); no explicit validation of lat/lon ranges or name length, relies on CLI command handler. URL decode has subtle issue (line 601 removes 2 chars after hex-decoding but doesn't bounds-check i+2). Error messages vary (some from command output, some generic 'No free slots'). Mutex timeout error (503) is possible but undocumented. 'add' optionally issues a second command for notes if the first succeeds and notes were provided — two-phase operation increases risk of partial state if second command fails silently.

<a id="get-api-gps-tracks"></a>

#### `GET /api/gps/tracks`

**Handler:** `handleGPSTracksAPI` · **Source:** `components/hardwareone/WebPage_Maps.cpp:108` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Manage GPS track recording and retrieval. Supports live tracking (auto-start GPS and sensor logging), save track to file, load historical track files with validation, and list available GPS log files in the system.

- **Auth detail:** Checks WEB_AUTH_OR_RETURN(req, ctx) at line 109 which calls tgRequireAuth(ctx); redirects unauthenticated to /login
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `live` | query | no | string | Live tracking command or state query. Supported values: 'start' (begin live tracking, auto-start GPS and logging), 'stop' (end live tracking), 'save' (save active track to file), or '1' (query current live track data and last 500 points). Any other value returns current live track state. |
  | `file` | query | no | string | URL-encoded path to a GPS log file to load and validate. Must start with '/'. Returns track data + validation result (valid, partial, out_of_bounds) and coverage percentage. |

- **Request body:** none
- **Response:** application/json - Three response modes: (1) If ?live=<cmd> present: {live:true/false, started:true/stopped:true/saved:true, path:<file path>, points:<count>, error:<msg>}. (2) If ?live=1 or live=null (live query): {live:<bool>, count:<int>, distance:<float>, duration:<float>, speed:<float>, lastUpdate:<timestamp>, points:[{lat,lon},...]} (up to last 500 points). (3) If ?file=<path>: {success:true, validation:<'valid'|'partial'|'out_of_bounds'>, coverage:<float>, points:[{lat,lon},...], count:<int>, message:<validation message>}. (4) No query params (list files): {success:true, files:[{path:<string>, size:<int>},...]}.
- **Observations:** Live tracking delegates to CLI commands (opengps, sensorlog). live=start enables GPS and starts logging to /logging_captures/tracks/live.csv with sensorlog format. live=stop stops logging but doesn't disable GPS (may be intentional for continuous monitoring). live=save calls GPSTrackManager::saveTrack() and returns path/count or error. File loading (line 180-226) URL-decodes the path, validates it starts with '/', reads file to check for 'gps:' marker or CSV format (heuristic check first 15 lines, line 250-257), then calls GPSTrackManager::loadTrack() + validateTrack(). File listing (line 234-273) scans three hardcoded directories (/logging_captures, /logging_captures/tracks, /logging_captures/sensors) and checks each file for GPS markers. Streaming response chunks data (lines 162, 171-175, 209, 224-225); no content-length header or chunked encoding marker visible. Response format inconsistency: live queries return {live,count,distance,...,points}, file queries return {success,validation,coverage,...,points}, list returns {success,files} — three different top-level structures. Track points capped at 500 in live mode (line 165); file mode returns all points (no pagination).


<a id="group-espnow"></a>

### ESP-NOW Mesh

ESP-NOW mesh networking UI and API endpoints. Provides a web interface for managing ESP-NOW device pairing, peer-to-peer messaging with delivery tracking, mesh topology configuration, firmware capability exchange, and smart-home device metadata (friendly names, rooms, zones). The page endpoint streams a complete single-page app (~3700 lines of embedded JS) that polls /api/espnow/* endpoints for message updates, device lists, and mesh status. API endpoints return JSON with support for cursor-based pagination (messages), cached remote device capabilities, firmware manifests for OTA, and per-device metadata lookup.

<a id="get-espnow"></a>

#### `GET /espnow`

**Handler:** `handleEspNowPage` · **Source:** `components/hardwareone/WebPage_ESPNow.cpp:27` · **Kind:** page · **Auth:** session · **Maturity:** adequate

Serves the main ESP-NOW mesh networking UI page. Provides controls for device pairing, messaging, file transfer, mesh role configuration, and network topology visualization.

- **Auth detail:** WEB_AUTH_OR_RETURN macro at line 28 requires authenticated session; redirects to /login if unauthenticated
- **Parameters:** _none_
- **Request body:** none
- **Response:** text/html. Complete HTML page with embedded CSS and JavaScript for ESP-NOW mesh interface, including device list, messaging, file transfer, metadata management, mesh topology visualization, and settings panels. Page calls /api/cli (batch), /api/espnow/messages, /api/espnow/remotecap, /api/espnow/remotemanifest, /api/espnow/metadata endpoints via polling.
- **Observations:** Large embedded single-page app with ~3700 lines of HTML/CSS/JS. Page aggressively polls /api/espnow/messages (~7s cadence per comments), /api/cli/batch, and other endpoints. Calls many CLI commands (espnowstatus, espnowmode, bondstatus, espnowlist, espnowmeshes, espnowdeviceinfo, espnowmeshrole, espnowmeshstatus) for status updates. Frontend has several dedup/polling mechanisms for stability. No admin checks—regular authenticated users can access full UI.

<a id="get-api-espnow-messages"></a>

#### `GET /api/espnow/messages`

**Handler:** `handleEspNowMessages` · **Source:** `components/hardwareone/WebPage_ESPNow.cpp:168` · **Kind:** api · **Auth:** session · **Maturity:** mature

Poll received ESP-NOW text messages and delivery status. Returns paginated message log (seq cursor-based) and real-time delivery state updates for bubbles sent from web UI or other interfaces.

- **Auth detail:** WEB_AUTH_OR_RETURN macro at line 169 requires authenticated session
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `since` | query | no | uint32 | Sequence number cursor for pagination. Returns only messages with seqNum > since. Defaults to 0 (all messages). Used for polling; client tracks lastSeq and requests ?since=lastSeq to fetch new messages only. |
  | `mac` | query | no | string | MAC address filter (format AA:BB:CC:DD:EE:FF or AABBCCDDEEFF, URL-encoded). Returns messages from this peer only. Must be URL-decoded by handler (browser sends E8%3A6B%3A... for colons). |

- **Request body:** none
- **Response:** application/json. JSON object with two arrays: { "messages": [ { "seq": uint32, "reqId": uint32, "piece": uint, "of": uint, "mac": "XX:XX:XX:XX:XX:XX", "name": string, "msg": string (JSON-escaped), "enc": bool, "ts": uint64, "type": int, "sent": bool, "sendState": uint } ], "deliveries": [ { "msgId": uint32, "state": "pending"|"delivered"|"timeout"|"failed", "mac": "XX:XX:XX:XX:XX:XX", "ageMs": uint64 } ] }. Messages capped at 50 per poll (PSRAM build) or 15 (no-PSRAM). Deliveries from 16-slot SendStatus ring.
- **Observations:** Uses streaming chunked responses for efficiency. Implements custom JSON escaping (webEspnowSendJsonEscapedString) to avoid per-request allocations. Pre-2026-05 allocated 32 KB PSRAM per poll; now uses persistent static buffer (gWebMessagesBuf) reused for lifetime. Buffer sizing is PSRAM-aware. Handler applies MAC filter inline (URL-decodes %3A). sendState is 0=pending, 1=delivered, 2=timeout, 3=failed. If gEspNow uninitialized, returns empty {"messages":[]}. Concurrency: single-threaded httpd worker, no mutex needed.

<a id="get-api-espnow-remotecap"></a>

#### `GET /api/espnow/remotecap`

**Handler:** `handleEspNowRemoteCap` · **Source:** `components/hardwareone/WebPage_ESPNow.cpp:345` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Get cached capability summary of last-bonded remote device from bond requestcap. Returns device capabilities, role, firmware hash, sensor/feature/service bitmasks, and hardware specs (flash, PSRAM, WiFi channel, uptime).

- **Auth detail:** WEB_AUTH_OR_RETURN macro at line 346 requires authenticated session
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json. JSON object: { "valid": true|false, "deviceName": string, "mac": "XX:XX:XX:XX:XX:XX", "role": int (0=worker, 1=master), "roleName": "worker"|"master", "fwHash": hex string (32 chars, 16-byte MD5), "featureMask": uint, "serviceMask": uint, "sensorMask": uint, "features": string (human-readable comma-separated list), "services": string (human-readable list), "sensors": string (human-readable list), "flashSizeMB": uint, "psramSizeMB": uint, "wifiChannel": int, "uptimeSeconds": uint, "ageMs": uint (time since lastRemoteCapTime) }. If !lastRemoteCapValid, returns {"valid":false}.
- **Observations:** Data is cached in gEspNow->lastRemoteCap after a bond requestcap exchange. ageMs indicates staleness. Uses streaming chunks for efficiency. Feature/service/sensor lists are human-readable translations of bitmasks. fwHash is 16-byte hash formatted as 32-char hex. Response only valid if gEspNow initialized and lastRemoteCapValid flag set; otherwise returns minimal {"valid":false}.

<a id="get-api-espnow-remotemanifest"></a>

#### `GET /api/espnow/remotemanifest`

**Handler:** `handleEspNowRemoteManifest` · **Source:** `components/hardwareone/WebPage_ESPNow.cpp:432` · **Kind:** api · **Auth:** session · **Maturity:** barebones

List cached firmware manifests or retrieve a specific manifest by fwHash. Manifests describe firmware structure (partitions, versions, checksums) for OTA/upgrade operations.

- **Auth detail:** WEB_AUTH_OR_RETURN macro at line 433 requires authenticated session
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `fwHash` | query | no | string | Firmware hash (hex string, no colons). If provided, returns the specific manifest file for that hash. Without this parameter, lists all available cached manifests. |

- **Request body:** none
- **Response:** application/json. Two response shapes: (1) With fwHash: { "fwHash": "hex_string", "manifest": <raw JSON object from file> }. (2) Without fwHash (list mode): { "manifests": [ { "fwHash": "hex_string", "size": int } ] }. Manifest files stored at /system/manifests/{fwHash}.json; content is streamed as-is.
- **Observations:** Uses VFS::openGuarded() for file access with auth context. Filesystem readiness checked at line 437-441. No pagination. If filesystem not ready or manifest not found, returns {"error":"..."}. Manifest content (JSON) is streamed directly into response envelope. Size field is file size in bytes. Stale flag: filesystem dependencies, no validation of manifest format.

<a id="get-api-espnow-metadata"></a>

#### `GET /api/espnow/metadata`

**Handler:** `handleEspNowMetadata` · **Source:** `components/hardwareone/WebPage_ESPNow_Metadata.cpp:38` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Query smart-home metadata for a peer device by MAC address. Returns device identity (friendly name, room, zone, tags, stationary flag) for home automation and mesh discovery.

- **Auth detail:** WEB_AUTH_JSON_OR_RETURN macro at line 39 requires authenticated session and sets Content-Type: application/json
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `mac` | query | yes | string | Target device MAC address (format AA:BB:CC:DD:EE:FF, URL-encoded with colons as %3A). Handler performs urlDecode() before parsing. |

- **Request body:** none
- **Response:** application/json. If found in gMeshPeerMeta: { "found": true, ...fields from espnowSerializeMeshPeerMeta(o, meta) ... } which typically includes "mac", "name", "deviceName", "friendlyName", "room", "zone", "tags", "stationary" (bool), plus other smart-home metadata fields. If not found: { "found": false }. All string fields are JSON-escaped via ArduinoJson serialization.
- **Observations:** Looks up target MAC in gMeshPeerMeta array (gMeshPeerSlots slots). Found metadata is serialized via espnowSerializeMeshPeerMeta(), which uses shared core serializer (ensures shape matches `espnowdevices json` CLI output). Uses ArduinoJson for correct escaping. URL-decode required because browser encodes colons as %3A. Debug logging at lines 65, 72-96 (ESPNOW_METADATAF macros). If gMeshPeerMeta null or MAC not found, returns {"found":false}. No pagination or filters.


<a id="group-bond"></a>

### Bond (paired-device control channel)

The Bond functional area provides HTTP endpoints for managing paired ESP-NOW device control channels. A "bonded" device is one of two devices in a master-worker relationship, communicating over ESP-NOW. The master controls the worker's sensors and settings, streams sensor data, executes remote commands, and browses the worker's filesystem. The /bond page is the UI hub; API endpoints handle status polling, sensor streaming control, remote command execution, role management, settings/schema synchronization with the bonded peer, and filesystem browsing on the peer device.

<a id="get-bond"></a>

#### `GET /bond`

**Handler:** `handleBondPage` · **Source:** `components/hardwareone/WebPage_Bond.cpp:958` · **Kind:** page · **Auth:** session · **Maturity:** mature

Render the bond management dashboard UI. Displays bonded device status, capabilities, sensors, and provides controls to enable/disable remote sensors, stream sensor data, execute CLI commands on the remote device, swap roles, and configure bonding.

- **Auth detail:** WEB_AUTH_OR_RETURN macro: checks auth via tgRequireAuth(), redirects to /login if not authenticated.
- **Parameters:** _none_
- **Request body:** none
- **Response:** HTML page. Streams the bond dashboard page with embedded JavaScript that polls /api/bond/status every 5s, renders device bond status, capabilities, sensors, link quality, and provides CLI command execution UI. The page calls /api/bond/paired-devices (device selection), /api/bond/status (bond state), /api/bond/stream (sensor streaming toggle), /api/bond/exec (remote CLI), /api/bond/role (master/worker swap), /api/cli (bondconnect/bonddisconnect/bondresync), and /api/espnow/messages (remote command output polling).
- **Observations:** Page heavily JavaScript-driven. Uses chunked HTML streaming (httpd_resp_send_chunk). JS polls /api/bond/status every 5s for UI refresh; shows progress overlay while bond is syncing, then renders full dashboard once synced. The page is complex with multiple sub-components (device card, sensors card, local capabilities card, remote CLI card). No apparent pagination or filtering concerns. The page correctly uses hw.postForm and hw.fetchJSON helpers (defined elsewhere) for auth-cookie handling. Restores CLI state (input value + output text) across re-renders.

<a id="get-api-bond-status"></a>

#### `GET /api/bond/status`

**Handler:** `handleBondStatus` · **Source:** `components/hardwareone/WebPage_Bond.cpp:968` · **Kind:** api · **Auth:** session · **Maturity:** mature

Poll current bond state: peer online/offline status, ESP-NOW link quality (RSSI, heartbeat counts, packet loss), peer hardware capabilities, live sensor connection state, streaming settings, and memory stats. Used by UI to refresh dashboard every 5 seconds.

- **Auth detail:** WEB_AUTH_JSON_OR_RETURN macro: checks auth via tgRequireAuth(), returns 401 if not authenticated.
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json. Returns comprehensive bond state object:
{
  espnowEnabled: bool,
  bonded: bool,
  peerConfigured: bool,
  peerOnline: bool,
  peerMac: string ("AA:BB:CC:DD:EE:FF"),
  localMac: string,
  peerName: string,
  role: int (1=master, 0=worker),
  lastHeartbeat: ms,
  lastHeartbeatAgeSec: uint32,
  heartbeatsRx: uint32,
  heartbeatsTx: uint32,
  rssi: int (dBm, or -100 if unknown),
  rssiLast: int,
  peerUptime: uint32 (seconds),
  peerSettingsHash: uint32 (CRC32),
  _dbg_synced: bool,
  _dbg_capValid: bool,
  _dbg_capSent: bool,
  _dbg_statusValid: bool,
  streamThermal/Tof/Imu/Gps/Input/Fmradio/Rtc/Presence: bool (stream enable flags),
  localCapabilities: {
    features: string (comma-separated list),
    sensors: string,
    featureMask: uint32,
    sensorMask: uint32,
    freeHeap: bytes,
    minFreeHeap: bytes,
    flashMB: uint32,
    psramMB: uint32,
    psramKB: uint32,
    sensorConnectedMask: uint16,
    sensorEnabledMask: uint16,
    inputDeviceType: int (0=none, 1=Gamepad, 2=ANO Encoder)
  },
  capabilities: null | {
    features: string,
    sensors: string,
    services: string,
    flashMB: uint32,
    psramMB: uint32,
    featureMask: uint32,
    sensorMask: uint32,
    serviceMask: uint32,
    inputDeviceType: int
  },
  sensorConnected: null | {
    valid: bool (have we received live status?),
    thermal/tof/imu/gps/input/apds/fmradio/presence/rtc: bool (connected),
    thermalOn/tofOn/imuOn/gpsOn/inputOn/presenceOn/rtcOn/apdsOn/fmradioOn: bool (enabled)
  },
  peerStatus: {
    valid: bool,
    sensorEnabled: uint16 (mask),
    sensorConnected: uint16 (mask),
    freeHeap: bytes,
    minFreeHeap: bytes,
    wifiConnected: bool,
    bluetoothActive: bool,
    httpActive: bool,
    ageSec: uint32 (time since peer last sent status)
  }
}
- **Observations:** Very comprehensive status object — uses chunked sending to avoid large intermediate Strings. Builds response in multiple sections (local cap, remote cap, sensor connected, peer status). Handles null caps gracefully (no capabilities synced yet). Debug fields (_dbg_*) aid troubleshooting. Response can be large (~2-3 KB with full caps); no pagination but acceptable since it's a single polled object. The code correctly coerces NULL c_str() to empty strings to avoid crashes. Streaming settings are read from gSettings; sensor connected/enabled masks are read from live cache or fall back to capabilities if no live status yet.

<a id="post-api-bond-stream"></a>

#### `POST /api/bond/stream`

**Handler:** `handleBondStream` · **Source:** `components/hardwareone/WebPage_Bond.cpp:1252` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Toggle or set streaming state for a specific sensor on the bonded peer. Sends a bondstream CLI command to the peer via the bond channel. Only the master can control streaming.

- **Auth detail:** WEB_AUTH_JSON_OR_RETURN macro: checks auth via tgRequireAuth(), returns 401 if not authenticated.
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `sensor` | body-form | yes | string | Sensor ID: thermal, tof, imu, gps, input, fmradio, presence, or rtc |
  | `action` | body-form | yes | string | Action: on, off, or toggle. If toggle, resolves to on/off based on current gSettings state. |

- **Request body:** application/x-www-form-urlencoded. Format: sensor=<id>&action=<on|off|toggle>
- **Response:** application/json. On success: {"success":true,"sensor":"<id>","enabled":bool}. On error: {"success":false,"error":"<reason>"}. Error reasons: "Bond not synced", "No data", "Missing sensor parameter", "Unknown sensor", "Stream command failed".
- **Observations:** Guard checks isBondSynced() before allowing. Parses form body manually (strstr + string extraction) rather than using httpd_query_parse (seems fragile). Resolves toggle action by consulting current gSettings state; does NOT query peer state. Routes through executeUnifiedWebCommand with 'bondstream <sensor> <on|off>' command. Response enumerates which sensor was toggled and the new state.

<a id="post-api-bond-exec"></a>

#### `POST /api/bond/exec`

**Handler:** `handleBondExec` · **Source:** `components/hardwareone/WebPage_Bond.cpp:1346` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Execute a CLI command on the bonded peer device and return the result. Used by the remote CLI command box on the bond page. Commands are routed through the bond session with 'remote:' prefix.

- **Auth detail:** WEB_AUTH_JSON_OR_RETURN macro: checks auth via tgRequireAuth(), returns 401 if not authenticated.
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `cmd` | body-form | yes | string | Command to execute on the bonded peer (e.g., "status", "memory", "openThermal", "closeThermal") |

- **Request body:** application/x-www-form-urlencoded. Format: cmd=<URL-encoded command>. Command is URL-decoded (handles %xx escapes and + for space).
- **Response:** application/json. {"success":true|false,"result":"<JSON-escaped output>"}. Result is the command output from the peer, escaped for JSON inclusion (\" for quotes, \n for newlines, etc.).
- **Observations:** Parses POST body manually to extract cmd= parameter and performs basic URL decoding inline. Routes through executeUnifiedWebCommand() with 'remote:' prefix. Result string is JSON-escaped character-by-character (handles quotes, backslash, newlines, tabs, carriage returns). Permits arbitrary command execution for authenticated users — no validation of command content. This is the primary interface for running remote sensor commands (openThermal, closeThermal, etc.) and diagnostic commands.

<a id="post-api-bond-role"></a>

#### `POST /api/bond/role`

**Handler:** `handleBondRole` · **Source:** `components/hardwareone/WebPage_Bond.cpp:1420` · **Kind:** api · **Auth:** session · **Maturity:** mature

Swap master/worker roles. Sends bondrole command to peer first, then to local device. Resets bond handshake and invalidates capability cache on both sides to trigger re-sync.

- **Auth detail:** WEB_AUTH_JSON_OR_RETURN macro: checks auth via tgRequireAuth(), returns 401 if not authenticated.
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `action` | body-form | yes | string | Always 'swap' in current code (hardcoded from UI). Swaps master/worker roles on both devices. |

- **Request body:** application/x-www-form-urlencoded. Format: action=swap
- **Response:** application/json. On success: {"success":true,"role":<0|1>,"roleName":"<master|worker>"}. On error: {"success":false,"error":"<reason>"}. Error reasons: "Bond mode not enabled", "Remote role change failed: ...".
- **Observations:** Important: sends remote role change FIRST before local change. Comment explains this prevents a race condition where local worker sends CAP_REQ before peer becomes master. Uses executeUnifiedWebCommand twice (once for peer, once for local). If remote change fails, aborts and does NOT change local role (to avoid split-brain). No param parsing (action is hardcoded in UI as 'swap'). Returns new role as integer (1=master, 0=worker) and string name. Callers should poll /api/bond/status afterward to see updated state.

<a id="post-api-bond-cli-batch"></a>

#### `POST /api/bond/cli/batch`

**Handler:** `handleBondCliBatch` · **Source:** `components/hardwareone/WebPage_Bond.cpp:1472` · **Kind:** api · **Auth:** session · **Maturity:** mature

Execute multiple CLI commands on the bonded worker in sequence, atomically. Routes each command through the bond session with 'remote:' prefix. Used by the bonded-device settings panel to apply edits (beginwrite + tz + locale + savesettings, etc.) in one HTTP request.

- **Auth detail:** WEB_AUTH_JSON_OR_RETURN macro: checks auth via tgRequireAuth(), returns 401 if not authenticated.
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `Content-Length` | header | yes | number | Must be > 0 and <= 32768 bytes |

- **Request body:** application/json. Body must be valid JSON with shape: {"commands":["cmd1","cmd2",...]}. Each command is a string; empty commands are skipped.
- **Response:** application/json. {"ok":true,"count":<N>,"results":["result1","result2",...]} (one result per command, in order). On error: {"ok":false,"error":"<reason>"}. Error reasons: "Bonded master required", "Invalid content length", "OOM", "Expected {\"commands\":[...]} JSON body".
- **Observations:** Master-only (checks isBondMaster()). Allocates buffer from PSRAM for the body (up to 32 KB). Deserializes JSON via ArduinoJson (PSRAM_JSON_DOC macro). Processes each command via executeUnifiedWebCommand with 'remote:' prefix. Results array mirrors command array — mismatches in count indicate truncation/error. Adds small vTaskDelay between commands for safety. If server becomes NULL during processing (unlikely), breaks early. Returns per-command results so client can identify which commands succeeded/failed.

<a id="post-api-bond-settings-sync"></a>

#### `POST /api/bond/settings/sync`

**Handler:** `handleBondSettingsSync` · **Source:** `components/hardwareone-idf/components/hardwareone/WebPage_Bond.cpp:1550` · **Kind:** api · **Auth:** session · **Maturity:** mature

Force a fresh pull of the worker's settings.json from disk cache to local cache. Resets the sync flag, sends a SETTINGS_REQ over the bond, and polls for completion (up to ~6 seconds). Returns the CRC32 hash of the pulled settings so the UI can detect if the worker changes settings while the form is open.

- **Auth detail:** WEB_AUTH_JSON_OR_RETURN macro: checks auth via tgRequireAuth(), returns 401 if not authenticated.
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json. On success: {"ok":true,"elapsedMs":<N>,"peerSettingsHash":<uint32>}. On error: {"ok":false,"error":"<reason>"}. Error reasons come from BondedPeer::lastError() (e.g., "not bonded", "timeout", "sync in progress").
- **Observations:** Master-only (implicit via BondedPeer::requestSettingsSync()). Does NOT parse any request params — always performs a full settings sync. Polls for completion synchronously in the HTTP handler (typical wait time ~1-2s for file transfer + retry margin = 6s timeout). Returns peerSettingsHash for UI to use as formLoadedHash — subsequent /api/bond/status polls compare peer's live hash to detect out-of-band worker settings changes. Error strings are delegated to BondedPeer layer.

<a id="get-api-bond-settings-schema"></a>

#### `GET /api/bond/settings/schema`

**Handler:** `handleBondSettingsSchema` · **Source:** `components/hardwareone/WebPage_Bond.cpp:1590` · **Kind:** api · **Auth:** session · **Maturity:** mature

Read the cached schema file from disk. The schema describes the structure of the peer's settings (form fields, types, ranges). Clients should call /api/bond/settings/schema/sync first if the cache may be stale, then read this endpoint to get the fresh schema.

- **Auth detail:** WEB_AUTH_JSON_OR_RETURN macro: checks auth via tgRequireAuth(), returns 401 if not authenticated.
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json (the cached schema file). Returns the raw JSON the peer emitted, byte-for-byte, from the cache at /system/espnow/peers/<MAC>/schema.json. On error: {"error":"<reason>"}.
- **Observations:** Read-only — does NOT trigger a sync. Just reads the on-disk cache file via BondedPeer::readCachedSchemaJson(). If cache is missing or unreadable, returns error object with message from BondedPeer::lastError(). The schema JSON is the peer's native schema structure (not wrapped). No pagination. Schema size is board-dependent (see /api/settings/schema notes); it is NOT ~8 KB.

<a id="post-api-bond-settings-schema-sync"></a>

#### `POST /api/bond/settings/schema/sync`

**Handler:** `handleBondSettingsSchemaSync` · **Source:** `components/hardwareone-idf/components/hardwareone/WebPage_Bond.cpp:1608` · **Kind:** api · **Auth:** session · **Maturity:** mature

Force a fresh pull of the peer's settings schema (form field definitions). Resets bondSchemaReceived flag, sends SCHEMA_REQ over the bond, and polls for completion (~6 seconds). Mirrors /api/bond/settings/sync exactly but for the schema file instead.

- **Auth detail:** WEB_AUTH_JSON_OR_RETURN macro: checks auth via tgRequireAuth(), returns 401 if not authenticated.
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json. On success: {"ok":true,"elapsedMs":<N>}. On error: {"ok":false,"error":"<reason>"}.
- **Observations:** Master-only (implicit via BondedPeer::requestSchemaSync()). No params — always triggers a full schema resync. Synchronous HTTP handler that blocks while waiting for file transfer. ~1-2s typical transfer time covered by 6s timeout. Error strings come from BondedPeer layer. Schema transport uses V4 file pipeline (same as settings) because the schema far exceeds the 4 KB unified-command buffer cap.

<a id="get-api-bond-settings"></a>

#### `GET /api/bond/settings`

**Handler:** `handleBondSettings` · **Source:** `components/hardwareone-idf/components/hardwareone/WebPage_Bond.cpp:1636` · **Kind:** api · **Auth:** session · **Maturity:** mature

Read the peer's cached settings.json from disk. Clients should call /api/bond/settings/sync first to ensure the cache is fresh.

- **Auth detail:** WEB_AUTH_JSON_OR_RETURN macro: checks auth via tgRequireAuth(), returns 401 if not authenticated.
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json. Returns the cached peer settings.json wrapped in {"settings":{...}} to match the local /api/settings shape. On error: {"error":"<reason>"}.
- **Observations:** Read-only — does NOT trigger a sync. Wraps the raw settings JSON in {"settings":{...}} so response shape mirrors /api/settings (this allows SchemaPanel to reuse the same response handling for both local and bonded targets). Uses chunked sending to avoid materializing the large JSON in a single String. If cache is missing, returns error object from BondedPeer::lastError().

<a id="get-api-bond-paired-devices"></a>

#### `GET /api/bond/paired-devices`

**Handler:** `handleBondPairedDevices` · **Source:** `components/hardwareone-idf/components/hardwareone/WebPage_Bond.cpp:1661` · **Kind:** api · **Auth:** session · **Maturity:** mature

List all ESP-NOW paired devices (excluding self). Used by the bond page's device-selection UI to populate the dropdown when configuring a new bond.

- **Auth detail:** WEB_AUTH_JSON_OR_RETURN macro: checks auth via tgRequireAuth(), returns 401 if not authenticated.
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json. {"devices":[{"mac":"AA:BB:CC:DD:EE:FF","name":"DeviceName","friendlyName":"...","room":"...","zone":"...","tags":"...","stationary":bool,"encrypted":bool},...]}. Empty array if no paired devices.
- **Observations:** Filters out the local device (isSelfMac check). Iterates gEspNow->devices array. Returns empty array gracefully if ESP-NOW is not initialized. Has a safety coercion for NULL c_str() (uses a lambda sz() to return empty string instead of NULL, avoiding LoadProhibited crashes when rendering freshly-paired devices with unconstructed String members). Uses chunked sending for the JSON array. Response can be large if many devices are paired (e.g., 50 devices ≈ 10-15 KB), but no pagination — acceptable since it's typically < 30 devices.

<a id="get-api-bond-fs-list"></a>

#### `GET /api/bond/fs/list`

**Handler:** `handleBondFsList` · **Source:** `components/hardwareone/WebPage_Bond.cpp:1827` · **Kind:** api · **Auth:** session · **Maturity:** adequate

List directory contents on the bonded peer's filesystem. Uses the V4 file protocol (FS_LIST_REQ/REPLY) instead of CLI scraping. Paginated to handle large directories; client polls with incrementing start indices.

- **Auth detail:** WEB_AUTH_JSON_OR_RETURN macro: checks auth via tgRequireAuth(), returns 401 if not authenticated.
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `path` | query | yes | string | Directory path to list (e.g., "/", "/sd", "/system/espnow"). Must be URL-encoded (/ → %2F, space → %20). Max path length FILE_MANAGER_MAX_PATH (~256 bytes). |
  | `start` | query | no | integer (0-65535) | Start index for pagination (default 0). Used for browsing large directories. |

- **Request body:** none
- **Response:** application/json. On success: {"success":true,"path":"<canonical_path>","totalEntries":<N>,"hasMore":bool,"nextStartIndex":<N>,"entries":[{"name":"filename","isDir":bool,"size":<bytes>,"perms":<octal_perms>},...]}. On error: {"success":false,"error":"<status_tag>","path":"<path>"}. Status tags: ok, not_found, not_a_dir, perm_denied, io_error, too_busy, not_ready, unknown.
- **Observations:** Master-only (implicit via BondedPeer::peerMacBytes()). Single-threaded bridge (WebFsBridge slot with mutex) to serialize concurrent web requests; second request gets 'bridge busy' error and can retry. Synchronous handler that blocks up to 6 seconds waiting for peer response. URL-decodes path minimally (only %2F and %20, matching local files API). Queries must include path= parameter; start defaults to 0. Response includes totalEntries (all entries in dir) and hasMore (more entries beyond this batch). Entries may be empty if path contains no files. JSON-escapes filenames (handles quotes, backslash, control chars). Snapshot of peer's canonical path returned in response for UI consistency.

<a id="get-api-bond-fs-stat"></a>

#### `GET /api/bond/fs/stat`

**Handler:** `handleBondFsStat` · **Source:** `components/hardwareone-idf/components/hardwareone/WebPage_Bond.cpp:1976` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Get filesystem usage statistics (total, used, free) for a mount point on the bonded peer. Mirrors local /api/fs/stat but queried over the bond.

- **Auth detail:** WEB_AUTH_JSON_OR_RETURN macro: checks auth via tgRequireAuth(), returns 401 if not authenticated.
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `path` | query | no | string | Filesystem mount/path to stat (e.g., "/", "/sd"). Defaults to "/" if omitted. URL-encoded. |

- **Request body:** none
- **Response:** application/json. On success: {"success":true,"path":"<path>","total":<uint64>,"used":<uint64>,"free":<uint64>,"usagePercent":<N.N>}. On error: {"success":false,"error":"<status_tag>","path":"<path>"}.
- **Observations:** Master-only (implicit via BondedPeer::peerMacBytes()). Uses FS_STAT_REQ/REPLY protocol. Separate bridge from fs/list (WebFsStatBridge + mutex) so storage-stat queries don't block file browsing and vice versa. Synchronous HTTP handler with 6s timeout. Path defaults to "/" if omitted. Usage percentage returned as percentUsedX10 (so 50.5% is returned as 505 to avoid float precision issues in JSON). Returns 64-bit integers for size fields (safe in JSON up to 2^53 for storage sizes < 1 EB).

<a id="get-api-bond-fs-get"></a>

#### `GET /api/bond/fs/get`

**Handler:** `handleBondFsGet` · **Source:** `components/hardwareone-idf/components/hardwareone/WebPage_Bond.cpp:2066` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Request a file from the bonded peer. Sends FS_GET_REQ, waits for FS_GET_ACK, then initiates asynchronous file transfer. The actual file content arrives via FILE_START/DATA/END messages handled by the standard inbound file pipeline. File lands at /espnow/received/<MAC>/<basename>.

- **Auth detail:** WEB_AUTH_JSON_OR_RETURN macro: checks auth via tgRequireAuth(), returns 401 if not authenticated.
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `path` | query | yes | string | Full file path to pull from peer (e.g., "/data/settings.json"). URL-encoded. |

- **Request body:** none
- **Response:** application/json. On success: {"success":true,"path":"<path>","size":<bytes>,"message":"transfer initiated; poll local FS for file landing"}. On error: {"success":false,"error":"<status_tag>","path":"<path>"}.
- **Observations:** Master-only (implicit via BondedPeer::peerMacBytes()). Two-stage protocol: sync ACK receipt in HTTP handler (up to 6s timeout), then async file transfer. This endpoint does NOT wait for the file to fully transfer — it returns once the peer has acknowledged the request and started sending. Client must poll the local filesystem to detect when the file has arrived. Uses FS_GET_ACK callback and separate bridge (WebFsGetBridge + mutex) to avoid blocking other file operations. Peer echoes back canonical path in ACK for UI consistency.


<a id="group-battery"></a>

### Battery & Power

Battery & Power functional area provides real-time battery status monitoring and historical data visualization. The /battery page displays live metrics (percentage, voltage, charging source) and a history section with interactive canvas chart and CSV table of battery samples. The /api/battery/status endpoint serves JSON snapshots of current battery state, with capability-flagged fields based on the compiled battery backend (fuelgauge, ADC, or USB-only). Both endpoints require session authentication.

<a id="get-battery"></a>

#### `GET /battery`

**Handler:** `handleBatteryPage` · **Source:** `components/hardwareone/WebPage_Battery.cpp:144` · **Kind:** page · **Auth:** session · **Maturity:** adequate

Render the Battery & Power web page. Displays live battery metrics (percentage, voltage, status, charge source) and historical battery data visualization (chart and table from battery.csv log).

- **Auth detail:** WEB_AUTH_OR_RETURN macro redirects unauthenticated users to /login
- **Parameters:** _none_
- **Request body:** none
- **Response:** text/html; charset=utf-8. HTML page with card-based layout: Battery status display (percentage, voltage, status, charging source) and history section with canvas chart and CSV table. The page embeds inline JavaScript that calls /api/battery/status (polling every 2s) and /api/files/view?name=battery.csv&mode=raw to render live updates and historical data.
- **Observations:** Page calls two downstream APIs: /api/battery/status (for live data polling) and /api/files/view (for CSV log). The history rendering is entirely client-side (CSV parsing, canvas charting, table generation). No pagination on the CSV table—it slices to last 60 rows; full file is fetched every refresh. Chart only renders if battery.csv exists (graceful no-op otherwise). No inline error recovery beyond console logging.

<a id="get-api-battery-status"></a>

#### `GET /api/battery/status`

**Handler:** `handleBatteryStatus` · **Source:** `components/hardwareone/WebPage_Battery.cpp:23` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Return a live snapshot of battery state as JSON. Single-source-of-truth schema shared with the CLI 'battery json' command and BLE battery reports. Capability-flagged (backend type determines which fields are populated).

- **Auth detail:** WEB_AUTH_OR_RETURN macro (line 24); unauthenticated requests return ESP_OK without response body
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json. JSON object with fields: schema (integer), present (boolean), backend (string: 'fuelgauge'|'adc'|'usb-only'), voltage (float), percentage (float), status (string), charging (boolean), usbPresent (boolean), vbusSense (boolean), lastReadMsAgo (unsigned long). If backend='fuelgauge', also includes: ratePctPerHr (float), etaMinutes (long, only if discharging). If backend='adc', also includes: rawADC (unsigned int). On allocation failure, returns '{}'.
- **Observations:** Uses static PSRAM buffer (512 bytes) allocated once. No query parameters. Hardcoded 512-byte limit—large JSON objects will silently truncate via serializeJson. On OOM, returns empty object '{}' with no error indication. Schema includes build-time backend selection which determines available fields; this is correct for single-backend devices but makes the response polymorphic.


<a id="group-connectivity"></a>

### Bluetooth, Speech & MQTT pages

This functional area provides web-based control and monitoring for three integrated device features: Bluetooth (BLE server and G2 glasses client modes), Speech Recognition (ESP-SR wake word detection and command recognition), and MQTT client publishing. The /bluetooth page allows users to manage BLE device discovery, streaming data intervals, and G2 glasses telemetry. The /speech page provides controls for ESP-SR engine tuning, audio input levels, and model management. The /mqtt page displays MQTT broker connection status, configuration summary, and which sensors are being published to external topics. All three pages are authentication-gated and rely heavily on CLI command execution via the /api/cli endpoint, with real-time polling of JSON and structured text responses to update UI state.

<a id="get-bluetooth"></a>

#### `GET /bluetooth`

**Handler:** `handleBluetoothPage` · **Source:** `components/hardwareone/WebPage_Bluetooth.cpp:20` · **Kind:** page · **Auth:** session · **Maturity:** adequate

Provides web UI for configuring and managing Bluetooth (BLE) server and G2 glasses client functionality, including device discovery, streaming data configuration, and power settings.

- **Auth detail:** Requires authentication via WEB_AUTH_OR_RETURN macro (line 21), which redirects unauthenticated users to /login.
- **Parameters:** _none_
- **Request body:** none
- **Response:** Content-Type: text/html. HTML page with Bluetooth device management interface including connection lists, streaming controls, configuration settings (device name, TX power, auto-start, auth requirement), and G2 glasses client-mode panels (temples, ring, display). Page calls JS functions that invoke /api/cli commands (bleinfo json, blestream, blename, bletxpower, bleautostart, blerequireauth, g2status, ringstatus, g2scan, g2init, g2deinit, etc.). Embedded CSS and JavaScript for real-time status polling, connection management, and mode switching.
- **Observations:** Large embedded HTML/CSS/JavaScript directly in header file (WebPage_Bluetooth.h lines 19-1145). Page is heavily dependent on CLI commands executed via /api/cli post requests. No direct request parameters; all config is loaded via CLI queries on page load and polling. Uses adaptive polling (FAST=1500ms when active, SLOW=15000ms when idle). JS uses a client-side queue to avoid overwhelming the server's 50ms /api/cli rate limiter. G2 glasses support is conditionally compiled (ENABLE_G2_GLASSES). Page couples tightly to multiple internal systems: bleinfo JSON structure, G2 status text format, ring status format. Possible future refactoring opportunity: extract Bluetooth API calls to dedicated JSON endpoints (/api/bluetooth/status, /api/bluetooth/config) rather than CLI text parsing.

<a id="get-speech"></a>

#### `GET /speech`

**Handler:** `handleSpeechPage` · **Source:** `components/hardwareone/WebPage_Speech.cpp:20` · **Kind:** page · **Auth:** session · **Maturity:** adequate

Provides web UI for controlling ESP-SR speech recognition engine, including wake word and command detection, audio input tuning, and custom model management.

- **Auth detail:** Requires authentication via WEB_AUTH_OR_RETURN macro (line 21), which redirects unauthenticated users to /login.
- **Parameters:** _none_
- **Request body:** none
- **Response:** Content-Type: text/html. HTML page with Speech Recognition (ESP-SR) control interface including status indicator, start/stop buttons, audio level meter with VAD indicator, statistics (wake word count, command count, last confidence, reject count), detection log, debug/tuning controls (raw mode toggle, auto-tune toggle, AFE gain and dynamic gain selectors), and model file management (file explorer, upload section). Embedded CSS for responsive grid layout and audio visualization. Embedded JavaScript that polls /api/cli commands (srstatus, srstart, srstop, srraw, srautotune, srtuninggain, srdyngain, ls /sd/ESP-SR Models, sr loadwake, sr loadcmds) and renders real-time status updates.
- **Observations:** Large embedded HTML/CSS/JavaScript directly in header file (WebPage_Speech.h lines 19-689). Heavily CLI-dependent: srstatus returns JSON with running/wakeActive/wakeCount/commandCount/lastConfidence/volumeDb/vadState/micgain/state/category/subcategory/hasAFE/hasMultiNet/wnModelName/mnModelName/autotuneActive. Detection log tracks history of wake words and commands. Audio meter uses dB-to-percentage mapping (-60dB=0%, 0dB=100%). File browser uses `ls` CLI for model discovery; upload goes to /api/upload. Gain controls apply tuning via srtuninggain and srdyngain commands. JS parses JSON srstatus output but also has fallback text parsing for older firmware. Tight coupling to CLI text/JSON formats; speech-recognition feature is conditionally compiled (ENABLE_ESP_SR), skipped if not available or if ESP32-S3 PSRAM not enabled.

<a id="get-mqtt"></a>

#### `GET /mqtt`

**Handler:** `handleMqttPage` · **Source:** `components/hardwareone/WebPage_MQTT.cpp:441` · **Kind:** page · **Auth:** session · **Maturity:** adequate

Provides web UI for monitoring and controlling MQTT broker connectivity and configuring which sensor data is published to external topics for home automation integration.

- **Auth detail:** Requires authentication via WEB_AUTH_OR_RETURN macro (line 442), which redirects unauthenticated users to /login.
- **Parameters:** _none_
- **Request body:** none
- **Response:** Content-Type: text/html. HTML page with MQTT client status and control interface. Page streams HTML in chunks. Includes: Service Status panel (enable/disable MQTT toggle, refresh button, status indicator), Connection panel (status dot + text, connect/disconnect buttons), Configuration table (host, port, security/TLS mode, username, password masked, base topic, publish interval, auto-start setting), Published Data table (toggles for System Info, WiFi Info, Thermal Sensor, ToF, IMU, Presence, GPS, APDS, RTC, Input Device sensors), External Sensors panel (if subscriptions enabled, shows received sensor data with age), About MQTT info box. Embedded JavaScript for service enable/disable via /api/cli, connection control via /api/cli (openmqtt, closemqtt, mqttclientenabled), and auto-refresh polling of /api/mqtt/status every 5 seconds.
- **Observations:** Handler streams response in chunks using httpd_resp_send_chunk for memory efficiency. Password field is masked (shown as ********). MQTT configuration is read from gSettings global at render time (mqttClientEnabled, mqttHost, mqttPort, mqttTLSMode, mqttCACertPath, mqttUser, mqttPassword, mqttBaseTopic, mqttPublishIntervalMs, mqttAutoStart, and per-sensor publish flags). Service enable/disable calls CLI via /api/cli (mqttclientenabled command). Connection control calls /api/cli (openmqtt/closemqtt). Auto-refresh polling calls /api/mqtt/status. External sensor listing iterates up to 20 sensors (getExternalSensorCount/getExternalSensor) and displays name, value (truncated to 100 chars), and age in seconds. Sensor publish toggles are conditional-compiled (ENABLE_THERMAL_SENSOR, ENABLE_TOF_SENSOR, ENABLE_IMU_SENSOR, ENABLE_PRESENCE_SENSOR, ENABLE_GPS_SENSOR, ENABLE_APDS_SENSOR, ENABLE_RTC_SENSOR, ENABLE_OLED_INPUT). Configuration edit link points to /settings page. Inlined CSS + JavaScript couples page tightly to gSettings schema and CLI output formats.

<a id="get-api-mqtt-status"></a>

#### `GET /api/mqtt/status`

**Handler:** `handleMqttStatus` · **Source:** `components/hardwareone/WebPage_MQTT.cpp:453` · **Kind:** api · **Auth:** session · **Maturity:** adequate

JSON API endpoint that returns the current MQTT broker connection status, used by the /mqtt page JS to poll for real-time connection state updates.

- **Auth detail:** Requires authentication via WEB_AUTH_OR_RETURN macro (line 454), which returns ESP_OK (no response) if auth fails.
- **Parameters:** _none_
- **Request body:** none
- **Response:** Content-Type: application/json. JSON object with one field: {"connected": true|false}. The 'connected' field reflects the result of isMqttConnected() call (boolean indicating current MQTT broker connection state).
- **Observations:** Simple polling endpoint with minimal state. Response hardcoded as snprintf into a fixed-size 64-byte buffer. No input parameters; query string is ignored. isMqttConnected() is a backend function that queries the MQTT client library's connection state. Auto-polled every 5 seconds by /mqtt page JS (line 435 in WebPage_MQTT.cpp). If needed, could be extended to include additional status fields (bytes published, last publish time, error count, etc.), but currently minimal. No error details returned; only boolean.


<a id="group-games"></a>

### Games & DarkRoom

Games & DarkRoom provides web interface endpoints for launching game experiences on the ESP-IDF device. The functional area supports two mutually exclusive game builds: a Tilt Maze game (using IMU/gamepad input) via ENABLE_WEB_GAME_MAZE, or A Dark Room (a minimalist text adventure) via ENABLE_WEB_GAME_DARKROOM. Both game endpoints are gated behind session authentication and serve HTML content (either a launcher card within the site navigation shell or a full-screen game document). The DarkRoom build variant replaces the /games landing page with a launcher linking to /darkroom.

<a id="get-games"></a>

#### `GET /games`

**Handler:** `handleGamesPage` · **Source:** `components/hardwareone/WebPage_Games.cpp:20` · **Kind:** page · **Auth:** session · **Maturity:** adequate

Renders the games launcher page within the site shell. In ENABLE_WEB_GAME_MAZE builds, streams Tilt Maze game content; in ENABLE_WEB_GAME_DARKROOM builds, displays a launcher card linking to /darkroom.

- **Auth detail:** WEB_AUTH_OR_RETURN(req, ctx) at line 21 — redirects unauthenticated users to /login via sendAuthRequiredResponse.
- **Parameters:** _none_
- **Request body:** none
- **Response:** text/html; charset=utf-8 — Complete HTML page with site navigation, header, authenticated username, and game content card (either Tilt Maze or A Dark Room launcher, depending on build variant).
- **Observations:** Conditional compilation: only available if ENABLE_WEB_GAME_MAZE is 1. DarkRoom builds register handleGamesLauncherPage (in WebPage_DarkRoom.cpp line 29) for the same /games endpoint, replacing this handler. Both game implementations are raw-embedded and mutually exclusive (build error if both enabled). Handler chains through streamPageWithContent() → streamBeginHtml() + streamGamesInner() + streamEndHtml(); streamGamesInner() is defined in WebPage_Games.h and contains the generated game code. Calls streamPageWithContent(req, "games", ctx.user, streamGamesContent) which enforces mandatory password-change redirect before rendering.

<a id="get-darkroom"></a>

#### `GET /darkroom`

**Handler:** `handleDarkRoomPage` · **Source:** `components/hardwareone/WebPage_DarkRoom.cpp:37` · **Kind:** page · **Auth:** session · **Maturity:** mature

Serves the full-screen A Dark Room text adventure game. Delivered as raw HTML (no site shell) to preserve game's full-screen layout. Game state persists in browser localStorage; users can export/import saves for portability.

- **Auth detail:** WEB_AUTH_OR_RETURN(req, ctx) at line 38 — redirects unauthenticated users to /login via sendAuthRequiredResponse.
- **Parameters:** _none_
- **Request body:** none
- **Response:** text/html; charset=utf-8 with Cache-Control: public, max-age=86400 — Complete self-contained A Dark Room game document (no site shell/navigation). HTML5 page with embedded JavaScript, jQuery, game engine, and localStorage-backed save system. Game supports en, es, fr, zh_cn languages. Built from https://github.com/doublespeakgames/adarkroom pinned commit 1fada4620b6c66bd07bf15a3f1eb8223df8bc1d7 (Mozilla Public License 2.0).
- **Observations:** Only available in ENABLE_WEB_GAME_DARKROOM builds (mutually exclusive with ENABLE_WEB_GAME_MAZE). Auto-generated header from tools/build_darkroom_header.py; original game audio/dropbox/analytics features removed. Sets Cache-Control public header (86400s max-age) since game content is static per build. Handler directly calls streamDarkRoomDoc() from WebPage_DarkRoom.h (auto-generated, ~54KB); streamDarkRoomDoc sends chunked HTML via httpd_resp_send_chunk and expects caller to send final terminator chunk (done at line 42).


<a id="group-ml"></a>

### Edge Impulse / ML

Edge Impulse (ML) integration provides on-device TensorFlow Lite model inference for object detection and classification. This functional area enables loading TFLITE models, running inference on camera frames, organizing model files into directory hierarchies, and tracking detection state changes across frames. The system supports both FOMO grid-based detection and traditional classification models with configurable confidence thresholds and multi-detection limits. Both endpoints require authenticated user sessions.

<a id="post-api-ei-organize"></a>

#### `POST /api/ei/organize`

**Handler:** `handleEIOrganize` · **Source:** `components/hardwareone/System_EdgeImpulse.cpp:1920` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Reorganizes model files in /EI Models directory by moving root-level .tflite and .labels.txt files into subdirectory hierarchies (creates directory per model name, moves .tflite file into it, pairs with matching .labels.txt if present).

- **Auth detail:** WEB_AUTH_OR_RETURN macro at line 1921; redirects unauthenticated to /login
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json: {success: bool, error?: string, moved?: int, failed?: int}
- **Observations:** Endpoint is a maintenance/setup utility with no input parameters—it unconditionally organizes all models found at root level. Hardcoded directory paths (/EI Models). Error response envelopes inconsistent: filesystem-not-ready and models-dir-missing return JSON with only {success, error} fields, while success case includes {success, moved, failed}. No safety checks for concurrent file operations. Assumes filesystem is stable and writable.

<a id="get-api-edgeimpulse-detect"></a>

#### `GET /api/edgeimpulse/detect`

**Handler:** `handleEdgeImpulseDetect` · **Source:** `components/hardwareone/System_EdgeImpulse.cpp:2021` · **Kind:** api · **Auth:** session · **Maturity:** adequate

Runs a single inference pass on the currently loaded Edge Impulse TensorFlow Lite model using the live camera frame, returning object detections (if FOMO grid model) or classification confidence scores, along with tracked object state changes across frames.

- **Auth detail:** WEB_AUTH_OR_RETURN macro at line 2022; redirects unauthenticated to /login
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json: {success: bool, inferenceTimeMs: int, error?: string, detections: [{label: string, confidence: float, x: int, y: int, width: int, height: int}], trackedObjects: [{label: string, prevLabel: string, confidence: float, x: int, y: int, width: int, height: int, stateChanged: bool}]}
- **Observations:** Endpoint has no input parameters; inference configuration (model, confidence threshold, detection limits) comes from global settings (gSettings). Camera must be connected and running. Model must be pre-loaded via separate model-management CLI commands. Detection count is clamped by gSettings.edgeImpulseMaxDetections (default 5, range 1-10). State tracking uses sliding window (3 consecutive stable frames) before confirming state change. Response includes both raw detections and tracked-object state (useful for event-driven automations). Minimal error detail—returns generic 'error' strings without HTTP error codes (always 200 OK). No rate limiting or request timeout configuration.


<a id="group-migration"></a>

### Backup, Restore & Recovery (Migration Tool)

The Backup, Restore & Recovery (Migration Tool) functional area provides device backup export and cross-device restore capabilities during first-time setup. The backup export (/api/backup) is an authenticated admin-only endpoint that serializes device configuration (settings, users, automations, ESP-NOW registry, maps, and TLS certificates) into a portable HWBACKUP JSON file with encrypted sensitive fields. The restore import (/api/restore) is a triple-gated unauthenticated endpoint that accepts backups only during first-time setup's "Import from Backup" flow; it validates device fingerprint compatibility, preserves or skips credentials based on cross-device detection, and writes restored files to the filesystem. CORS headers are applied exclusively to these endpoints. In restore-only firmware builds (ENABLE_MIGRATION_TOOL=1, ENABLE_HTTP_SERVER=0), a lightweight headless recovery server spins up during setup, serving a plain-text splash page, connectivity ping (/api/ping), and restore endpoint. The whole subsystem gates itself at three levels (setup state, flag checks, dynamic handler registration) to prevent unauthorized restore outside the intended flow."

<a id="post-api-backup"></a>

#### `POST /api/backup`

**Handler:** `handleBackup` · **Source:** `components/hardwareone/WebServer_MigrationTool.cpp:198` · **Kind:** api · **Auth:** admin · **Maturity:** mature

Export a device backup containing configuration, user settings, automations, ESP-NOW device registry, maps, and TLS certificates. Only admins can export; exports are device-fingerprint-specific for security.

- **Auth detail:** Uses WEB_AUTH_OR_RETURN (lines 218-222), checks isAdminUser() at line 229. Redirects unauthenticated; returns 403 Forbidden if non-admin.
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `categories` | body-json | no | array of strings | Optional array of categories to include in backup. Supported values: 'settings', 'users', 'automations', 'espnow', 'maps', 'certs'. If empty or omitted, all categories are included. |

- **Request body:** application/json object with optional 'categories' array field, e.g. {"categories": ["settings", "users"]}
- **Response:** application/json: {"magic": "HWBACKUP", "formatVersion": 1, "timestamp": "<millis>", "device": {"hostname": "...", "mac": "...", "fingerprint": "...", "board": "...", "firmwareVersion": "...", "ip": "..."}, "files": {"<path>": "<content or JSON object>"}, "warnings": ["..."], "encryptedFields": ["<paths of AES-encrypted fields>"]}
- **Observations:** Certificate files (.pem, .crt, .key) are AES-encrypted during export and tracked in encryptedFields. JSON files are parsed and stored as objects; binary .hwmap files stored as raw strings. Guarded directory reads (VFS::openGuarded with canRead checks) mean non-admin exports would receive an incomplete bundle. Warnings array tracks skipped files (permissions, missing paths). Filesystem-ready check (line 237) returns early if FS not mounted. No request pagination or filtering.

<a id="post-api-restore"></a>

#### `POST /api/restore`

**Handler:** `handleRestore` · **Source:** `components/hardwareone/WebServer_MigrationTool.cpp:357` · **Kind:** api · **Auth:** special · **Maturity:** adequate

Import a backup file during first-time setup. Validates magic header, checks device fingerprint compatibility (skips user credentials and certs if different device). Writes all configuration files to filesystem. Sets gAcceptingRestore=false and gRestoreComplete=true on success. On cross-device restore, writes PENDING_CRED_SETUP_FILE to trigger streamlined login-only setup on next boot.

- **Auth detail:** NO auth check. Public endpoint but triple-gated: (1) only registered during 'Import from Backup' flow, (2) gFirstTimeSetupState == SETUP_IN_PROGRESS && gAcceptingRestore check at line 361, (3) unregistered after restore complete. Recovery-firmware-only path, self-contained from normal HTTP server.
- **Parameters:**

  | Name | In | Required | Type | Description |
  |------|----|----------|------|-------------|
  | `content_len` | header | yes | number | Content-Length header. Must be >0 and <=512KB (line 375). Enforced to prevent denial-of-service via unbounded allocations. |
  | `force` | body-json | no | boolean | If true, allows restore of backup from a different device (bypasses fingerprint check at line 442). Defaults to false. |

- **Request body:** application/json backup file (HWBACKUP format): {"magic": "HWBACKUP", "formatVersion": 1, "timestamp": "...", "device": {...}, "files": {...}, "force": false}
- **Response:** application/json: {"success": true, "compatible": true or false, "filesWritten": <int>, "filesErrored": <int>, "warnings": [...], "credentialsSkipped": true (if cross-device), "message": "..."}. On error: {"error": "<message>"} with appropriate HTTP status (400, 403, 409, 500).
- **Observations:** Cross-device restore (line 445) splits logic: compatible restores decrypt cert files and write everything; incompatible restores skip /system/users/* and encrypted certs, preserve THIS device's WiFi credentials (autoReconnect=true), and set credentialsSkipped flag. Payload size enforced (0 < len <= 512KB). Triple-gate means handler body is unreachable outside first-time-setup with Import selected — 403 Forbidden guards external callers. Warnings track failed writes and skipped files. JSON parse error at line 414 returns 400 with deserialize details. No field-level validation of backup contents (accepts any /path structure).

<a id="options-api-backup"></a>

#### `OPTIONS /api/backup`

**Handler:** `handleCorsOptions` · **Source:** `components/hardwareone/WebServer_MigrationTool.cpp:75` · **Kind:** options · **Auth:** public · **Maturity:** mature

Handle CORS preflight request for POST /api/backup. Allows cross-origin migration tool JavaScript to call backup export.

- **Auth detail:** Public CORS preflight. No auth check required per HTTP OPTIONS spec. Sets CORS headers unconditionally.
- **Parameters:** _none_
- **Request body:** none
- **Response:** 204 No Content (or empty 200). Response headers: Access-Control-Allow-Origin: *, Access-Control-Allow-Methods: GET, POST, OPTIONS, Access-Control-Allow-Headers: Content-Type, Authorization, Access-Control-Max-Age: 86400
- **Observations:** Shared handler (handleCorsOptions) used by all CORS preflight endpoints. Sets Access-Control-Max-Age to 86400 (1 day). CORS headers scoped to migration endpoints only (comment at lines 22-23).

<a id="options-api-restore"></a>

#### `OPTIONS /api/restore`

**Handler:** `handleCorsOptions` · **Source:** `components/hardwareone/WebServer_MigrationTool.cpp:75` · **Kind:** options · **Auth:** public · **Maturity:** mature

Handle CORS preflight request for POST /api/restore. Allows cross-origin migration tool JavaScript to call restore import.

- **Auth detail:** Public CORS preflight. No auth check required per HTTP OPTIONS spec. Sets CORS headers unconditionally.
- **Parameters:** _none_
- **Request body:** none
- **Response:** 204 No Content (or empty 200). Response headers: Access-Control-Allow-Origin: *, Access-Control-Allow-Methods: GET, POST, OPTIONS, Access-Control-Allow-Headers: Content-Type, Authorization, Access-Control-Max-Age: 86400
- **Observations:** Shared handler (handleCorsOptions). Note that actual /api/restore handler is NOT authenticated (triple-gate is at application logic level, not HTTP level), so CORS preflight succeeds regardless of restore-mode state.

<a id="options-api-ping"></a>

#### `OPTIONS /api/ping`

**Handler:** `handleCorsOptions` · **Source:** `components/hardwareone/WebServer_MigrationTool.cpp:75` · **Kind:** options · **Auth:** public · **Maturity:** mature

Handle CORS preflight request for GET /api/ping. Allows cross-origin migration tool to check device availability in restore mode.

- **Auth detail:** Public CORS preflight. No auth check. Used by migration tool to pre-check device availability and compatibility.
- **Parameters:** _none_
- **Request body:** none
- **Response:** 204 No Content (or empty 200). Response headers: Access-Control-Allow-Origin: *, Access-Control-Allow-Methods: GET, POST, OPTIONS, Access-Control-Allow-Headers: Content-Type, Authorization, Access-Control-Max-Age: 86400
- **Observations:** Registered both in main HTTP server (line 610-618) and restore-only server (line 765-770). Shared handler.

<a id="get"></a>

#### `GET /`

**Handler:** `handleRestoreSplash` · **Source:** `components/hardwareone/WebServer_MigrationTool.cpp:650` · **Kind:** page · **Auth:** public · **Maturity:** barebones

Display a plain-text splash page when user accesses the restore-only HTTP server root. Informs user that device is in restore mode and waiting for backup import.

- **Auth detail:** Public. No auth required. Only registered in restore-only HTTP server (startRestoreOnlyHttpServer, line 784), not in main web UI.
- **Parameters:** _none_
- **Request body:** none
- **Response:** text/plain; charset=utf-8. Plain-text human-readable splash page explaining restore mode and directing user to use Migration Tool on another device. Instructs user to press B on gamepad or use 'back' serial command to exit restore mode.
- **Observations:** Restore-only mode: this page is the ONLY content served at / in recovery firmware. No HTML, no JavaScript, no links — purely instructional text. References 'CadenGithubB/HardwareOne-Migration-Tool' repository. No dynamic content, no XSS risk.

<a id="get-api-ping"></a>

#### `GET /api/ping`

**Handler:** `handlePingRestore` · **Source:** `components/hardwareone/WebServer_MigrationTool.cpp:672` · **Kind:** api · **Auth:** public · **Maturity:** mature

Health-check endpoint that confirms device is reachable and in restore mode, and returns device fingerprint for compatibility pre-check by migration tool.

- **Auth detail:** Public. No auth required. Restore-mode connectivity test. Available in both main HTTP server (if registered for /api/ping) and restore-only server.
- **Parameters:** _none_
- **Request body:** none
- **Response:** application/json: {"ok": true, "mode": "restore", "fingerprint": "<device-fingerprint-string>"}
- **Observations:** Fingerprint is dynamically computed via getDeviceFingerprint() (line 677). Allows migration tool to pre-screen device compatibility before uploading a potentially large backup file. No parameters, no request body parsing. CORS headers set at line 673 via setCorsHeaders().


---

<a id="cross-cutting-observations"></a>

## Cross-Cutting Observations

A large (112-endpoint) organically-grown HTTP surface that mixes server-rendered HTML pages, JSON data APIs, SSE/MJPEG streams, binary asset handlers, and a separate restore-only server. It is feature-rich and mostly mature in the auth/session core (login, sessions, password-change, restore are well-considered), but the data-API layer is inconsistent: error envelopes, parameter parsing, pagination, and naming vary endpoint-by-endpoint, and a large fraction of "pages" are thin HTML shells that obtain data by scraping CLI text output through /api/cli rather than from dedicated JSON endpoints. There is no machine-readable index of the surface, several barebones/stale endpoints with fixed tiny buffers and silent truncation, and the dominant architectural debt is HTML/CLI coupling plus the absence of shared conventions across the JSON endpoints.

| Priority | Theme | Detail | Affected |
|----------|-------|--------|----------|
| high | Inconsistent error/success response envelopes | At least five distinct response shapes coexist with no shared contract: {success:bool,error:str} (e.g. /api/user/settings, /api/automations, /api/admin/approve), bare {error:...} (/api/maps/features, /api/espnow/remotemanifest, /api/bond/settings*), {found:false} (/api/espnow/metadata), {valid:false} (/api/espnow/remotecap), and {ok:true} (/api/llm/unload, /api/llm/stop). Several endpoints further decouple HTTP status from payload (e.g. /api/edgeimpulse/detect always returns 200 with a generic 'error' string), while others (/api/cli) do map status codes. The migration utility /api/ei/organize is internally inconsistent: failure returns {success,error} but success returns {success,moved,failed}. Consumers cannot write one generic error handler. | Nearly all /api/* JSON endpoints; acute in /api/maps/features, /api/espnow/metadata, /api/espnow/remotecap, /api/espnow/remotemanifest, /api/edgeimpulse/detect, /api/ei/organize, /api/llm/* (ok vs done vs error) |
| high | Data APIs coupled to HTML pages via CLI text scraping | Many feature pages have no dedicated JSON API and instead drive functionality by POSTing CLI commands to /api/cli (and /api/cli/batch) then regex/text-parsing the output client-side. /bluetooth, /speech, /logging, /mqtt, /automations, /maps, and /espnow all do this (e.g. bluetooth parses bleinfo/G2 status text; speech parses srstatus; logging regex-parses 'sensorlog status'). This makes the firmware's text output a de-facto API contract, breaks on output-format changes, and is rate-limited by the 50ms /api/cli throttle (the bluetooth page even maintains a client-side queue to cope). The catalog itself flags this as a refactor opportunity for /bluetooth (extract /api/bluetooth/status, /api/bluetooth/config). | /bluetooth, /speech, /logging, /mqtt, /automations, /maps, /espnow (all rely on /api/cli text parsing); contrast with the clean /api/mqtt/status which only returns a boolean |
| high | Inconsistent parameter parsing conventions | Body/query parsing is reinvented per-endpoint with three competing styles: httpd_query_key_value loops (/api/output/temp), ArduinoJson deserialize (/api/user/settings, /api/bond/cli/batch, /api/llm/load), and manual strstr/substring extraction (/api/bond/stream, /api/bond/exec, /api/waypoints). The manual parsers are fragile and at least one has a flagged bounds bug (/api/waypoints URL-decode removes 2 chars without bounds-checking i+2). Several POST bodies use fixed stack buffers that fail silently when exceeded (/api/llm/load 256B silent fail, readPostBody buffers). MAC-address URL-decoding (%3A) is hand-rolled separately in /api/sensors/remote, /api/espnow/messages, /api/espnow/metadata. | /api/output/temp, /api/bond/stream, /api/bond/exec, /api/waypoints, /api/llm/load, /api/sensors/remote, /api/espnow/messages, /api/espnow/metadata |
| high | No pagination/filtering standard; unbounded responses | List endpoints uniformly return entire collections with explicit 'no pagination' notes, and several serialize untrusted/large data into fixed buffers that silently truncate. /api/maps/features can dump thousands of place-names; /api/system and /api/events serializeJson into a 4KB buffer that silently truncates to invalid JSON; /api/battery/status and /api/llm/status use 512B/448B fixed buffers that truncate; /api/files/list, /api/recordings, /api/videos, /api/automations, /api/waypoints, /api/bond/paired-devices, /api/cli/logs all return full sets. Only /api/espnow/messages (seq cursor) and /api/bond/fs/list (start/hasMore) implement pagination, and they do it differently from each other. | /api/system, /api/events, /api/maps/features, /api/battery/status, /api/llm/status, /api/files/list, /api/recordings, /api/videos, /api/automations, /api/waypoints, /api/bond/paired-devices, /api/cli/logs |
| high | Silent truncation / silent failure as a recurring failure mode | Multiple endpoints share a pattern where serializeJson into an undersized fixed buffer silently produces truncated/invalid JSON or empty {} rather than signalling overflow: /api/system & /api/events (4KB, flagged as potential dashboard-breaking bug), /api/battery/status (512B -> {} on OOM, no error), /api/llm/status (448B), /api/devices (4KB -> {} on alloc fail), /api/buildconfig (512B). There is no shared 'response too large' signal, so growth in device/sensor/peer counts degrades silently. | /api/system, /api/events, /api/battery/status, /api/llm/status, /api/devices, /api/buildconfig |
| medium | No machine-readable API index / discovery surface | There is no endpoint that enumerates the available routes, methods, or response schemas. /api/buildconfig advertises compiled features and /api/settings/schema describes settings fields, but neither maps to the HTTP surface. With 112 endpoints behind compile-time flags (ENABLE_*), there is no runtime way to know which endpoints actually exist in a given build, forcing pages to assume-and-handle 501/503. A generated route manifest would also catch duplications and naming drift. | Whole surface; especially endpoints gated on ENABLE_* flags (sensors, camera, LLM, espnow, games, speech) |
| medium | Duplicate and overloaded routes | /api/sessions and /api/admin/sessions are byte-identical (both call buildAllSessionsJson) with no differentiation. /api/ping and OPTIONS /api/ping and GET / are registered in both the main server and the restore-only server with different bodies. Several GET endpoints are heavily action-overloaded via query params instead of distinct resources: /api/sensors?sensor=... multiplexes ~10 sensor schemas through one handler with no validation on the sensor param; /api/gps/tracks overloads live/save/load/list into one endpoint returning three different top-level shapes ({live,...} vs {success,validation,...} vs {success,files}); POST /api/waypoints multiplexes add/delete/goto/clear. | /api/sessions vs /api/admin/sessions; /api/ping (dual registration); /api/sensors (multiplexed); /api/gps/tracks (3 response shapes); POST /api/waypoints |
| medium | Naming convention drift across endpoint groups | The same subsystem is spelled differently across paths: /api/ei/organize vs /api/edgeimpulse/detect (both Edge Impulse). ESP-NOW uses /api/espnow/* but also /api/sensors/remote for ESP-NOW sensor data. Bond filesystem uses /api/bond/fs/stat while local stats use /api/files/stats (fs vs files, stat vs stats). Settings split across /api/settings, /api/settings/schema, /api/user/settings with no consistent ordering. There is no documented prefix taxonomy (resource-first vs feature-first). | /api/ei/organize, /api/edgeimpulse/detect, /api/sensors/remote, /api/bond/fs/stat vs /api/files/stats, /api/settings* vs /api/user/settings |
| medium | Auth/authorization inconsistencies | Authorization granularity is uneven and partly implicit. /api/files/stats and /api/files/list expose per-path stats/listings to any authenticated user with the catalog noting no per-path auth gate on stats. Several bond endpoints enforce master-only only *implicitly* via downstream BondedPeer calls rather than an explicit check (/api/bond/settings/sync, /api/bond/fs/*), making the auth contract non-obvious. /api/restore is intentionally unauthenticated at the HTTP layer with auth enforced only by an application-level triple-gate. /espnow has full control UI with no admin check while sibling admin actions require admin. Auth checks are sometimes inline at specific line numbers (camera frame/stream) rather than via the uniform WEB_AUTH_OR_RETURN macro. | /api/files/stats, /api/files/list, /api/bond/settings/sync, /api/bond/fs/list, /api/bond/fs/stat, /api/bond/fs/get, /api/restore, /espnow |
| low | No API versioning | None of the /api/* routes carry a version segment, yet response shapes are already migrating (LLM 'suppress' legacy field deprecated in favor of /api/llm/chat/retry; automations 'v1 schema normalization' done in JS; bond settings shape mirrors /api/settings deliberately). Schema evolution currently happens by silent in-place change, which is exactly what couples pages tightly to firmware versions. | All /api/* endpoints; evolution pressure visible in /api/llm/generate, /api/llm/chat/retry, /api/automations |

<a id="improvement-candidates"></a>

## Improvement Candidates

Prioritized opportunities (directional — not implementation plans).

| Priority | Target | Issue | Suggested Direction |
|----------|--------|-------|---------------------|
| high | /bluetooth, /speech, /logging, /mqtt groups | These pages have no dedicated data API and reconstruct state by POSTing CLI commands to /api/cli and text/regex-parsing the output, throttled by the 50ms rate limiter and brittle to any CLI format change. /mqtt only has the minimal /api/mqtt/status (a single boolean). | Promote the read paths to dedicated JSON status/config endpoints (e.g. /api/bluetooth/status, /api/speech/status, /api/logging/status, expand /api/mqtt/status) backed by the same shared builders the CLI uses, so the page consumes structured JSON instead of scraping text. Treat CLI text output as human-facing, not as an API. |
| high | GET /api/maps/features | Barebones: manual string-concatenation JSON with hand-rolled quote-escaping (only replaces "), no escaping of other control chars, no pagination for potentially thousands of place-names from untrusted map binary -> oversized/invalid response risk. | Rebuild on a real JSON serializer with full escaping, and add cursor/offset pagination (or a separate names endpoint) so large name tables don't serialize into one response. Align the no-map case to the standard error envelope. |
| high | GET /api/system and GET /api/events | Both serializeJson into a shared 4KB buffer that silently truncates to invalid JSON once device/sensor lists grow; /api/events is the dashboard's live feed so truncation breaks the primary page silently. | Detect overflow and either grow the buffer (PSRAM) or signal an explicit error/partial flag; bound or paginate the device list; never emit silently-truncated JSON. Consider splitting the heavy device inventory out of the hot polling payload. |
| high | POST /api/waypoints and POST /api/bond/stream, POST /api/bond/exec | Manual strstr/substring body parsing; /api/waypoints has a flagged URL-decode bounds bug (removes 2 chars after hex decode without checking i+2), no lat/lon/name validation, two-phase add+notes with silent partial-failure risk; bond endpoints parse form bodies by hand which the catalog calls fragile. | Move all body parsing onto a single shared parser (httpd_query_key_value or ArduinoJson) used everywhere, fix the bounds bug, add explicit field validation, and make multi-step mutations report partial success in the response. |
| medium | GET /api/gps/tracks | Single endpoint overloads live/save/load/list and returns three different top-level JSON structures depending on the action; streaming with no content-length/chunked marker; 500-point cap only in live mode. | Split into distinct resources (e.g. /api/gps/tracks for list, /api/gps/track/live, /api/gps/track for load/save) or at minimum return one consistent envelope across actions; apply a uniform point-pagination strategy across live and file modes. |
| medium | GET /api/sensors (multiplexed sensor=...) | One handler multiplexes ~10 distinct sensor schemas with no validation on the sensor parameter (invalid type returns a generic error), making per-sensor schema discovery impossible. | Validate the sensor param against a known list with a clear error, and expose the available sensor types (and ideally their schemas) via the sensor-status endpoint so the multiplex is discoverable; keep the multiplex but make it contract-checked. |
| medium | /api/ei/organize and /api/edgeimpulse/detect | Naming drift (ei vs edgeimpulse for the same subsystem); /api/ei/organize has inconsistent envelopes (failure {success,error} vs success {success,moved,failed}); /api/edgeimpulse/detect always returns HTTP 200 even on error with no codes. | Unify under one prefix (e.g. /api/edgeimpulse/*), standardize the envelope across success/failure, and map real failures to HTTP status codes. |
| medium | GET /api/battery/status, GET /api/llm/status, GET /api/devices, GET /api/buildconfig | Fixed tiny buffers (512B/448B/4KB) with silent truncation or empty {} on OOM and no error indication; polymorphic battery schema with no field to indicate which backend/fields are present. | Adopt a shared response-buffer helper that detects overflow and signals it; for battery, add an explicit backend/capability field so the polymorphic shape is self-describing rather than implicit. |
| medium | GET /api/espnow/remotemanifest and GET /api/espnow/metadata, GET /api/espnow/remotecap | Barebones, each with a different ad-hoc not-found/empty shape ({error}, {found:false}, {valid:false}); manifest depends on filesystem readiness with stale-flag note; no pagination on manifest list. | Standardize the not-found/empty responses onto the common envelope, add a list/pagination contract for cached manifests, and document the freshness/age semantics consistently (some already expose ageMs; apply uniformly). |
| medium | All bond master-only endpoints (/api/bond/settings/sync, /api/bond/fs/list, /api/bond/fs/stat, /api/bond/fs/get, /api/bond/cli/batch) | Master-only authorization is enforced only implicitly via downstream BondedPeer calls; the HTTP contract does not state or check the precondition, so non-master calls fail with delegated/unclear errors. | Add an explicit, uniform isBondMaster()/isBondSynced() guard at handler entry returning a standard error envelope, so the precondition is visible and consistent across the whole bond group. |
| low | /api/sessions and /api/admin/sessions | Byte-identical duplicate endpoints (both call buildAllSessionsJson) with no functional difference. | Collapse to one canonical route (keep /api/admin/sessions for the admin namespace) and redirect/alias the other, or delete the redundant registration to reduce surface. |
| low | Cross-server /api/ping and the icon/debug pages (/icons/test, /api/icon) | /api/ping is dual-registered with divergent bodies in main vs restore servers; /api/icon does arbitrary unvalidated name lookup; /icons/test is a barebones debug page that inlines a potentially large HTML string with no pagination. | Factor a single ping builder shared by both servers (vary only the restore flag), add minimal name validation/whitelisting to /api/icon, and gate or paginate the icon-test debug page (or mark it admin/debug-only) to keep it out of the production surface. |


---

<a id="modernization-backlog"></a>

## Modernization Backlog

Checklist for the API improvement project. Phase 0 establishes shared conventions; the
per-endpoint phases below depend on them. Priorities mirror the [Cross-Cutting
Observations](#cross-cutting-observations) and [Improvement Candidates](#improvement-candidates)
tables. Tick items as they land.

### Phase 0 — Foundations (do first; the rest build on these)

- [ ] **Standard response envelope** — define one success/error JSON contract and apply it everywhere. Replaces the 5+ ad-hoc shapes (`{success,error}`, bare `{error}`, `{found:false}`, `{valid:false}`, `{ok:true}`). _Affects: nearly all `/api/*`._
- [ ] **Overflow-safe serialization helper** — shared builder that detects buffer overflow and signals it (or grows into PSRAM) instead of emitting truncated/empty JSON. _Affects: `/api/system`, `/api/events`, `/api/devices`, `/api/battery/status`, `/api/llm/status`, `/api/buildconfig`._
- [ ] **Pagination convention** — one cursor/offset scheme for all list endpoints (today only `/api/espnow/messages` and `/api/bond/fs/list` paginate, differently). _Affects: `/api/files/list`, `/api/recordings`, `/api/videos`, `/api/automations`, `/api/waypoints`, `/api/bond/paired-devices`, `/api/cli/logs`, `/api/maps/features`._
- [ ] **Unified request parser** — single ArduinoJson / `httpd_query_key_value` helper for query + body across all handlers; retire hand-rolled `strstr`/substring parsers and the duplicated `%3A` MAC decoders. **Fix the `/api/waypoints` URL-decode bounds bug (removes 2 chars without checking `i+2`).**
- [ ] **Route-discovery index** — a `/api/routes` (or similar) endpoint enumerating live routes/methods, honoring `ENABLE_*` compile flags so consumers know what exists in a given build.
- [ ] **Naming taxonomy** — document a prefix/resource convention and resolve drift: `ei` vs `edgeimpulse`, `fs` vs `files`, `stat` vs `stats`, `/api/sensors/remote` (ESP-NOW data under sensors).
- [ ] **Auth-guard consistency** — explicit, uniform guards at handler entry (admin, bond-master, restore-mode) returning the standard envelope, instead of implicit downstream/inline checks.
- [ ] **API versioning decision** — choose `/api/v1` prefix or a version header before shapes drift further (LLM/automations/bond schemas are already migrating in place).

### Phase 1 — High priority

- [ ] **De-couple pages from CLI text scraping** — give the CLI-scraped pages real JSON read endpoints backed by the same builders the CLI uses (treat CLI text as human-facing only). _Targets: `/bluetooth`, `/speech`, `/logging`, `/mqtt` (then `/automations`, `/maps`, `/espnow`)._
- [ ] **Rebuild `GET /api/maps/features`** — real JSON serializer with full escaping + pagination for large place-name tables; standard error envelope for the no-map case.
- [ ] **Fix `/api/system` + `/api/events` silent truncation** — overflow detection / partial flag + bounded device list; `/api/events` is the dashboard's live feed, so truncation breaks the primary page silently.
- [ ] **Harden mutation bodies** — `POST /api/waypoints`, `POST /api/bond/stream`, `POST /api/bond/exec`: move onto the shared parser, add field validation, and report partial success for multi-step mutations.

### Phase 2 — Medium priority

- [ ] **Split/normalize `GET /api/gps/tracks`** — currently overloads live/save/load/list into one route with three top-level shapes; split into distinct resources or unify the envelope; consistent point-pagination.
- [ ] **Contract-check `GET /api/sensors?sensor=`** — validate the `sensor` param against a known list with a clear error, and expose available sensor types/schemas for discoverability.
- [ ] **Unify Edge Impulse** — one prefix (`/api/edgeimpulse/*`), consistent success/failure envelope, real HTTP status codes (`/api/edgeimpulse/detect` always returns 200 today).
- [ ] **Self-describing + overflow-safe** — `/api/battery/status`, `/api/llm/status`, `/api/devices`, `/api/buildconfig`: shared overflow helper; add a backend/capability field so polymorphic battery schema is explicit.
- [ ] **Standardize ESP-NOW responses** — common not-found/empty shape across `/api/espnow/remotemanifest`, `/metadata`, `/remotecap`; add manifest list pagination; uniform freshness/age semantics.
- [ ] **Explicit bond master-only guards** — `/api/bond/settings/sync`, `/api/bond/fs/{list,stat,get}`, `/api/bond/cli/batch`: uniform `isBondMaster()`/`isBondSynced()` guard at entry instead of implicit downstream failure.

### Phase 3 — Low priority

- [ ] **Collapse duplicate session routes** — `/api/sessions` and `/api/admin/sessions` are byte-identical; keep one canonical (admin namespace) and alias/remove the other.
- [ ] **Factor shared `/api/ping` builder** — dual-registered with divergent bodies in main vs restore servers; share one builder varying only the restore flag.
- [ ] **Tidy icon/debug surface** — add name validation/whitelisting to `/api/icon`; gate or paginate the `/icons/test` debug page (mark admin/debug-only).
- [ ] **API versioning rollout** — if deferred from Phase 0, execute the version migration here.
