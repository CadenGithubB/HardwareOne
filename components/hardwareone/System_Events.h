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
// Entry format:  X(SYSEVT_..., "name")  /* what fires it; subject= detail= */
#define SYSEVT_KIND_LIST(X) \
  /* --- ESP-NOW mesh / bond / pairing --- */ \
  X(SYSEVT_PEER_ONLINE,         "peer_online")          /* mesh peer heartbeat appeared     subject=name  detail=mac */ \
  X(SYSEVT_PEER_OFFLINE,        "peer_offline")         /* mesh peer heartbeat timed out    subject=name  detail=mac */ \
  X(SYSEVT_PEER_PAIRED,         "peer_paired")          /* pairing-mode auto-pair ran       subject=name  detail=mac */ \
  X(SYSEVT_TEXT_RX,             "text_rx")              /* ESP-NOW text message received    subject=sender detail=text */ \
  X(SYSEVT_FILE_RX,             "file_rx")              /* ESP-NOW file received            subject=sender detail=filename */ \
  X(SYSEVT_BOND_ONLINE,         "bond_online")          /* bond peer session/heartbeat up   subject=name  detail=mac */ \
  X(SYSEVT_BOND_OFFLINE,        "bond_offline")         /* bond peer heartbeat timed out    subject=name  detail=mac */ \
  X(SYSEVT_BOND_REJECT,         "bond_reject")          /* unpaired sender probed bond channel  subject=mac detail=count (30s cooldown) */ \
  X(SYSEVT_ESPNOW_ON,           "espnow_on")            /* ESP-NOW initialized */ \
  X(SYSEVT_ESPNOW_OFF,          "espnow_off")           /* ESP-NOW deinitialized */ \
  X(SYSEVT_PAIR_WINDOW_OPEN,    "pair_window_open")     /* pairing-mode window opened   subject=seconds */ \
  X(SYSEVT_PAIR_WINDOW_CLOSED,  "pair_window_closed")   /* pairing-mode window closed */ \
  X(SYSEVT_MESH_PROMOTED,       "mesh_promoted")        /* this device promoted to backup master */ \
  X(SYSEVT_MESH_DEMOTED,        "mesh_demoted")         /* real master returned, demoted back */ \
  X(SYSEVT_REMOTE_CMD_RX,       "remote_cmd_rx")        /* remote command ran on THIS device  subject=sender detail=command */ \
  X(SYSEVT_REMOTE_CMD_SENT,     "remote_cmd_sent")      /* remote command sent FROM this device  subject=target detail="#reqId cmd" */ \
  X(SYSEVT_REMOTE_CMD_RESULT,   "remote_cmd_result")    /* a sent remote command's result came back  subject=peer detail="#reqId ok|failed result" */ \
  /* --- Connectivity (WiFi / MQTT / BLE / time) --- */ \
  X(SYSEVT_WIFI_CONNECTED,      "wifi_connected")       /* subject=ip */ \
  X(SYSEVT_WIFI_DISCONNECTED,   "wifi_disconnected") \
  X(SYSEVT_WIFI_CONNECT_FAILED, "wifi_connect_failed")  /* subject=ssid detail=attempts/status */ \
  X(SYSEVT_WIFI_NET_ADDED,      "wifi_net_added")       /* subject=ssid */ \
  X(SYSEVT_WIFI_NET_REMOVED,    "wifi_net_removed")     /* subject=ssid */ \
  X(SYSEVT_MQTT_CONNECTED,      "mqtt_connected")       /* subject=broker host:port */ \
  X(SYSEVT_MQTT_DISCONNECTED,   "mqtt_disconnected")    /* subject=seconds connected */ \
  X(SYSEVT_BLE_CONNECTED,       "ble_connected")        /* companion BLE central connected  subject=type detail=mac */ \
  X(SYSEVT_BLE_DISCONNECTED,    "ble_disconnected")     /* subject=remaining connection count */ \
  X(SYSEVT_TIME_SYNCED,         "time_synced")          /* clock first became valid  subject=ntp|rtc detail=time */ \
  /* --- G2 glasses --- */ \
  X(SYSEVT_G2_CONNECTED,        "g2_connected")         /* subject=sides (L+R/L/R) */ \
  X(SYSEVT_G2_DISCONNECTED,     "g2_disconnected")      /* subject=side that dropped */ \
  X(SYSEVT_G2_WORN,             "g2_worn")              /* plugin heartbeats resumed (picked up)  subject=side */ \
  X(SYSEVT_G2_NOT_WORN,         "g2_not_worn")          /* plugin heartbeats stopped (set down)   subject=side */ \
  /* --- Auth / users --- */ \
  X(SYSEVT_LOGIN_OK,            "login_ok")             /* subject=username detail=transport */ \
  X(SYSEVT_LOGIN_FAIL,          "login_fail")           /* subject=username detail=transport */ \
  X(SYSEVT_USER_REQUEST,        "user_request")         /* account request submitted  subject=username */ \
  X(SYSEVT_USER_ADDED,          "user_added")           /* subject=username detail=by */ \
  X(SYSEVT_USER_DELETED,        "user_deleted")         /* subject=username detail=by */ \
  X(SYSEVT_USER_APPROVED,       "user_approved")        /* subject=username detail=by */ \
  X(SYSEVT_PASSWORD_CHANGED,    "password_changed")     /* subject=username detail=self|admin-reset */ \
  /* --- Power / battery --- */ \
  X(SYSEVT_USB_ON,              "usb_on") \
  X(SYSEVT_USB_OFF,             "usb_off") \
  X(SYSEVT_CHARGING_STARTED,    "charging_started")     /* subject=percent */ \
  X(SYSEVT_CHARGING_STOPPED,    "charging_stopped")     /* subject=percent */ \
  X(SYSEVT_BATTERY_LOW,         "battery_low")          /* subject=percent */ \
  X(SYSEVT_BATTERY_CRITICAL,    "battery_critical")     /* subject=percent */ \
  X(SYSEVT_POWER_SAVE_ENTER,    "power_save_enter") \
  X(SYSEVT_POWER_SAVE_EXIT,     "power_save_exit") \
  /* --- Storage --- */ \
  X(SYSEVT_SD_MOUNTED,          "sd_mounted")           /* subject=free MB */ \
  X(SYSEVT_SD_UNMOUNTED,        "sd_unmounted") \
  X(SYSEVT_SD_WRITE_FAILED,     "sd_write_failed")      /* SD went unwritable (once per episode) */ \
  X(SYSEVT_FS_LOW_SPACE,        "fs_low_space")         /* flash log-overflow latch tripped (once per boot) */ \
  X(SYSEVT_FILE_DELETED,        "file_deleted")         /* subject=filename detail=full path */ \
  X(SYSEVT_SETTINGS_SAVE_FAILED,"settings_save_failed") /* subject=stage detail=file */ \
  /* --- System / settings / sensor lifecycle --- */ \
  X(SYSEVT_SETTING_CHANGED,     "setting_changed")      /* subject=key detail=value */ \
  X(SYSEVT_SENSOR_STARTED,      "sensor_started")       /* subject=sensor name (successful starts only) */ \
  X(SYSEVT_SENSOR_STOPPED,      "sensor_stopped")       /* subject=sensor name */ \
  X(SYSEVT_SENSOR_START_FAILED, "sensor_start_failed")  /* subject=sensor name */ \
  X(SYSEVT_SENSOR_FAULT,        "sensor_fault")         /* sensor auto-disabled after repeated errors  subject=name */ \
  /* --- Sensor / input events --- */ \
  X(SYSEVT_PRESENCE_DETECTED,   "presence_detected")    /* STHS34PF80 presence trip  subject=value */ \
  X(SYSEVT_PRESENCE_CLEARED,    "presence_cleared")     /* presence gone (held-down) */ \
  X(SYSEVT_GESTURE,             "gesture")              /* APDS9960 gesture  subject=up|down|left|right */ \
  X(SYSEVT_IMU_SHAKE,           "imu_shake")            /* subject=intensity */ \
  X(SYSEVT_IMU_TAP,             "imu_tap")              /* subject=strength */ \
  X(SYSEVT_IMU_FREEFALL,        "imu_freefall")         /* subject=duration (posts after 150ms sustained) */ \
  X(SYSEVT_IMU_ORIENTATION,     "imu_orientation")      /* stable orientation change  subject=new detail=prev */ \
  X(SYSEVT_GPS_FIX,             "gps_fix")              /* fix acquired  subject=sats detail=lat,lon */ \
  X(SYSEVT_GPS_LOST,            "gps_lost")             /* fix lost (10s hold-down) */ \
  X(SYSEVT_BUTTON,              "button")               /* gamepad/encoder button PRESS  subject=button name */ \
  X(SYSEVT_FM_RDS_STATION,      "fm_rds_station")       /* RDS station identified  subject=name detail=freq */ \
  /* --- Voice / AI / media --- */ \
  X(SYSEVT_VOICE_WAKE,          "voice_wake")           /* wake word detected */ \
  X(SYSEVT_VOICE_COMMAND,       "voice_command")        /* subject=command (successful commands only) */ \
  X(SYSEVT_EI_DETECTED,         "ei_detected")          /* Edge Impulse object confirmed  subject=label detail=confidence */ \
  X(SYSEVT_EI_LOST,             "ei_lost")              /* tracked object gone (2s timeout)  subject=label */ \
  X(SYSEVT_PHOTO_SAVED,         "photo_saved")          /* subject=filename detail=path */ \
  X(SYSEVT_VIDEO_SAVED,         "video_saved")          /* subject=filename detail=frames */ \
  X(SYSEVT_MIC_SAVED,           "mic_saved")            /* recording saved  subject=filename */ \
  X(SYSEVT_LLM_GEN_DONE,        "llm_gen_done")         /* subject=stop reason detail=tokens */ \
  X(SYSEVT_LLM_MODEL_LOADED,    "llm_model_loaded")     /* subject=model filename */ \
  /* --- Lifecycle --- */ \
  X(SYSEVT_BOOT,                "boot")                 /* device finished booting — posted at end of setup  subject=reset-reason detail="boot #N" */ \
  X(SYSEVT_REBOOT,              "reboot")               /* intentional restart — posted on the NEXT boot (the ring can't survive the restart)  subject=reason(command|setup|g2|factory|software) detail=reset-reason */ \
  X(SYSEVT_CRASH,               "crash")                /* unexpected reset (panic/watchdog/brownout/lockup) — posted on next boot  subject=reset-reason detail="boot #N crashCount=M" */ \
  /* --- Security / trust (coverage) --- */ \
  X(SYSEVT_PEER_UNPAIRED,       "peer_unpaired")        /* a paired peer removed from the trust list  subject=name detail=mac */ \
  X(SYSEVT_IDENTITY_REGEN,      "identity_regenerated") /* this device's ESP-NOW identity keypair rotated (invalidates all bonds)  subject=pubkey */ \
  X(SYSEVT_USER_PROMOTED,       "user_promoted")        /* account granted admin  subject=username */ \
  X(SYSEVT_USER_DEMOTED,        "user_demoted")         /* admin privileges revoked  subject=username */ \
  X(SYSEVT_USER_BANNED,         "user_banned")          /* account suspended (sessions kicked)  subject=username */ \
  X(SYSEVT_IP_BANNED,           "ip_banned")            /* an IP added to the ban list  subject=ip */ \
  X(SYSEVT_LOGIN_LOCKED,        "login_locked")         /* an IP rate-limit locked out after failed logins  subject=ip detail=seconds */ \
  X(SYSEVT_VOICE_ARMED,         "voice_armed")          /* voice pipeline armed to run commands as a user  subject=username */ \
  X(SYSEVT_STORAGE_FORMATTED,   "storage_formatted")    /* a storage volume wiped  subject=sd|flash */ \
  X(SYSEVT_G2_SILENT_MODE,      "g2_silent_mode")       /* glasses silent/DND toggled  subject=on|off */ \
  /* --- Service / engine faults (coverage) --- */ \
  X(SYSEVT_MQTT_START_FAILED,   "mqtt_start_failed")    /* MQTT failed to start  subject=reason */ \
  X(SYSEVT_LLM_LOAD_FAILED,     "llm_load_failed")      /* on-device model load failed  subject=reason */ \
  X(SYSEVT_LLM_STATE_CORRUPT,   "llm_state_corrupt")    /* unrecoverable LLM engine fault  subject=detail */ \
  /* --- Sensors / capability (coverage) --- */ \
  X(SYSEVT_MOTION_DETECTED,     "motion_detected")      /* STHS34PF80 motion algorithm edge  subject=detected|cleared */ \
  X(SYSEVT_RTC_POWER_LOSS,      "rtc_power_loss")       /* RTC lost power (dead coin cell), kept time invalid */ \
  /* --- Recording / automation (coverage) --- */ \
  X(SYSEVT_MIC_RECORD_STARTED,  "mic_record_started")   /* microphone recording to a file started  subject=filename */ \
  X(SYSEVT_AUTOMATION_FIRED,    "automation_fired")     /* an automation matched and ran its commands  subject=name detail=trigger */ \
  /* --- Service / connectivity lifecycle (tier 2) --- */ \
  X(SYSEVT_HTTP_SERVER_STARTED, "http_server_started")  /* web server came up  subject=http|https detail=port */ \
  X(SYSEVT_HTTP_SERVER_STOPPED, "http_server_stopped")  /* web server shut down */ \
  X(SYSEVT_BLE_ON,              "ble_on")               /* BLE radio stack started */ \
  X(SYSEVT_BLE_OFF,             "ble_off")              /* BLE radio stack stopped */ \
  X(SYSEVT_MQTT_EXT_SENSOR_NEW, "mqtt_ext_sensor_new")  /* new external sensor discovered via MQTT  subject=topic */ \
  X(SYSEVT_CERT_GENERATED,      "cert_generated")       /* TLS certificate generated  subject=cn|type */ \
  /* --- Sessions / access (tier 2) --- */ \
  X(SYSEVT_LOGOUT,              "logout")               /* a session ended  subject=username detail=reason */ \
  X(SYSEVT_USER_REJECTED,       "user_rejected")        /* a pending user request was denied  subject=username */ \
  X(SYSEVT_COMMAND_DENIED,      "command_denied")       /* a logged-in user was denied a privileged command  subject=user detail=command */ \
  X(SYSEVT_AUTH_DB_FAULT,       "auth_db_fault")        /* the user/credential database failed to load or is corrupt */ \
  /* --- Config / device lifecycle (tier 2) --- */ \
  X(SYSEVT_FACTORY_RESET,       "factory_reset")        /* accounts wiped, device reset to setup  subject=actor */ \
  X(SYSEVT_FEATURE_TOGGLED,     "feature_toggled")      /* a subsystem/feature enabled or disabled  subject=feature detail=on|off */ \
  X(SYSEVT_FIRMWARE_CHANGED,    "firmware_changed")     /* running firmware differs from last boot (update applied)  subject=old->new */ \
  X(SYSEVT_BACKUP_CREATED,      "backup_created")       /* a config backup was exported  subject=categories */ \
  X(SYSEVT_BACKUP_RESTORED,     "backup_restored")      /* a config backup was restored  subject=categories */ \
  X(SYSEVT_CONFIG_FILE_CORRUPT, "config_file_corrupt")  /* a critical config file failed its integrity check  subject=file */ \
  X(SYSEVT_SECRET_DECRYPT_FAILED,"secret_decrypt_failed")/* a stored secret failed to decrypt at load  subject=which */ \
  X(SYSEVT_SD_WRITE_RECOVERED,  "sd_write_recovered")   /* SD writes resumed after a fault  subject=sd */ \
  X(SYSEVT_POWER_MODE_CHANGED,  "power_mode_changed")   /* power mode toggled  subject=mode */ \
  X(SYSEVT_BATTERY_FULL,        "battery_full")         /* battery reached full charge  subject=percent */ \
  /* --- ESP-NOW / streaming (tier 2) --- */ \
  X(SYSEVT_MESH_PASSPHRASE_CHANGED,"mesh_passphrase_changed") /* mesh group key set/changed/cleared  subject=mesh detail=set|cleared */ \
  X(SYSEVT_REMOTE_STREAM_STARTED,"remote_stream_started") /* a peer started tapping this device's output  subject=peer */ \
  X(SYSEVT_FILE_RX_FAILED,      "file_rx_failed")       /* an inbound ESP-NOW file transfer failed  subject=sender detail=filename */ \
  /* --- Glasses / recording (tier 2) --- */ \
  X(SYSEVT_G2_HIJACK_ENTERED,   "g2_hijack_entered")    /* interactive glasses-driven session started  subject=page */ \
  X(SYSEVT_G2_HIJACK_EXITED,    "g2_hijack_exited")     /* glasses-driven session ended  subject=reason */ \
  X(SYSEVT_VIDEO_RECORD_STARTED,"video_record_started") /* video recording began  subject=filename */ \
  /* --- Sensors / AI (tier 2) --- */ \
  X(SYSEVT_THERMAL_HOT_ALERT,   "thermal_hot_alert")    /* thermal camera saw an over-temp hotspot  subject=maxC */ \
  X(SYSEVT_TOF_OBJECT_DETECTED, "tof_object_detected")  /* ToF sensor sees an object near/far edge  subject=near|far detail=mm */ \
  X(SYSEVT_FM_TUNED,            "fm_tuned")             /* FM radio tuned  subject=MHz */ \
  X(SYSEVT_EI_CONTINUOUS_STARTED,"ei_continuous_started")/* continuous camera-AI detection mode started */ \
  X(SYSEVT_IMU_WALKING,         "imu_walking")          /* step detector edge  subject=started|stopped detail=cadence */ \
  /* --- Voice / model / UI (tier 2) --- */ \
  X(SYSEVT_VOICE_DISARMED,      "voice_disarmed")       /* voice command execution disarmed  subject=username */ \
  X(SYSEVT_LLM_MODEL_UNLOADED,  "llm_model_unloaded")   /* on-device model unloaded (assistant offline) */ \
  X(SYSEVT_DISPLAY_INIT_FAILED, "display_init_failed")  /* the OLED failed to initialize at boot  subject=address */ \
  /* --- Automation CRUD (tier 2) --- */ \
  X(SYSEVT_AUTOMATION_ADDED,    "automation_added")     /* an automation was created  subject=name */ \
  X(SYSEVT_AUTOMATION_DELETED,  "automation_deleted")   /* an automation was deleted  subject=name */ \
  X(SYSEVT_AUTOMATION_ACTION_DROPPED,"automation_action_dropped") /* an automation action was skipped/failed mid-run  subject=name detail=reason */

enum SystemEventKind : uint8_t {
  SYSEVT_NONE = 0,
#define SYSEVT_X(sym, name) sym,
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
