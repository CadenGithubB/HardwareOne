#ifndef G2_RE_REFERENCE_H
#define G2_RE_REFERENCE_H

// =============================================================================
// G2 Protocol — FlutterApp RE Reference Header
// =============================================================================
//
// **THIS IS NOT FIRMWARE — IT'S A DOCUMENTATION HEADER.**
//
// All constants here are derived from `docs/FlutterApp-main/`, a community
// reverse-engineering project. They are NOT from the official Even Realities
// app source. Trust hierarchy:
//
//   1. Empirical firmware observations  (System_G2_Protocol.h comments,
//                                        capture logs, on-device tests)
//   2. base64 FileDescriptorProto bytes  (in *.pbjson.dart — wire format
//                                        likely accurate)
//   3. RE-inferred names                 (in *.pb.dart, *.pbenum.dart —
//                                        could be guesses)
//   4. R1 framing                        (RE authors flag as "best effort")
//
// All identifiers are prefixed `G2RE_` so they cannot collide with the
// firmware's existing `G2_SID_*`, `G2_CMD_*`, etc. Use this header for
// grep-ability against FlutterApp/RE traces, capture logs, and external
// docs that use these names.
//
// To use as documentation only: do NOT #include in active firmware paths.
// To use as a reference table at runtime (e.g. naming SIDs in a debug
// dump): you may #include it; nothing here will be auto-linked.
//
// Header is self-contained — no includes needed.
// =============================================================================

// =============================================================================
// SECTION 1: Service IDs (envelope byte 6)
// =============================================================================
// Firmware existing constants live in System_G2_Protocol.h:73-114 with
// names chosen empirically (e.g. G2_SID_HEARTBEAT for what FlutterApp calls
// UX_DEVICE_SETTINGS_APP_ID). Cross-reference the FW column:
//
//   FW = G2_SID_*  if firmware has a #define for this value
//   ?? = the firmware does NOT have this SID named at all

// ----- Foreground UI app IDs (0..16) -----------------------------------------
#define G2RE_SID_UI_DEFAULT_APP_ID                   0   // FW: ??
#define G2RE_SID_UI_BACKGROUND_DASHBOARD_APP_ID      1   // FW: G2_SID_APP_LAUNCH
                                                          //     (firmware uses
                                                          //      this as the
                                                          //      app-launch
                                                          //      prelude SID;
                                                          //      same value)
// 2 = reserved/unused
#define G2RE_SID_UI_FOREGROUND_MEUN_ID               3   // FW: ?? [sic MEUN]
#define G2RE_SID_UI_FOREGROUND_NOTIFICATION_ID       4   // FW: ??
#define G2RE_SID_UI_TRANSLATE_APP_ID                 5   // FW: G2_SID_TRANSLATE
#define G2RE_SID_UI_TELEPROMPT_APP_ID                6   // FW: G2_SID_TELEPROMPT
#define G2RE_SID_UI_FOREGROUND_EVEN_AI_ID            7   // FW: G2_SID_EVEN_AI
#define G2RE_SID_UI_BACKGROUND_NAVIGATION_ID         8   // FW: ??
#define G2RE_SID_UI_SETTING_APP_ID                   9   // FW: G2_SID_SETTINGS
#define G2RE_SID_UI_TRANSCRIBE_APP_ID                10  // FW: G2_SID_TRANSCRIBE
#define G2RE_SID_UI_CONVERSATE_APP_ID                11  // FW: G2_SID_CONVERSATE
#define G2RE_SID_UI_QUICKLIST_APP_ID                 12  // FW: ??
#define G2RE_SID_SERVICE_SYNC_INFO_APP_ID            13  // FW: G2_SID_STATE_EVENT
                                                          //     (FW name reflects
                                                          //      observed use as
                                                          //      gesture/event
                                                          //      channel)
#define G2RE_SID_UI_HEALTH_APP_ID                    14  // FW: G2_SID_WIDGET_XFORM
                                                          //     [DISAGREE]
                                                          //     RE infers HEALTH
                                                          //     from name; FW
                                                          //     observes widget
                                                          //     transform shape.
                                                          //     **Trust FW empirical.**
#define G2RE_SID_UI_LOGGER_APP_ID                    15  // FW: ??
#define G2RE_SID_UI_ONBOARDING_APP_ID                16  // FW: ??

