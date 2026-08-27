// System_EventCatalogRows.h - single private owner of System Event metadata.
//
// This file is intentionally repeat-included and intentionally has no include
// guard. Define exactly one row macro before including it, then undefine that
// macro immediately afterward. Interface adapters must use System_EventCatalog
// instead of including this private row source.

#if defined(HW1_EVENT_CATALOG_FAMILY_ROW) == defined(HW1_EVENT_CATALOG_KIND_ROW)
#error "define exactly one System Event catalog row macro"
#endif

#ifdef HW1_EVENT_CATALOG_FAMILY_ROW
HW1_EVENT_CATALOG_FAMILY_ROW(SYSEVT_FAM_MESH,       "Mesh & ESP-NOW")
HW1_EVENT_CATALOG_FAMILY_ROW(SYSEVT_FAM_NETWORK,    "Connectivity")
HW1_EVENT_CATALOG_FAMILY_ROW(SYSEVT_FAM_GLASSES,    "Glasses")
HW1_EVENT_CATALOG_FAMILY_ROW(SYSEVT_FAM_SENSORS,    "Sensors")
HW1_EVENT_CATALOG_FAMILY_ROW(SYSEVT_FAM_MEDIA,      "Voice, AI & Media")
HW1_EVENT_CATALOG_FAMILY_ROW(SYSEVT_FAM_AUTH,       "Auth & Users")
HW1_EVENT_CATALOG_FAMILY_ROW(SYSEVT_FAM_SECURITY,   "Security")
HW1_EVENT_CATALOG_FAMILY_ROW(SYSEVT_FAM_POWER,      "Power & Battery")
HW1_EVENT_CATALOG_FAMILY_ROW(SYSEVT_FAM_STORAGE,    "Storage")
HW1_EVENT_CATALOG_FAMILY_ROW(SYSEVT_FAM_AUTOMATION, "Automation")
HW1_EVENT_CATALOG_FAMILY_ROW(SYSEVT_FAM_OTA,        "Firmware & OTA")
HW1_EVENT_CATALOG_FAMILY_ROW(SYSEVT_FAM_SYSTEM,     "System")
#endif

