#ifndef SYSTEM_EVENTS_H
#define SYSTEM_EVENTS_H

/**
 * System Events - in-memory event register (the "event bus")
 *
 * A fixed ring of discrete, semantic system events (peer came online, text
 * received, battery low, login failed, ...) that any subsystem can post to
 * from any task, and that multiple consumers drain independently via
 * sequence-number cursors. Since the Phase-1 notification cutover this is
 * ALSO the notification pipeline: the old notify*() layer is gone, and
 * System_Notifications' main-loop renderer + the OLED notification-center
 * view are just consumers of this ring (per-kind rules decide which events
 * become banners/toasts/queue entries).
 *
 * It is deliberately NOT a log. logSystemEvent() remains the durable,
 * file-backed audit trail; this ring holds only the last SYSEVT_RING_SIZE
 * events, in RAM, for immediate reaction.
 *
 * Attribution: every event carries WHO caused it — a source interface byte
 * (NotificationSource, defined here) plus a short who[] identity (username /
 * device / IP prefix), stamped automatically at post time from the calling
 * task's TLS context (see setNotificationContext below). Consumers surface
 * it ("by web:hub"), and automation event-trigger `match` patterns test it.
 *
 * Task-safety: systemEventPost() is a bounded copy under a spinlock —
 * callable from any task (espnow_task, sensor workers, sr_task, cmd_exec,
 * BLE callbacks). Not for ISRs. Consumers each keep their own cursor and
 * call systemEventFetchSince() from their own task.
 */

#include <Arduino.h>
#include <stdint.h>

// ============================================================================
// Source attribution (moved here from System_Notifications.h — events are
// the base layer now; notifications consume them)
// ============================================================================

enum NotificationSource : uint8_t {
  NOTIF_SOURCE_UNKNOWN = 0,
  NOTIF_SOURCE_CLI     = 1,
  NOTIF_SOURCE_OLED    = 2,
  NOTIF_SOURCE_WEB     = 3,
  NOTIF_SOURCE_VOICE   = 4,
  NOTIF_SOURCE_REMOTE  = 5,
  NOTIF_SOURCE_SYSTEM  = 6,  // Firmware-generated: sensor starts, WiFi events, etc.
  NOTIF_SOURCE_G2      = 7   // BLE-attached lens — mirrors SOURCE_G2_GLASSES
};

// Per-task source context. Set before executing user-attributable work so
// events (and therefore notifications) carry who caused them.
// source: NotificationSource enum value; subsource: username/device/IP.
void setNotificationContext(uint8_t source, const char* subsource = nullptr);
void clearNotificationContext();

// RAII guard — installs the context on construction, restores the PRIOR
// context on destruction (save/restore, so nested guards compose).
struct NotificationContextGuard {
  NotificationContextGuard(uint8_t source, const char* subsource = nullptr);
  ~NotificationContextGuard();
  NotificationContextGuard(const NotificationContextGuard&) = delete;
  NotificationContextGuard& operator=(const NotificationContextGuard&) = delete;
  NotificationContextGuard(NotificationContextGuard&&) = delete;
  NotificationContextGuard& operator=(NotificationContextGuard&&) = delete;

 private:
  uint8_t savedSource_;
  char    savedSubsource_[32];
};

// ============================================================================
// Event kinds
// ============================================================================
// Single source of truth: the X-macro list below generates BOTH the
// SystemEventKind enum and the kEventKindNames[] table (System_Events.cpp),
// so the two can never drift.
//
// The snake_case names are the public contract — they're what users write in
// automation event triggers ({"type":"event","on":"peer_online"}) and what
// the web UI lists (WebPage_Automations.h keeps a manual name+label copy).
// Keep the names stable. The numeric values are internal-only: never
// persisted, never on any wire, rebuilt from names on every boot — so
// inserting a new event mid-list is safe. Keep each event with its family.
//
// ---------------------------------------------------------------------------
// Families — the grouping every surface renders by
// ---------------------------------------------------------------------------
// The kind list below was already grouped by comment blocks, but comments are
// invisible to consumers, so every UI showed one flat 134-row wall. The family
// is now DATA on each entry, generated from the same X-macro as the enum and
// the names, so it cannot drift either.
//
// The old comment blocks were not a taxonomy — the "(coverage)" and "(tier 2)"
// suffixes record when a batch was ADDED, which split ESP-NOW across two
// blocks, Sensors across three, Voice across two. These 11 families are the
// collapsed version; a kind lives with what it is ABOUT, not when it landed.
//
// Order here is display order. Within a family, kinds stay in declaration
// order so natural pairs (peer_online/peer_offline, charging_started/stopped)
// stay adjacent — alphabetising would split them.
#define SYSEVT_FAMILY_LIST(F) \
  F(SYSEVT_FAM_MESH,       "Mesh & ESP-NOW") \
  F(SYSEVT_FAM_NETWORK,    "Connectivity") \
  F(SYSEVT_FAM_GLASSES,    "Glasses") \
  F(SYSEVT_FAM_SENSORS,    "Sensors") \
  F(SYSEVT_FAM_MEDIA,      "Voice, AI & Media") \
  F(SYSEVT_FAM_AUTH,       "Auth & Users") \
  F(SYSEVT_FAM_SECURITY,   "Security") \
  F(SYSEVT_FAM_POWER,      "Power & Battery") \
  F(SYSEVT_FAM_STORAGE,    "Storage") \
  F(SYSEVT_FAM_AUTOMATION, "Automation") \
  F(SYSEVT_FAM_SYSTEM,     "System")