// ----- Service / system IDs (32..34) -----------------------------------------
#define G2RE_SID_SERVICE_MODULE_CONFIGURE_APP_ID     32  // FW: ??
#define G2RE_SID_UI_FOREGROUND_SYSTEM_ALERT_APP_ID   33  // FW: ??
#define G2RE_SID_UI_FOREGROUND_SYSTEM_CLOSE_APP_ID   34  // FW: ??

// ----- UX (high) range (128..) -----------------------------------------------
#define G2RE_SID_UX_DEVICE_SETTINGS_APP_ID           128 // FW: G2_SID_HEARTBEAT
                                                          //     [DISAGREE]
                                                          //     RE: DevCfgDataPackage
                                                          //     (auth, time, ring info,
                                                          //     heartbeat).
                                                          //     FW: heartbeat-style
                                                          //     pings observed.
                                                          //     ** OBSERVED RX:
                                                          //     `08 06 10 ?? 2A 00`
                                                          //     fits RE's
                                                          //     RING_CONNECT_INFO {}.
                                                          //     Plausible RE is right
                                                          //     and FW name is wrong;
                                                          //     keep brick blocklist
                                                          //     until verified. **
#define G2RE_SID_UX_GLASSES_CASE_APP_ID              129 // FW: ??
#define G2RE_SID_UX_RING_ROW_DATA_ID                 144 // FW: ??
#define G2RE_SID_UX_RING_DATA_RELAY_ID               145 // FW: ??
                                                          //     (ring → glasses
                                                          //      bridge events
                                                          //      land here)

// ----- OTA (192..195) --------------------------------------------------------
#define G2RE_SID_UX_OTA_TRANSMIT_CMD_ID              192 // FW: ?? (transport-only)
#define G2RE_SID_UX_OTA_TRANSMIT_RAW_DATA_ID         193 // FW: ??
#define G2RE_SID_UX_OTA_EXPORT_FILE_CMD_ID           194 // FW: ??
#define G2RE_SID_UX_OTA_EXPORT_FILE_RAW_DATA_ID      195 // FW: ??

// ----- File service (196..199) -----------------------------------------------
#define G2RE_SID_UX_EVEN_FILE_SERVICE_CMD_SEND_ID         196 // FW: G2_SID_FILE_CMD
#define G2RE_SID_UX_EVEN_FILE_SERVICE_RAW_SEND_DATA_ID    197 // FW: G2_SID_FILE_RAW
#define G2RE_SID_UX_EVEN_FILE_SERVICE_CMD_EXPORT_ID       198 // FW: ??
#define G2RE_SID_UX_EVEN_FILE_SERVICE_RAW_EXPORT_DATA_ID  199 // FW: ??

// ----- Background EvenHub (224) ----------------------------------------------
#define G2RE_SID_UI_BACKGROUND_EVENHUB_APP_ID        224 // FW: G2_SID_EVEN_CORE
                                                          //     (firmware names it
                                                          //      EvenCore to avoid
                                                          //      Even's "EvenHub"
                                                          //      SDK confusion)
#define G2RE_SID_INVALID_SERVICE_ID                  255

// =============================================================================
// SECTION 2: DevConfig opcodes — sid=128 payload (DevCfgDataPackage.cmd)
// =============================================================================
// **WARNING**: firmware blocks sid=0x80 (= 128) in g2probe via the brick
// blocklist (G2_Glasses.cpp:9105+). Even with a well-formed payload, do
// not send these without a unit you can physically reset.
//
// FW status: NOT IMPLEMENTED. Firmware skips DevConfig entirely.
// =============================================================================
#define G2RE_DEVCFG_CMD_NONE_COMMAND                 0
#define G2RE_DEVCFG_CMD_CONNECT_PRO                  1
#define G2RE_DEVCFG_CMD_SERVICE_PRO                  2
#define G2RE_DEVCFG_CMD_COMMAND_PRO                  3
#define G2RE_DEVCFG_CMD_AUTHENTICATION               4   // AuthMgr (just secAuth bool;
                                                          // no real crypto)
#define G2RE_DEVCFG_CMD_PIPE_ROLE_CHANGE             5   // PipeRoleChange (eGlassesLR)
#define G2RE_DEVCFG_CMD_RING_CONNECT_INFO            6   // RingInfo (mac, name)
                                                          // *** This is the bridge
                                                          // trigger — see
                                                          // Ring_Bridge_Sequence.h.
                                                          // RingInfo.ringMac MUST
                                                          // be byte-REVERSED from
                                                          // BLE address order.
                                                          // RingInfo.ringName is
                                                          // raw UTF-8 bytes, NOT
                                                          // null-terminated. ***