#ifdef HW1_EVENT_CATALOG_KIND_ROW
  /* --- ESP-NOW mesh / bond / pairing --- */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_PEER_ONLINE,         "peer_online", SYSEVT_FAM_MESH)          /* mesh peer heartbeat appeared     subject=name  detail=mac */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_PEER_OFFLINE,        "peer_offline", SYSEVT_FAM_MESH)         /* mesh peer heartbeat timed out    subject=name  detail=mac */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_PEER_PAIRED,         "peer_paired", SYSEVT_FAM_MESH)          /* pairing-mode auto-pair ran       subject=name  detail=mac */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_TEXT_RX,             "text_rx", SYSEVT_FAM_MESH)              /* ESP-NOW text message received    subject=sender detail=text */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_FILE_RX,             "file_rx", SYSEVT_FAM_MESH)              /* ESP-NOW file received            subject=sender detail=filename */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_BOND_ONLINE,         "bond_online", SYSEVT_FAM_MESH)          /* bond peer session/heartbeat up   subject=name  detail=mac */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_BOND_OFFLINE,        "bond_offline", SYSEVT_FAM_MESH)         /* bond peer heartbeat timed out    subject=name  detail=mac */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_BOND_REJECT,         "bond_reject", SYSEVT_FAM_MESH)          /* unpaired sender probed bond channel  subject=mac detail=count (30s cooldown) */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_ESPNOW_ON,           "espnow_on", SYSEVT_FAM_MESH)            /* ESP-NOW initialized */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_ESPNOW_OFF,          "espnow_off", SYSEVT_FAM_MESH)           /* ESP-NOW deinitialized */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_PAIR_WINDOW_OPEN,    "pair_window_open", SYSEVT_FAM_MESH)     /* pairing-mode window opened   subject=seconds */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_PAIR_WINDOW_CLOSED,  "pair_window_closed", SYSEVT_FAM_MESH)   /* pairing-mode window closed */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_MESH_PROMOTED,       "mesh_promoted", SYSEVT_FAM_MESH)        /* this device promoted to backup master */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_MESH_DEMOTED,        "mesh_demoted", SYSEVT_FAM_MESH)         /* real master returned, demoted back */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_REMOTE_CMD_RX,       "remote_cmd_rx", SYSEVT_FAM_MESH)        /* remote command ran on THIS device  subject=sender detail=command */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_REMOTE_CMD_SENT,     "remote_cmd_sent", SYSEVT_FAM_MESH)      /* remote command sent FROM this device  subject=target detail="#reqId cmd" */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_REMOTE_CMD_RESULT,   "remote_cmd_result", SYSEVT_FAM_MESH)    /* a sent remote command's result came back  subject=peer detail="#reqId ok|failed result" */
  /* --- Connectivity (WiFi / MQTT / BLE / time) --- */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_WIFI_CONNECTED,      "wifi_connected", SYSEVT_FAM_NETWORK)       /* subject=ip */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_WIFI_DISCONNECTED,   "wifi_disconnected", SYSEVT_FAM_NETWORK)
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_WIFI_CONNECT_FAILED, "wifi_connect_failed", SYSEVT_FAM_NETWORK)  /* subject=ssid detail=attempts/status */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_WIFI_NET_ADDED,      "wifi_net_added", SYSEVT_FAM_NETWORK)       /* subject=ssid */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_WIFI_NET_REMOVED,    "wifi_net_removed", SYSEVT_FAM_NETWORK)     /* subject=ssid */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_MQTT_CONNECTED,      "mqtt_connected", SYSEVT_FAM_NETWORK)       /* subject=broker host:port */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_MQTT_DISCONNECTED,   "mqtt_disconnected", SYSEVT_FAM_NETWORK)    /* subject=seconds connected */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_BLE_CONNECTED,       "ble_connected", SYSEVT_FAM_NETWORK)        /* companion BLE central connected  subject=type detail=mac */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_BLE_DISCONNECTED,    "ble_disconnected", SYSEVT_FAM_NETWORK)     /* subject=remaining connection count */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_TIME_SYNCED,         "time_synced", SYSEVT_FAM_NETWORK)          /* clock first became valid  subject=ntp|rtc|manual|ring|cm5 detail=time */
  /* --- G2 glasses --- */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_G2_CONNECTED,        "g2_connected", SYSEVT_FAM_GLASSES)         /* subject=sides (L+R/L/R) */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_G2_DISCONNECTED,     "g2_disconnected", SYSEVT_FAM_GLASSES)      /* subject=side that dropped */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_G2_WORN,             "g2_worn", SYSEVT_FAM_GLASSES)              /* plugin heartbeats resumed (picked up)  subject=side */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_G2_NOT_WORN,         "g2_not_worn", SYSEVT_FAM_GLASSES)          /* plugin heartbeats stopped (set down)   subject=side */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_RING_CONNECTED,      "ring_connected", SYSEVT_FAM_GLASSES)       /* R1 ring GATT up after setup  subject=name detail=mac */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_RING_DISCONNECTED,   "ring_disconnected", SYSEVT_FAM_GLASSES)    /* R1 ring link lost  subject=name detail=mac */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_RING_WORN,           "ring_worn", SYSEVT_FAM_GLASSES)            /* R1 wearStatus→wear (on finger)  subject=name */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_RING_NOT_WORN,       "ring_not_worn", SYSEVT_FAM_GLASSES)        /* R1 wearStatus→notWear (off finger)  subject=name */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_RING_RECONNECT_FAILED, "ring_reconnect_failed", SYSEVT_FAM_GLASSES) /* R1 connect attempt failed  subject=name detail=reason fail# elapsed; throttled 10min/streak */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_G2_RECONNECT_FAILED, "g2_reconnect_failed", SYSEVT_FAM_GLASSES)  /* glasses connect attempt failed  subject=sides sought detail=reason fail#; throttled 10min/streak */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_BLE_STACK_RECYCLED,  "ble_stack_recycled", SYSEVT_FAM_NETWORK)   /* Bluedroid host torn down + re-inited to clear a wedge  subject=wedged|reinit-failed */
  /* --- Auth / users --- */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_LOGIN_OK,            "login_ok", SYSEVT_FAM_AUTH)             /* subject=username detail=transport */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_LOGIN_FAIL,          "login_fail", SYSEVT_FAM_AUTH)           /* subject=username detail=transport */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_USER_REQUEST,        "user_request", SYSEVT_FAM_AUTH)         /* account request submitted  subject=username */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_USER_ADDED,          "user_added", SYSEVT_FAM_AUTH)           /* subject=username detail=by */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_USER_DELETED,        "user_deleted", SYSEVT_FAM_AUTH)         /* subject=username detail=by */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_USER_APPROVED,       "user_approved", SYSEVT_FAM_AUTH)        /* subject=username detail=by */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_PASSWORD_CHANGED,    "password_changed", SYSEVT_FAM_AUTH)     /* subject=username detail=self|admin-reset */
  /* --- Power / battery --- */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_USB_ON,              "usb_on", SYSEVT_FAM_POWER)
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_USB_OFF,             "usb_off", SYSEVT_FAM_POWER)
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_CHARGING_STARTED,    "charging_started", SYSEVT_FAM_POWER)     /* subject=percent */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_CHARGING_STOPPED,    "charging_stopped", SYSEVT_FAM_POWER)     /* subject=percent */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_BATTERY_LOW,         "battery_low", SYSEVT_FAM_POWER)          /* subject=percent */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_BATTERY_CRITICAL,    "battery_critical", SYSEVT_FAM_POWER)     /* subject=percent */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_POWER_SAVE_ENTER,    "power_save_enter", SYSEVT_FAM_POWER)
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_POWER_SAVE_EXIT,     "power_save_exit", SYSEVT_FAM_POWER)
  /* --- Storage --- */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_SD_MOUNTED,          "sd_mounted", SYSEVT_FAM_STORAGE)           /* subject=free MB */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_SD_UNMOUNTED,        "sd_unmounted", SYSEVT_FAM_STORAGE)
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_SD_WRITE_FAILED,     "sd_write_failed", SYSEVT_FAM_STORAGE)      /* SD went unwritable (once per episode) */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_FS_LOW_SPACE,        "fs_low_space", SYSEVT_FAM_STORAGE)         /* flash log-overflow latch tripped (once per boot) */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_FILE_DELETED,        "file_deleted", SYSEVT_FAM_STORAGE)         /* subject=filename detail=full path */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_SETTINGS_SAVE_FAILED,"settings_save_failed", SYSEVT_FAM_STORAGE) /* subject=stage detail=file */
  /* --- System / settings / sensor lifecycle --- */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_SETTING_CHANGED,     "setting_changed", SYSEVT_FAM_SYSTEM)      /* subject=key detail=value */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_SENSOR_STARTED,      "sensor_started", SYSEVT_FAM_SENSORS)       /* subject=sensor name (successful starts only) */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_SENSOR_STOPPED,      "sensor_stopped", SYSEVT_FAM_SENSORS)       /* subject=sensor name */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_SENSOR_START_FAILED, "sensor_start_failed", SYSEVT_FAM_SENSORS)  /* subject=sensor name */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_SENSOR_FAULT,        "sensor_fault", SYSEVT_FAM_SENSORS)         /* sensor auto-disabled after repeated errors  subject=name */
  /* --- Sensor / input events --- */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_PRESENCE_DETECTED,   "presence_detected", SYSEVT_FAM_SENSORS)    /* STHS34PF80 presence trip  subject=value */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_PRESENCE_CLEARED,    "presence_cleared", SYSEVT_FAM_SENSORS)     /* presence gone (held-down) */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_GESTURE,             "gesture", SYSEVT_FAM_SENSORS)              /* APDS9960 gesture  subject=up|down|left|right */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_IMU_SHAKE,           "imu_shake", SYSEVT_FAM_SENSORS)            /* subject=intensity */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_IMU_TAP,             "imu_tap", SYSEVT_FAM_SENSORS)              /* subject=strength */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_IMU_FREEFALL,        "imu_freefall", SYSEVT_FAM_SENSORS)         /* subject=duration (posts after 150ms sustained) */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_IMU_ORIENTATION,     "imu_orientation", SYSEVT_FAM_SENSORS)      /* stable orientation change  subject=new detail=prev */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_GPS_FIX,             "gps_fix", SYSEVT_FAM_SENSORS)              /* fix acquired  subject=sats detail=lat,lon */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_GPS_LOST,            "gps_lost", SYSEVT_FAM_SENSORS)             /* fix lost (10s hold-down) */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_BUTTON,              "button", SYSEVT_FAM_SENSORS)               /* gamepad/encoder button PRESS  subject=button name */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_FM_RDS_STATION,      "fm_rds_station", SYSEVT_FAM_SENSORS)       /* RDS station identified  subject=name detail=freq */
  /* --- Voice / AI / media --- */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_VOICE_WAKE,          "voice_wake", SYSEVT_FAM_MEDIA)           /* wake word detected */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_VOICE_COMMAND,       "voice_command", SYSEVT_FAM_MEDIA)        /* subject=command (successful commands only) */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_EI_DETECTED,         "ei_detected", SYSEVT_FAM_MEDIA)          /* Edge Impulse object confirmed  subject=label detail=confidence */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_EI_LOST,             "ei_lost", SYSEVT_FAM_MEDIA)              /* tracked object gone (2s timeout)  subject=label */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_PHOTO_SAVED,         "photo_saved", SYSEVT_FAM_MEDIA)          /* subject=filename detail=path */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_VIDEO_SAVED,         "video_saved", SYSEVT_FAM_MEDIA)          /* subject=filename detail=frames */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_MIC_SAVED,           "mic_saved", SYSEVT_FAM_MEDIA)            /* recording saved  subject=filename */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_LLM_GEN_DONE,        "llm_gen_done", SYSEVT_FAM_MEDIA)         /* subject=stop reason detail=tokens */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_LLM_MODEL_LOADED,    "llm_model_loaded", SYSEVT_FAM_MEDIA)     /* subject=model filename */
  /* --- Lifecycle --- */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_BOOT_STARTED,        "boot_started", SYSEVT_FAM_SYSTEM)         /* setup entered  subject=reset-reason (boot counter is not loaded yet) */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_BOOT_FINISHED,       "boot_finished", SYSEVT_FAM_SYSTEM)        /* setup completed and is returning  subject=reset-reason detail="boot #N" */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_REBOOT,              "reboot", SYSEVT_FAM_SYSTEM)               /* intentional restart — posted on the NEXT boot (the ring can't survive the restart)  subject=reason(command|setup|g2|factory|software) detail=reset-reason */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_CRASH,               "crash", SYSEVT_FAM_SYSTEM)                /* unexpected reset (panic/watchdog/brownout/lockup) — posted on next boot  subject=reset-reason detail="boot #N crashCount=M" */
  /* --- Security / trust (coverage) --- */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_PEER_UNPAIRED,       "peer_unpaired", SYSEVT_FAM_SECURITY)        /* a paired peer removed from the trust list  subject=name detail=mac */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_IDENTITY_REGEN,      "identity_regenerated", SYSEVT_FAM_SECURITY) /* this device's ESP-NOW identity keypair rotated (invalidates all bonds)  subject=pubkey */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_USER_PROMOTED,       "user_promoted", SYSEVT_FAM_SECURITY)        /* account granted admin  subject=username */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_USER_DEMOTED,        "user_demoted", SYSEVT_FAM_SECURITY)         /* admin privileges revoked  subject=username */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_USER_BANNED,         "user_banned", SYSEVT_FAM_SECURITY)          /* account suspended (sessions kicked)  subject=username */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_IP_BANNED,           "ip_banned", SYSEVT_FAM_SECURITY)            /* an IP added to the ban list  subject=ip */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_LOGIN_LOCKED,        "login_locked", SYSEVT_FAM_SECURITY)         /* an IP rate-limit locked out after failed logins  subject=ip detail=seconds */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_VOICE_ARMED,         "voice_armed", SYSEVT_FAM_MEDIA)          /* voice pipeline armed to run commands as a user  subject=username */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_STORAGE_FORMATTED,   "storage_formatted", SYSEVT_FAM_STORAGE)    /* a storage volume wiped  subject=sd|flash */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_G2_SILENT_MODE,      "g2_silent_mode", SYSEVT_FAM_GLASSES)       /* glasses silent/DND toggled  subject=on|off */
  /* --- Service / engine faults (coverage) --- */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_MQTT_START_FAILED,   "mqtt_start_failed", SYSEVT_FAM_NETWORK)    /* MQTT failed to start  subject=reason */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_LLM_LOAD_FAILED,     "llm_load_failed", SYSEVT_FAM_MEDIA)      /* on-device model load failed  subject=reason */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_LLM_STATE_CORRUPT,   "llm_state_corrupt", SYSEVT_FAM_MEDIA)    /* unrecoverable LLM engine fault  subject=detail */
  /* --- Sensors / capability (coverage) --- */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_MOTION_DETECTED,     "motion_detected", SYSEVT_FAM_SENSORS)      /* STHS34PF80 motion algorithm edge  subject=detected|cleared */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_RTC_POWER_LOSS,      "rtc_power_loss", SYSEVT_FAM_SENSORS)       /* RTC lost power (dead coin cell), kept time invalid */
  /* --- Recording / automation (coverage) --- */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_MIC_RECORD_STARTED,  "mic_record_started", SYSEVT_FAM_MEDIA)   /* microphone recording to a file started  subject=filename */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_AUTOMATION_FIRED,    "automation_fired", SYSEVT_FAM_AUTOMATION)     /* an automation matched and ran its commands  subject=name detail=trigger */
  /* --- Service / connectivity lifecycle (tier 2) --- */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_HTTP_SERVER_STARTED, "http_server_started", SYSEVT_FAM_NETWORK)  /* web server came up  subject=http|https detail=port */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_HTTP_SERVER_STOPPED, "http_server_stopped", SYSEVT_FAM_NETWORK)  /* web server shut down */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_BLE_ON,              "ble_on", SYSEVT_FAM_NETWORK)               /* BLE radio stack started */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_BLE_OFF,             "ble_off", SYSEVT_FAM_NETWORK)              /* BLE radio stack stopped */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_MQTT_EXT_SENSOR_NEW, "mqtt_ext_sensor_new", SYSEVT_FAM_NETWORK)  /* new external sensor discovered via MQTT  subject=topic */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_CERT_GENERATED,      "cert_generated", SYSEVT_FAM_NETWORK)       /* TLS certificate generated  subject=cn|type */
  /* --- Sessions / access (tier 2) --- */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_LOGOUT,              "logout", SYSEVT_FAM_AUTH)               /* a session ended  subject=username detail=reason */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_USER_REJECTED,       "user_rejected", SYSEVT_FAM_AUTH)        /* a pending user request was denied  subject=username */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_COMMAND_DENIED,      "command_denied", SYSEVT_FAM_AUTH)       /* a logged-in user was denied a privileged command  subject=user detail=command */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_AUTH_DB_FAULT,       "auth_db_fault", SYSEVT_FAM_AUTH)        /* the user/credential database failed to load or is corrupt */
  /* --- Config / device lifecycle (tier 2) --- */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_FACTORY_RESET,       "factory_reset", SYSEVT_FAM_SYSTEM)        /* accounts wiped, device reset to setup  subject=actor */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_FEATURE_TOGGLED,     "feature_toggled", SYSEVT_FAM_SYSTEM)      /* a subsystem/feature enabled or disabled  subject=feature detail=on|off */
  /* --- Signed recovery OTA (tier 2) --- */
  /* One kind per distinct ACTION, matching the auth/security families. The
   * subsystem previously had a single ota_result carrying an 11-value state
   * machine in `subject`, which no notification rule matched, so every OTA
   * event was invisible on every surface. ota_result is kept for the generic
   * journal phase transitions; the events below are the ones an operator or an
   * incident responder actually needs to see. */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_FIRMWARE_CHANGED,    "firmware_changed", SYSEVT_FAM_OTA)         /* running firmware differs from last boot (update applied)  subject=old->new */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_OTA_RESULT,          "ota_result", SYSEVT_FAM_OTA)               /* signed OTA transaction state/result  subject=phase detail=result */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_OTA_CREDENTIAL_CHANGED,"ota_credential_changed", SYSEVT_FAM_OTA) /* recovery WPA2/HTTP credential set or cleared  subject=set|cleared detail=actor */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_OTA_UPLOAD_STARTED,  "ota_upload_started", SYSEVT_FAM_OTA)       /* firmware upload opened  subject=candidate|manifest detail=bytes */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_OTA_UPLOAD_FINISHED, "ota_upload_finished", SYSEVT_FAM_OTA)      /* firmware upload closed  subject=candidate|manifest detail=ok|reason */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_OTA_STAGED,          "ota_staged", SYSEVT_FAM_OTA)               /* candidate validated and journaled  subject=version detail=allow-downgrade|normal */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_OTA_STAGE_REJECTED,  "ota_stage_rejected", SYSEVT_FAM_OTA)       /* signature/contract check refused a candidate  subject=version detail=reason */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_OTA_TRIAL_STARTED,   "ota_trial_started", SYSEVT_FAM_OTA)        /* an unverified image is running its probation  subject=version */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_OTA_ACCEPTED,        "ota_accepted", SYSEVT_FAM_OTA)             /* trial image marked valid  subject=version detail=probation|provisioning */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_OTA_ROLLED_BACK,     "ota_rolled_back", SYSEVT_FAM_OTA)          /* trial image rejected and rolled back  subject=cause detail=uptime/diagnostic */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_OTA_RECOVERY_ENTERED,"ota_recovery_entered", SYSEVT_FAM_OTA)     /* device left the OS for the recovery updater  subject=operator|crash_loop|storage_fault */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_BACKUP_CREATED,      "backup_created", SYSEVT_FAM_SYSTEM)       /* a config backup was exported  subject=categories */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_BACKUP_RESTORED,     "backup_restored", SYSEVT_FAM_SYSTEM)      /* a config backup was restored  subject=categories */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_CONFIG_FILE_CORRUPT, "config_file_corrupt", SYSEVT_FAM_SYSTEM)  /* a critical config file failed its integrity check  subject=file */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_SECRET_DECRYPT_FAILED,"secret_decrypt_failed", SYSEVT_FAM_SYSTEM)/* a stored secret failed to decrypt at load  subject=which */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_SD_WRITE_RECOVERED,  "sd_write_recovered", SYSEVT_FAM_STORAGE)   /* SD writes resumed after a fault  subject=sd */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_POWER_MODE_CHANGED,  "power_mode_changed", SYSEVT_FAM_POWER)   /* power mode toggled  subject=mode */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_BATTERY_FULL,        "battery_full", SYSEVT_FAM_POWER)         /* battery reached full charge  subject=percent */
  /* --- ESP-NOW / streaming (tier 2) --- */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_MESH_PASSPHRASE_CHANGED,"mesh_passphrase_changed", SYSEVT_FAM_MESH) /* mesh group key set/changed/cleared  subject=mesh detail=set|cleared */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_REMOTE_STREAM_STARTED,"remote_stream_started", SYSEVT_FAM_MESH) /* a peer started tapping this device's output  subject=peer */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_FILE_RX_FAILED,      "file_rx_failed", SYSEVT_FAM_MESH)       /* an inbound ESP-NOW file transfer failed  subject=sender detail=filename */
  /* --- Glasses / recording (tier 2) --- */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_G2_HIJACK_ENTERED,   "g2_hijack_entered", SYSEVT_FAM_GLASSES)    /* interactive glasses-driven session started  subject=page */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_G2_HIJACK_EXITED,    "g2_hijack_exited", SYSEVT_FAM_GLASSES)     /* glasses-driven session ended  subject=reason */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_VIDEO_RECORD_STARTED,"video_record_started", SYSEVT_FAM_MEDIA) /* video recording began  subject=filename */
  /* --- Sensors / AI (tier 2) --- */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_THERMAL_HOT_ALERT,   "thermal_hot_alert", SYSEVT_FAM_SENSORS)    /* thermal camera saw an over-temp hotspot  subject=maxC */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_TOF_OBJECT_DETECTED, "tof_object_detected", SYSEVT_FAM_SENSORS)  /* ToF sensor sees an object near/far edge  subject=near|far detail=mm */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_FM_TUNED,            "fm_tuned", SYSEVT_FAM_SENSORS)             /* FM radio tuned  subject=MHz */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_EI_CONTINUOUS_STARTED,"ei_continuous_started", SYSEVT_FAM_MEDIA)/* continuous camera-AI detection mode started */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_IMU_WALKING,         "imu_walking", SYSEVT_FAM_SENSORS)          /* step detector edge  subject=started|stopped detail=cadence */
  /* --- Voice / model / UI (tier 2) --- */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_VOICE_DISARMED,      "voice_disarmed", SYSEVT_FAM_MEDIA)       /* voice command execution disarmed  subject=username */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_LLM_MODEL_UNLOADED,  "llm_model_unloaded", SYSEVT_FAM_MEDIA)   /* on-device model unloaded (assistant offline) */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_DISPLAY_INIT_FAILED, "display_init_failed", SYSEVT_FAM_SYSTEM)  /* the OLED failed to initialize at boot  subject=address */
  /* --- Automation CRUD (tier 2) --- */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_AUTOMATION_ADDED,    "automation_added", SYSEVT_FAM_AUTOMATION)     /* an automation was created  subject=name */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_AUTOMATION_DELETED,  "automation_deleted", SYSEVT_FAM_AUTOMATION)   /* an automation was deleted  subject=name */
HW1_EVENT_CATALOG_KIND_ROW(SYSEVT_AUTOMATION_ACTION_DROPPED,"automation_action_dropped", SYSEVT_FAM_AUTOMATION) /* an automation action was skipped/failed mid-run  subject=name detail=reason */
#endif