enum SystemEventFamily : uint8_t {
#define SYSEVT_FAM_X(sym, label) sym,
  SYSEVT_FAMILY_LIST(SYSEVT_FAM_X)
#undef SYSEVT_FAM_X
  SYSEVT_FAM_COUNT
};

// Display label for a family ("Mesh & ESP-NOW"). Returns "?" if out of range.
const char* systemEventFamilyName(uint8_t family);
// Which family a kind belongs to. Returns SYSEVT_FAM_SYSTEM for unknown kinds.
uint8_t systemEventKindFamily(uint8_t kind);

// Entry format:  X(SYSEVT_..., "name", SYSEVT_FAM_...)  /* what fires it; subject= detail= */
#define SYSEVT_KIND_LIST(X) \
  /* --- ESP-NOW mesh / bond / pairing --- */ \
  X(SYSEVT_PEER_ONLINE,         "peer_online", SYSEVT_FAM_MESH)          /* mesh peer heartbeat appeared     subject=name  detail=mac */ \
  X(SYSEVT_PEER_OFFLINE,        "peer_offline", SYSEVT_FAM_MESH)         /* mesh peer heartbeat timed out    subject=name  detail=mac */ \
  X(SYSEVT_PEER_PAIRED,         "peer_paired", SYSEVT_FAM_MESH)          /* pairing-mode auto-pair ran       subject=name  detail=mac */ \
  X(SYSEVT_TEXT_RX,             "text_rx", SYSEVT_FAM_MESH)              /* ESP-NOW text message received    subject=sender detail=text */ \
  X(SYSEVT_FILE_RX,             "file_rx", SYSEVT_FAM_MESH)              /* ESP-NOW file received            subject=sender detail=filename */ \
  X(SYSEVT_BOND_ONLINE,         "bond_online", SYSEVT_FAM_MESH)          /* bond peer session/heartbeat up   subject=name  detail=mac */ \
  X(SYSEVT_BOND_OFFLINE,        "bond_offline", SYSEVT_FAM_MESH)         /* bond peer heartbeat timed out    subject=name  detail=mac */ \
  X(SYSEVT_BOND_REJECT,         "bond_reject", SYSEVT_FAM_MESH)          /* unpaired sender probed bond channel  subject=mac detail=count (30s cooldown) */ \
  X(SYSEVT_ESPNOW_ON,           "espnow_on", SYSEVT_FAM_MESH)            /* ESP-NOW initialized */ \
  X(SYSEVT_ESPNOW_OFF,          "espnow_off", SYSEVT_FAM_MESH)           /* ESP-NOW deinitialized */ \
  X(SYSEVT_PAIR_WINDOW_OPEN,    "pair_window_open", SYSEVT_FAM_MESH)     /* pairing-mode window opened   subject=seconds */ \
  X(SYSEVT_PAIR_WINDOW_CLOSED,  "pair_window_closed", SYSEVT_FAM_MESH)   /* pairing-mode window closed */ \
  X(SYSEVT_MESH_PROMOTED,       "mesh_promoted", SYSEVT_FAM_MESH)        /* this device promoted to backup master */ \
  X(SYSEVT_MESH_DEMOTED,        "mesh_demoted", SYSEVT_FAM_MESH)         /* real master returned, demoted back */ \
  X(SYSEVT_REMOTE_CMD_RX,       "remote_cmd_rx", SYSEVT_FAM_MESH)        /* remote command ran on THIS device  subject=sender detail=command */ \
  X(SYSEVT_REMOTE_CMD_SENT,     "remote_cmd_sent", SYSEVT_FAM_MESH)      /* remote command sent FROM this device  subject=target detail="#reqId cmd" */ \
  X(SYSEVT_REMOTE_CMD_RESULT,   "remote_cmd_result", SYSEVT_FAM_MESH)    /* a sent remote command's result came back  subject=peer detail="#reqId ok|failed result" */ \
  /* --- Connectivity (WiFi / MQTT / BLE / time) --- */ \
  X(SYSEVT_WIFI_CONNECTED,      "wifi_connected", SYSEVT_FAM_NETWORK)       /* subject=ip */ \
  X(SYSEVT_WIFI_DISCONNECTED,   "wifi_disconnected", SYSEVT_FAM_NETWORK) \
  X(SYSEVT_WIFI_CONNECT_FAILED, "wifi_connect_failed", SYSEVT_FAM_NETWORK)  /* subject=ssid detail=attempts/status */ \
  X(SYSEVT_WIFI_NET_ADDED,      "wifi_net_added", SYSEVT_FAM_NETWORK)       /* subject=ssid */ \
  X(SYSEVT_WIFI_NET_REMOVED,    "wifi_net_removed", SYSEVT_FAM_NETWORK)     /* subject=ssid */ \
  X(SYSEVT_MQTT_CONNECTED,      "mqtt_connected", SYSEVT_FAM_NETWORK)       /* subject=broker host:port */ \
  X(SYSEVT_MQTT_DISCONNECTED,   "mqtt_disconnected", SYSEVT_FAM_NETWORK)    /* subject=seconds connected */ \
  X(SYSEVT_BLE_CONNECTED,       "ble_connected", SYSEVT_FAM_NETWORK)        /* companion BLE central connected  subject=type detail=mac */ \
  X(SYSEVT_BLE_DISCONNECTED,    "ble_disconnected", SYSEVT_FAM_NETWORK)     /* subject=remaining connection count */ \
  X(SYSEVT_TIME_SYNCED,         "time_synced", SYSEVT_FAM_NETWORK)          /* clock first became valid  subject=ntp|rtc detail=time */ \
  /* --- G2 glasses --- */ \
  X(SYSEVT_G2_CONNECTED,        "g2_connected", SYSEVT_FAM_GLASSES)         /* subject=sides (L+R/L/R) */ \
  X(SYSEVT_G2_DISCONNECTED,     "g2_disconnected", SYSEVT_FAM_GLASSES)      /* subject=side that dropped */ \
  X(SYSEVT_G2_WORN,             "g2_worn", SYSEVT_FAM_GLASSES)              /* plugin heartbeats resumed (picked up)  subject=side */ \
  X(SYSEVT_G2_NOT_WORN,         "g2_not_worn", SYSEVT_FAM_GLASSES)          /* plugin heartbeats stopped (set down)   subject=side */ \
  X(SYSEVT_RING_CONNECTED,      "ring_connected", SYSEVT_FAM_GLASSES)       /* R1 ring GATT up after setup  subject=name detail=mac */ \
  X(SYSEVT_RING_DISCONNECTED,   "ring_disconnected", SYSEVT_FAM_GLASSES)    /* R1 ring link lost  subject=name detail=mac */ \
  X(SYSEVT_RING_WORN,           "ring_worn", SYSEVT_FAM_GLASSES)            /* R1 wearStatus→wear (on finger)  subject=name */ \
  X(SYSEVT_RING_NOT_WORN,       "ring_not_worn", SYSEVT_FAM_GLASSES)        /* R1 wearStatus→notWear (off finger)  subject=name */ \
  X(SYSEVT_RING_RECONNECT_FAILED, "ring_reconnect_failed", SYSEVT_FAM_GLASSES) /* R1 connect attempt failed  subject=name detail=reason fail# elapsed; throttled 10min/streak */ \
  X(SYSEVT_G2_RECONNECT_FAILED, "g2_reconnect_failed", SYSEVT_FAM_GLASSES)  /* glasses connect attempt failed  subject=sides sought detail=reason fail#; throttled 10min/streak */ \
  X(SYSEVT_BLE_STACK_RECYCLED,  "ble_stack_recycled", SYSEVT_FAM_NETWORK)   /* Bluedroid host torn down + re-inited to clear a wedge  subject=wedged|reinit-failed */ \
  /* --- Auth / users --- */ \
  X(SYSEVT_LOGIN_OK,            "login_ok", SYSEVT_FAM_AUTH)             /* subject=username detail=transport */ \
  X(SYSEVT_LOGIN_FAIL,          "login_fail", SYSEVT_FAM_AUTH)           /* subject=username detail=transport */ \
  X(SYSEVT_USER_REQUEST,        "user_request", SYSEVT_FAM_AUTH)         /* account request submitted  subject=username */ \
  X(SYSEVT_USER_ADDED,          "user_added", SYSEVT_FAM_AUTH)           /* subject=username detail=by */ \
  X(SYSEVT_USER_DELETED,        "user_deleted", SYSEVT_FAM_AUTH)         /* subject=username detail=by */ \
  X(SYSEVT_USER_APPROVED,       "user_approved", SYSEVT_FAM_AUTH)        /* subject=username detail=by */ \
  X(SYSEVT_PASSWORD_CHANGED,    "password_changed", SYSEVT_FAM_AUTH)     /* subject=username detail=self|admin-reset */ \
  /* --- Power / battery --- */ \
  X(SYSEVT_USB_ON,              "usb_on", SYSEVT_FAM_POWER) \
  X(SYSEVT_USB_OFF,             "usb_off", SYSEVT_FAM_POWER) \
  X(SYSEVT_CHARGING_STARTED,    "charging_started", SYSEVT_FAM_POWER)     /* subject=percent */ \
  X(SYSEVT_CHARGING_STOPPED,    "charging_stopped", SYSEVT_FAM_POWER)     /* subject=percent */ \
  X(SYSEVT_BATTERY_LOW,         "battery_low", SYSEVT_FAM_POWER)          /* subject=percent */ \
  X(SYSEVT_BATTERY_CRITICAL,    "battery_critical", SYSEVT_FAM_POWER)     /* subject=percent */ \
  X(SYSEVT_POWER_SAVE_ENTER,    "power_save_enter", SYSEVT_FAM_POWER) \
  X(SYSEVT_POWER_SAVE_EXIT,     "power_save_exit", SYSEVT_FAM_POWER) \
  /* --- Storage --- */ \
  X(SYSEVT_SD_MOUNTED,          "sd_mounted", SYSEVT_FAM_STORAGE)           /* subject=free MB */ \
  X(SYSEVT_SD_UNMOUNTED,        "sd_unmounted", SYSEVT_FAM_STORAGE) \
  X(SYSEVT_SD_WRITE_FAILED,     "sd_write_failed", SYSEVT_FAM_STORAGE)      /* SD went unwritable (once per episode) */ \
  X(SYSEVT_FS_LOW_SPACE,        "fs_low_space", SYSEVT_FAM_STORAGE)         /* flash log-overflow latch tripped (once per boot) */ \
  X(SYSEVT_FILE_DELETED,        "file_deleted", SYSEVT_FAM_STORAGE)         /* subject=filename detail=full path */ \
  X(SYSEVT_SETTINGS_SAVE_FAILED,"settings_save_failed", SYSEVT_FAM_STORAGE) /* subject=stage detail=file */ \
  /* --- System / settings / sensor lifecycle --- */ \
  X(SYSEVT_SETTING_CHANGED,     "setting_changed", SYSEVT_FAM_SYSTEM)      /* subject=key detail=value */ \
  X(SYSEVT_SENSOR_STARTED,      "sensor_started", SYSEVT_FAM_SENSORS)       /* subject=sensor name (successful starts only) */ \
  X(SYSEVT_SENSOR_STOPPED,      "sensor_stopped", SYSEVT_FAM_SENSORS)       /* subject=sensor name */ \
  X(SYSEVT_SENSOR_START_FAILED, "sensor_start_failed", SYSEVT_FAM_SENSORS)  /* subject=sensor name */ \
  X(SYSEVT_SENSOR_FAULT,        "sensor_fault", SYSEVT_FAM_SENSORS)         /* sensor auto-disabled after repeated errors  subject=name */ \
  /* --- Sensor / input events --- */ \
  X(SYSEVT_PRESENCE_DETECTED,   "presence_detected", SYSEVT_FAM_SENSORS)    /* STHS34PF80 presence trip  subject=value */ \
  X(SYSEVT_PRESENCE_CLEARED,    "presence_cleared", SYSEVT_FAM_SENSORS)     /* presence gone (held-down) */ \
  X(SYSEVT_GESTURE,             "gesture", SYSEVT_FAM_SENSORS)              /* APDS9960 gesture  subject=up|down|left|right */ \
  X(SYSEVT_IMU_SHAKE,           "imu_shake", SYSEVT_FAM_SENSORS)            /* subject=intensity */ \
  X(SYSEVT_IMU_TAP,             "imu_tap", SYSEVT_FAM_SENSORS)              /* subject=strength */ \
  X(SYSEVT_IMU_FREEFALL,        "imu_freefall", SYSEVT_FAM_SENSORS)         /* subject=duration (posts after 150ms sustained) */ \
  X(SYSEVT_IMU_ORIENTATION,     "imu_orientation", SYSEVT_FAM_SENSORS)      /* stable orientation change  subject=new detail=prev */ \
  X(SYSEVT_GPS_FIX,             "gps_fix", SYSEVT_FAM_SENSORS)              /* fix acquired  subject=sats detail=lat,lon */ \
  X(SYSEVT_GPS_LOST,            "gps_lost", SYSEVT_FAM_SENSORS)             /* fix lost (10s hold-down) */ \
  X(SYSEVT_BUTTON,              "button", SYSEVT_FAM_SENSORS)               /* gamepad/encoder button PRESS  subject=button name */ \
  X(SYSEVT_FM_RDS_STATION,      "fm_rds_station", SYSEVT_FAM_SENSORS)       /* RDS station identified  subject=name detail=freq */ \
  /* --- Voice / AI / media --- */ \
  X(SYSEVT_VOICE_WAKE,          "voice_wake", SYSEVT_FAM_MEDIA)           /* wake word detected */ \
  X(SYSEVT_VOICE_COMMAND,       "voice_command", SYSEVT_FAM_MEDIA)        /* subject=command (successful commands only) */ \
  X(SYSEVT_EI_DETECTED,         "ei_detected", SYSEVT_FAM_MEDIA)          /* Edge Impulse object confirmed  subject=label detail=confidence */ \
  X(SYSEVT_EI_LOST,             "ei_lost", SYSEVT_FAM_MEDIA)              /* tracked object gone (2s timeout)  subject=label */ \
  X(SYSEVT_PHOTO_SAVED,         "photo_saved", SYSEVT_FAM_MEDIA)          /* subject=filename detail=path */ \
  X(SYSEVT_VIDEO_SAVED,         "video_saved", SYSEVT_FAM_MEDIA)          /* subject=filename detail=frames */ \
  X(SYSEVT_MIC_SAVED,           "mic_saved", SYSEVT_FAM_MEDIA)            /* recording saved  subject=filename */ \
  X(SYSEVT_LLM_GEN_DONE,        "llm_gen_done", SYSEVT_FAM_MEDIA)         /* subject=stop reason detail=tokens */ \
  X(SYSEVT_LLM_MODEL_LOADED,    "llm_model_loaded", SYSEVT_FAM_MEDIA)     /* subject=model filename */ \
  /* --- Lifecycle --- */ \
  X(SYSEVT_BOOT,                "boot", SYSEVT_FAM_SYSTEM)                 /* device finished booting — posted at end of setup  subject=reset-reason detail="boot #N" */ \
  X(SYSEVT_REBOOT,              "reboot", SYSEVT_FAM_SYSTEM)               /* intentional restart — posted on the NEXT boot (the ring can't survive the restart)  subject=reason(command|setup|g2|factory|software) detail=reset-reason */ \
  X(SYSEVT_CRASH,               "crash", SYSEVT_FAM_SYSTEM)                /* unexpected reset (panic/watchdog/brownout/lockup) — posted on next boot  subject=reset-reason detail="boot #N crashCount=M" */ \
  /* --- Security / trust (coverage) --- */ \
  X(SYSEVT_PEER_UNPAIRED,       "peer_unpaired", SYSEVT_FAM_SECURITY)        /* a paired peer removed from the trust list  subject=name detail=mac */ \
  X(SYSEVT_IDENTITY_REGEN,      "identity_regenerated", SYSEVT_FAM_SECURITY) /* this device's ESP-NOW identity keypair rotated (invalidates all bonds)  subject=pubkey */ \
  X(SYSEVT_USER_PROMOTED,       "user_promoted", SYSEVT_FAM_SECURITY)        /* account granted admin  subject=username */ \
  X(SYSEVT_USER_DEMOTED,        "user_demoted", SYSEVT_FAM_SECURITY)         /* admin privileges revoked  subject=username */ \
  X(SYSEVT_USER_BANNED,         "user_banned", SYSEVT_FAM_SECURITY)          /* account suspended (sessions kicked)  subject=username */ \
  X(SYSEVT_IP_BANNED,           "ip_banned", SYSEVT_FAM_SECURITY)            /* an IP added to the ban list  subject=ip */ \
  X(SYSEVT_LOGIN_LOCKED,        "login_locked", SYSEVT_FAM_SECURITY)         /* an IP rate-limit locked out after failed logins  subject=ip detail=seconds */ \
  X(SYSEVT_VOICE_ARMED,         "voice_armed", SYSEVT_FAM_MEDIA)          /* voice pipeline armed to run commands as a user  subject=username */ \
  X(SYSEVT_STORAGE_FORMATTED,   "storage_formatted", SYSEVT_FAM_STORAGE)    /* a storage volume wiped  subject=sd|flash */ \
  X(SYSEVT_G2_SILENT_MODE,      "g2_silent_mode", SYSEVT_FAM_GLASSES)       /* glasses silent/DND toggled  subject=on|off */ \
  /* --- Service / engine faults (coverage) --- */ \
  X(SYSEVT_MQTT_START_FAILED,   "mqtt_start_failed", SYSEVT_FAM_NETWORK)    /* MQTT failed to start  subject=reason */ \
  X(SYSEVT_LLM_LOAD_FAILED,     "llm_load_failed", SYSEVT_FAM_MEDIA)      /* on-device model load failed  subject=reason */ \
  X(SYSEVT_LLM_STATE_CORRUPT,   "llm_state_corrupt", SYSEVT_FAM_MEDIA)    /* unrecoverable LLM engine fault  subject=detail */ \
  /* --- Sensors / capability (coverage) --- */ \
  X(SYSEVT_MOTION_DETECTED,     "motion_detected", SYSEVT_FAM_SENSORS)      /* STHS34PF80 motion algorithm edge  subject=detected|cleared */ \
  X(SYSEVT_RTC_POWER_LOSS,      "rtc_power_loss", SYSEVT_FAM_SENSORS)       /* RTC lost power (dead coin cell), kept time invalid */ \
  /* --- Recording / automation (coverage) --- */ \
  X(SYSEVT_MIC_RECORD_STARTED,  "mic_record_started", SYSEVT_FAM_MEDIA)   /* microphone recording to a file started  subject=filename */ \
  X(SYSEVT_AUTOMATION_FIRED,    "automation_fired", SYSEVT_FAM_AUTOMATION)     /* an automation matched and ran its commands  subject=name detail=trigger */ \
  /* --- Service / connectivity lifecycle (tier 2) --- */ \
  X(SYSEVT_HTTP_SERVER_STARTED, "http_server_started", SYSEVT_FAM_NETWORK)  /* web server came up  subject=http|https detail=port */ \
  X(SYSEVT_HTTP_SERVER_STOPPED, "http_server_stopped", SYSEVT_FAM_NETWORK)  /* web server shut down */ \
  X(SYSEVT_BLE_ON,              "ble_on", SYSEVT_FAM_NETWORK)               /* BLE radio stack started */ \
  X(SYSEVT_BLE_OFF,             "ble_off", SYSEVT_FAM_NETWORK)              /* BLE radio stack stopped */ \
  X(SYSEVT_MQTT_EXT_SENSOR_NEW, "mqtt_ext_sensor_new", SYSEVT_FAM_NETWORK)  /* new external sensor discovered via MQTT  subject=topic */ \
  X(SYSEVT_CERT_GENERATED,      "cert_generated", SYSEVT_FAM_NETWORK)       /* TLS certificate generated  subject=cn|type */ \
  /* --- Sessions / access (tier 2) --- */ \
  X(SYSEVT_LOGOUT,              "logout", SYSEVT_FAM_AUTH)               /* a session ended  subject=username detail=reason */ \
  X(SYSEVT_USER_REJECTED,       "user_rejected", SYSEVT_FAM_AUTH)        /* a pending user request was denied  subject=username */ \
  X(SYSEVT_COMMAND_DENIED,      "command_denied", SYSEVT_FAM_AUTH)       /* a logged-in user was denied a privileged command  subject=user detail=command */ \
  X(SYSEVT_AUTH_DB_FAULT,       "auth_db_fault", SYSEVT_FAM_AUTH)        /* the user/credential database failed to load or is corrupt */ \
  /* --- Config / device lifecycle (tier 2) --- */ \
  X(SYSEVT_FACTORY_RESET,       "factory_reset", SYSEVT_FAM_SYSTEM)        /* accounts wiped, device reset to setup  subject=actor */ \
  X(SYSEVT_FEATURE_TOGGLED,     "feature_toggled", SYSEVT_FAM_SYSTEM)      /* a subsystem/feature enabled or disabled  subject=feature detail=on|off */ \
  X(SYSEVT_FIRMWARE_CHANGED,    "firmware_changed", SYSEVT_FAM_SYSTEM)     /* running firmware differs from last boot (update applied)  subject=old->new */ \
  X(SYSEVT_BACKUP_CREATED,      "backup_created", SYSEVT_FAM_SYSTEM)       /* a config backup was exported  subject=categories */ \
  X(SYSEVT_BACKUP_RESTORED,     "backup_restored", SYSEVT_FAM_SYSTEM)      /* a config backup was restored  subject=categories */ \
  X(SYSEVT_CONFIG_FILE_CORRUPT, "config_file_corrupt", SYSEVT_FAM_SYSTEM)  /* a critical config file failed its integrity check  subject=file */ \
  X(SYSEVT_SECRET_DECRYPT_FAILED,"secret_decrypt_failed", SYSEVT_FAM_SYSTEM)/* a stored secret failed to decrypt at load  subject=which */ \
  X(SYSEVT_SD_WRITE_RECOVERED,  "sd_write_recovered", SYSEVT_FAM_STORAGE)   /* SD writes resumed after a fault  subject=sd */ \
  X(SYSEVT_POWER_MODE_CHANGED,  "power_mode_changed", SYSEVT_FAM_POWER)   /* power mode toggled  subject=mode */ \
  X(SYSEVT_BATTERY_FULL,        "battery_full", SYSEVT_FAM_POWER)         /* battery reached full charge  subject=percent */ \
  /* --- ESP-NOW / streaming (tier 2) --- */ \
  X(SYSEVT_MESH_PASSPHRASE_CHANGED,"mesh_passphrase_changed", SYSEVT_FAM_MESH) /* mesh group key set/changed/cleared  subject=mesh detail=set|cleared */ \
  X(SYSEVT_REMOTE_STREAM_STARTED,"remote_stream_started", SYSEVT_FAM_MESH) /* a peer started tapping this device's output  subject=peer */ \
  X(SYSEVT_FILE_RX_FAILED,      "file_rx_failed", SYSEVT_FAM_MESH)       /* an inbound ESP-NOW file transfer failed  subject=sender detail=filename */ \
  /* --- Glasses / recording (tier 2) --- */ \
  X(SYSEVT_G2_HIJACK_ENTERED,   "g2_hijack_entered", SYSEVT_FAM_GLASSES)    /* interactive glasses-driven session started  subject=page */ \
  X(SYSEVT_G2_HIJACK_EXITED,    "g2_hijack_exited", SYSEVT_FAM_GLASSES)     /* glasses-driven session ended  subject=reason */ \
  X(SYSEVT_VIDEO_RECORD_STARTED,"video_record_started", SYSEVT_FAM_MEDIA) /* video recording began  subject=filename */ \
  /* --- Sensors / AI (tier 2) --- */ \
  X(SYSEVT_THERMAL_HOT_ALERT,   "thermal_hot_alert", SYSEVT_FAM_SENSORS)    /* thermal camera saw an over-temp hotspot  subject=maxC */ \
  X(SYSEVT_TOF_OBJECT_DETECTED, "tof_object_detected", SYSEVT_FAM_SENSORS)  /* ToF sensor sees an object near/far edge  subject=near|far detail=mm */ \
  X(SYSEVT_FM_TUNED,            "fm_tuned", SYSEVT_FAM_SENSORS)             /* FM radio tuned  subject=MHz */ \
  X(SYSEVT_EI_CONTINUOUS_STARTED,"ei_continuous_started", SYSEVT_FAM_MEDIA)/* continuous camera-AI detection mode started */ \
  X(SYSEVT_IMU_WALKING,         "imu_walking", SYSEVT_FAM_SENSORS)          /* step detector edge  subject=started|stopped detail=cadence */ \
  /* --- Voice / model / UI (tier 2) --- */ \
  X(SYSEVT_VOICE_DISARMED,      "voice_disarmed", SYSEVT_FAM_MEDIA)       /* voice command execution disarmed  subject=username */ \
  X(SYSEVT_LLM_MODEL_UNLOADED,  "llm_model_unloaded", SYSEVT_FAM_MEDIA)   /* on-device model unloaded (assistant offline) */ \
  X(SYSEVT_DISPLAY_INIT_FAILED, "display_init_failed", SYSEVT_FAM_SYSTEM)  /* the OLED failed to initialize at boot  subject=address */ \
  /* --- Automation CRUD (tier 2) --- */ \
  X(SYSEVT_AUTOMATION_ADDED,    "automation_added", SYSEVT_FAM_AUTOMATION)     /* an automation was created  subject=name */ \
  X(SYSEVT_AUTOMATION_DELETED,  "automation_deleted", SYSEVT_FAM_AUTOMATION)   /* an automation was deleted  subject=name */ \
  X(SYSEVT_AUTOMATION_ACTION_DROPPED,"automation_action_dropped", SYSEVT_FAM_AUTOMATION) /* an automation action was skipped/failed mid-run  subject=name detail=reason */