#define G2RE_DEVCFG_CMD_BLE_CONNECT_PARAM            7   // BleConnectParam (MTU,
                                                          // conn interval)
#define G2RE_DEVCFG_CMD_DISCONNECT_INFO              8
#define G2RE_DEVCFG_CMD_UNPAIR_INFO                  9
#define G2RE_DEVCFG_CMD_COMMAND_EXCEPTION            10
#define G2RE_DEVCFG_CMD_SET_DEVICE_INFO              11
#define G2RE_DEVCFG_CMD_GET_DEVICE_INFO              12
#define G2RE_DEVCFG_CMD_RESTORE_TO_FACTORY_SETTINGS  13  // *** dangerous ***
#define G2RE_DEVCFG_CMD_BASE_CONNECT_HEART_BEAT      14  // empty body; 30 s cadence
                                                          // in FlutterApp
#define G2RE_DEVCFG_CMD_QUICK_RESTART                15
#define G2RE_DEVCFG_CMD_TIME_SYNC                    128 // TimeSync (s32 unix +
                                                          // i64 quarter-hour offset)
                                                          // *** TIMEZONE GOTCHA:
                                                          // G2 uses QUARTER-HOURS
                                                          // (minutes/15). R1 uses
                                                          // RAW MINUTES for the
                                                          // same field. See
                                                          // R1_RE_Reference.h
                                                          // SECTION 4 systemTime. ***
#define G2RE_DEVCFG_CMD_AUD_CONTROL                  129 // AudControl (open/close
                                                          // mic stream — different
                                                          // SID/cmd than firmware's
                                                          // EvenCore audio path)
#define G2RE_DEVCFG_CMD_COMMAND_ERROR                255

// =============================================================================
// SECTION 3: EvenAI commands — sid=7 payload (EvenAIDataPackage.commandId)
// =============================================================================
// Firmware coverage in System_G2_Protocol.h:169-173. FW handles 5 of these;
// the rest are device-initiated and may be arriving silently. If you start
// seeing unknown EvenAI subCmd logs, the missing ones below are candidates.
// =============================================================================
#define G2RE_AI_CMD_NONE_COMMAND   0
#define G2RE_AI_CMD_CTRL           1   // FW: G2_AI_CMD_CTRL
#define G2RE_AI_CMD_VAD_INFO       2   // FW: NOT IMPLEMENTED — voice activity
                                       //     start/end/timeout
#define G2RE_AI_CMD_ASK            3   // FW: G2_AI_CMD_ASK
#define G2RE_AI_CMD_ANALYSE        4   // FW: G2_AI_CMD_ANALYSE
#define G2RE_AI_CMD_REPLY          5   // FW: G2_AI_CMD_REPLY
#define G2RE_AI_CMD_SKILL          6   // FW: NOT IMPLEMENTED — device asks host
                                       //     to invoke a skill (BRIGHTNESS,
                                       //     TRANSLATE_CTRL, NOTIFICATION,
                                       //     TELEPROMPT, NAVIGATE, CONVERSATE,
                                       //     QUICKLIST, AUTO_BRIGHTNESS)
#define G2RE_AI_CMD_PROMPT         7   // FW: NOT IMPLEMENTED — device asks host
                                       //     to display an error prompt
                                       //     (NETWORK_ERR, SERVER_ERR, etc.)
#define G2RE_AI_CMD_EVENT          8   // FW: NOT IMPLEMENTED — device → host
                                       //     SCROLL or STREAM_COMPLETE
#define G2RE_AI_CMD_HEARTBEAT      9   // FW: G2_AI_CMD_HEARTBEAT
#define G2RE_AI_CMD_CONFIG         10  // FW: NOT IMPLEMENTED — voice/stream-speed
                                       //     config
#define G2RE_AI_CMD_COMM_RSP       161 // FW: NOT IMPLEMENTED — generic ack

// =============================================================================
// SECTION 4: EvenHub commands — sid=224 payload (EvenHubDataPackage.commandId)
// =============================================================================
// Firmware coverage in System_G2_Protocol.h:125-133. FW has the request side
// of the table. The OS_RESPONSE_* values complete the round-trip pairs and
// the IMU pair (19/20) is genuinely new — phone can REQUEST IMU data.
// =============================================================================
#define G2RE_HUB_CMD_APP_REQUEST_CREATE_STARTUP_PAGE_PACKET   0   // FW: G2_CMD_CREATE_STARTUP
#define G2RE_HUB_CMD_OS_RESPONSE_CREATE_STARTUP_PAGE_PACKET   1   // FW: response side handled
#define G2RE_HUB_CMD_OS_NOITY_EVENT_TO_APP_PACKET             2   // FW: state events handled
                                                                   //     (RX of click/scroll)
                                                                   //     [sic NOITY in RE]
#define G2RE_HUB_CMD_APP_UPDATE_IMAGE_RAW_DATA_PACKET         3   // FW: G2_CMD_IMAGE_RAW_DATA
#define G2RE_HUB_CMD_OS_RESPONSE_IMAGE_RAW_DATA_PACKET        4   // FW: image-ack handled
#define G2RE_HUB_CMD_APP_UPDATE_TEXT_DATA_PACKET              5   // FW: G2_CMD_UPDATE_TEXT
#define G2RE_HUB_CMD_OS_RESPONSE_TEXT_DATA_PACKET             6   // FW: text-ack handled
#define G2RE_HUB_CMD_APP_REQUEST_REBUILD_PAGE_PACKET          7   // FW: G2_CMD_REBUILD_PAGE
#define G2RE_HUB_CMD_OS_RESPONSE_REBUILD_PAGE_PACKET          8   // FW: rebuild-ack handled
#define G2RE_HUB_CMD_APP_REQUEST_SHUTDOWN_PAGE_PACKET         9   // FW: G2_CMD_SHUTDOWN_PAGE
#define G2RE_HUB_CMD_OS_RESPONSE_SHUTDOWN_PAGE_PACKET         10  // FW: shutdown-ack handled
#define G2RE_HUB_CMD_OS_PRIVATE_EVENT_PACKET                  11  // FW: NOT IMPLEMENTED
                                                                   //     (device-initiated
                                                                   //      private event)
#define G2RE_HUB_CMD_APP_REQUEST_HEARTBEAT_PACKET             12  // FW: G2_CMD_HEARTBEAT
#define G2RE_HUB_CMD_OS_RESPONSE_HEARTBEAT_PACKET             13  // FW: heartbeat-ack handled
#define G2RE_HUB_CMD_OS_PRIVATE_SYSTEM_EVENT_PACKET           14  // FW: NOT IMPLEMENTED
                                                                   //     (system event from
                                                                   //      device, e.g. timeout
                                                                   //      page enter/exit)
#define G2RE_HUB_CMD_APP_REQUEST_AUDIO_CTR_PACKET             15  // FW: G2_CMD_AUDIO_CTRL
                                                                   //     [sic CTR in RE]
#define G2RE_HUB_CMD_OS_RESPONSE_AUDIO_CTR_PACKET             16  // FW: audio-ack handled
#define G2RE_HUB_CMD_OS_NOTIFY_MENU_STARTUP_PACKET            17  // FW: G2_CMD_MENU_STARTUP
#define G2RE_HUB_CMD_APP_RESPONSE_MENU_STARTUP_FAILED_PACKET  18  // FW: G2_CMD_MENU_FAILED
#define G2RE_HUB_CMD_APP_REQUEST_OPEN_IMU_PACKET              19  // FW: NOT IMPLEMENTED
                                                                   //     ** new feature: phone
                                                                   //     can request IMU stream
                                                                   //     from glasses **
#define G2RE_HUB_CMD_OS_RESPONSE_IMU_PACKET                   20  // FW: NOT IMPLEMENTED
                                                                   //     (IMU samples from
                                                                   //      glasses; arrive
                                                                   //      after OPEN_IMU req)

// =============================================================================
// SECTION 5: EvenHub on-lens user gestures (OsEventTypeList)
// =============================================================================
// Body of OS_NOITY_EVENT_TO_APP_PACKET (cmd=2 above). Firmware routes these
// via its sid=0x0D handler (state event channel). Names below are useful for
// dumping the event type in logs.
// =============================================================================
#define G2RE_HUB_EVT_CLICK_EVENT             0
#define G2RE_HUB_EVT_SCROLL_TOP_EVENT        1
#define G2RE_HUB_EVT_SCROLL_BOTTOM_EVENT     2
#define G2RE_HUB_EVT_DOUBLE_CLICK_EVENT      3
#define G2RE_HUB_EVT_FOREGROUND_ENTER_EVENT  4
#define G2RE_HUB_EVT_FOREGROUND_EXIT_EVENT   5
#define G2RE_HUB_EVT_ABNORMAL_EXIT_EVENT     6
#define G2RE_HUB_EVT_SYSTEM_EXIT_EVENT       7
#define G2RE_HUB_EVT_IMU_DATA_REPORT         8