enum SystemEventKind : uint8_t {
  SYSEVT_NONE = 0,
#define SYSEVT_X(sym, name, fam) sym,
  SYSEVT_KIND_LIST(SYSEVT_X)
#undef SYSEVT_X
  SYSEVT_COUNT
};
static_assert(SYSEVT_COUNT <= 256, "automation subscription mask is 256 bits (8x uint32 words)");

#define SYSEVT_RING_SIZE 48
// Field widths grown 2026-07 for longer names/descriptions. Cost is ~2x the
// per-slot growth in internal DRAM: the struct sizes BOTH the ring here and
// the automation drain buffer (sEventBuf[SYSEVT_RING_SIZE]). Keep modest.
#define SYSEVT_SUBJECT_LEN 48
#define SYSEVT_DETAIL_LEN 80
#define SYSEVT_WHO_LEN 24

struct SystemEvent {
  uint32_t seq;     // monotonically increasing, never 0 for a live event
  uint32_t tsMs;    // millis() at post time
  uint8_t kind;     // SystemEventKind
  uint8_t source;   // NotificationSource — which interface caused it
  char who[SYSEVT_WHO_LEN];          // username / device / IP prefix ('\0' = n/a)
  char subject[SYSEVT_SUBJECT_LEN];  // who/what (peer name, sensor, username, key)
  char detail[SYSEVT_DETAIL_LEN];    // extra (MAC, text preview, filename, value)
};