// EventSourceType — where the touch came from. Useful when the ring bridge
// is up: gestures from the ring arrive labelled TOUCH_EVENT_FROM_RING.
#define G2RE_HUB_SRC_TOUCH_EVENT_FORM_DUMMY_NULL  0  // [sic FORM]
#define G2RE_HUB_SRC_TOUCH_EVENT_FROM_GLASSES_R   1
#define G2RE_HUB_SRC_TOUCH_EVENT_FROM_RING        2
#define G2RE_HUB_SRC_TOUCH_EVENT_FROM_GLASSES_L   3

// =============================================================================
// SECTION 6: Even File Service — sid=196..199
// =============================================================================
// File transfer subsystem used for Android JSON notifications.
// =============================================================================
#define G2RE_EFS_TYPE_NOTIFICATION_JSON_WHITELIST    0
#define G2RE_EFS_TYPE_ANDROID_MSG_JSON_NOTIFICATION  1
#define G2RE_EFS_TYPE_OTHER_FILE                     170

#define G2RE_EFS_SEND_CMD_START                      0  // metadata + total CRC32
#define G2RE_EFS_SEND_CMD_DATA                       1  // ack of raw chunks
#define G2RE_EFS_SEND_CMD_RESULT_CHECK               2  // final verify

#define G2RE_EFS_EXPORT_CMD_START                    0
#define G2RE_EFS_EXPORT_CMD_DATA                     1
#define G2RE_EFS_EXPORT_CMD_RESULT_CHECK             2

#define G2RE_EFS_RSP_SUCCESS                         0
#define G2RE_EFS_RSP_START_ERR                       1
#define G2RE_EFS_RSP_DATA_CRC_ERR                    2
#define G2RE_EFS_RSP_FLASH_WRITE_ERR                 3
#define G2RE_EFS_RSP_TIMEOUT                         4
#define G2RE_EFS_RSP_NO_RESOURCES                    5
#define G2RE_EFS_RSP_RESULT_CHECK_FAIL               6
#define G2RE_EFS_RSP_FAIL                            7

// =============================================================================
// SECTION 7: OTA Transmit — sid=192..195
// =============================================================================
#define G2RE_OTA_CMD_TRANSMIT_START         0
#define G2RE_OTA_CMD_TRANSMIT_INFORMATION   1   // version, total size, partition
#define G2RE_OTA_CMD_TRANSMIT_FILE          2   // ack of raw firmware chunks
#define G2RE_OTA_CMD_TRANSMIT_RESULT_CHECK  3
#define G2RE_OTA_CMD_TRANSMIT_NOTIFY        4   // device → host status push

#define G2RE_OTA_RSP_SUCCESS         0
#define G2RE_OTA_RSP_HEADER_ERR      1
#define G2RE_OTA_RSP_PATH_ERR        2
#define G2RE_OTA_RSP_CRC_ERR         3
#define G2RE_OTA_RSP_TIMEOUT         4
#define G2RE_OTA_RSP_NO_RESOURCES    5
#define G2RE_OTA_RSP_FLASH_WRITE_ERR 6
#define G2RE_OTA_RSP_CHECK_FAIL      7
#define G2RE_OTA_RSP_UPDATING        8   // pushed via NOTIFY mid-transfer
#define G2RE_OTA_RSP_SYS_RESTART     9
#define G2RE_OTA_RSP_FAIL            10

// =============================================================================
// SECTION 8: Common error code (eErrorCode) — sub-message `result` field
// =============================================================================
#define G2RE_ERR_SUCCESS              0
#define G2RE_ERR_CRC                  1
#define G2RE_ERR_PKT_LOST             2
#define G2RE_ERR_TIMEOUT              3
#define G2RE_ERR_NO_RESOURCES         4
#define G2RE_ERR_PB_ERROR             5    // protobuf decode failure on device
#define G2RE_ERR_NULL                 6
#define G2RE_ERR_FAIL                 7
#define G2RE_ERR_NOT_SUPPORT          8
#define G2RE_ERR_SUPPORT              9
#define G2RE_ERR_HEAD_ID              10   // unrecognised SID
#define G2RE_ERR_INVALID_LENGTH       11
#define G2RE_ERR_INVALID_SDID         12   // bad sub-data-id
#define G2RE_ERR_DUPLICATE_PACKET     13   // re-used syncId
#define G2RE_ERR_RING_CONNECT_TIMEOUT 90

#endif // G2_RE_REFERENCE_H