// Post an event from any task (not ISR). subject/detail may be nullptr; both
// truncate to their field sizes. source/who are stamped from the calling
// task's notification TLS context (UNKNOWN maps to SYSTEM — a task with no
// installed context is firmware-initiated by definition); pass srcOverride /
// whoOverride to stamp explicitly instead. Also nudges the automation
// scheduler if any enabled automation subscribes to this kind.
void systemEventPost(uint8_t kind, const char* subject = nullptr, const char* detail = nullptr,
                     uint8_t srcOverride = 0xFF, const char* whoOverride = nullptr);

// Copy out every event with seq > *cursor (oldest first), up to maxOut, and
// advance *cursor past the newest copied. A consumer more than
// SYSEVT_RING_SIZE behind skips the overwritten gap. Pass
// *cursor = UINT32_MAX to start "from now"; 0 to start from the oldest
// live event. Returns the number copied.
// skippedOut (optional): ACCUMULATES the size of any skipped gap — pass a
// persistent per-consumer counter to make the loss observable. The aggregate
// across all consumers is systemEventRingSkippedTotal().
int systemEventFetchSince(uint32_t* cursor, SystemEvent* out, int maxOut,
                          uint32_t* skippedOut = nullptr);

// Total ring-overwrite skips across every consumer since boot. Nonzero means
// a consumer fell >SYSEVT_RING_SIZE events behind and lost events silently
// (for the automation cursor that means event triggers that never fired).
uint32_t systemEventRingSkippedTotal();

// Copy one event by exact sequence number. Returns false if that seq has
// been overwritten (fell off the ring) or never existed. Lets viewers walk
// the ring newest-first without a 5KB snapshot buffer.
bool systemEventGetBySeq(uint32_t seq, SystemEvent* out);

// Newest sequence number posted so far (0 = nothing yet).
uint32_t systemEventLatestSeq();

// Total events ever posted (== latest seq).
uint32_t systemEventTotalPosted();

// Kind <-> name mapping ("peer_online", "battery_low", ...).
const char* systemEventKindName(uint8_t kind);
int systemEventKindFromName(const char* name);

// Short source-interface name ("cli", "web", "system", ...).
const char* systemEventSourceName(uint8_t source);

// Match helper shared by the automation event matcher: true when `pattern`
// is empty, "*", or a case-insensitive substring of the event's subject,
// detail, OR who.
bool systemEventMatches(const SystemEvent& ev, const char* pattern);

// `events` CLI command (registered in the system module table).
const char* cmd_events(const String& argsInput);

// Structured event-history file sink — the third ring consumer. Drains the
// ring into /system/sys_logs/events.log (one machine-parseable line per
// event) via the debug output task's [EVLOG] tee. Called once per main-loop
// iteration; internally throttled and gated on gSettings.eventLogEnabled.
// The curated free-text system-events.log is a separate, untouched pipeline.
void systemEventLogTick(bool force = false);  // force=true drains the ring ignoring the 2s throttle
void systemEventLogFlush();                    // enqueue all pending events to events.log NOW (e.g. before a reboot)

#endif // SYSTEM_EVENTS_H
