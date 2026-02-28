# Bond Mode Refactoring Notes

## Completed Renames (code symbols)

- ✅ `ESPNOW_V3_TYPE_PAIR_CAP_REQ` → `ESPNOW_V3_TYPE_BOND_CAP_REQ`
- ✅ `ESPNOW_V3_TYPE_PAIR_CAP_RESP` → `ESPNOW_V3_TYPE_BOND_CAP_RESP`
- ✅ `MSG_TYPE_PAIR_CAP_REQ` → `MSG_TYPE_BOND_CAP_REQ` (wire value kept as "PAIR_CAP_REQ" for compat)
- ✅ `MSG_TYPE_PAIR_CAP_RESP` → `MSG_TYPE_BOND_CAP_RESP`
- ✅ `MSG_TYPE_PAIR_MANIFEST_REQ` → `MSG_TYPE_BOND_MANIFEST_REQ`
- ✅ `MSG_TYPE_PAIR_MANIFEST_RESP` → `MSG_TYPE_BOND_MANIFEST_RESP`
- ✅ `processPairedHeartbeats()` → `processBondHeartbeats()` (comment refs)
- ✅ `requestPairedSettings()` → `requestBondSettings()`
- ✅ `sendPairedSettings()` → `sendBondSettings()` (incl extern in System_Settings.cpp)
- ✅ `gPairedSensorSeqNum` → `gBondSensorSeqNum`
- ✅ `streamPairInner()` → `streamBondInner()`
- ✅ `streamPairContent()` → `streamBondContent()`
- ✅ `registerPairHandlers()` → `registerBondHandlers()` (incl WebServer_Server.cpp call)
- ✅ `[PAIR_TEST]` log prefix → `[BOND_TEST]`
- ✅ Settings display labels: "Paired Mode Enabled" → "Bond Mode Enabled"
- ✅ Settings display labels: "Paired Role" → "Bond Role"
- ✅ Settings display labels: "Paired Peer MAC" → "Bond Peer MAC"
- ✅ Section comments: "Paired Mode CLI Commands" → "Bond Mode CLI Commands"
- ✅ Section comments: "Settings Sync for Paired Mode" → "Settings Sync for Bond Mode"
- ✅ Doc comments updated in System_ESPNow.cpp, OLED_RemoteSettings.h

## Additional Cosmetic Renames (all completed)

- ✅ OLED_RemoteSettings.cpp: "Get bonded peer MAC", "Not bonded", "bond mode disabled"
- ✅ OLED_Mode_Remote.cpp: file header "Remote device UI for bond mode"
- ✅ OLED_Utils.cpp: "Data Source Selection (for bond mode)", "Not bonded", etc.
- ✅ System_Settings.h: all bond field comments updated to "bonded peer"
- ✅ System_ESPNow_Sensors.h: "bonded device or mesh workers"
- ✅ OLED_Mode_Network.cpp: menu name "Pair" → "Bond", comment updated
- ✅ OLED_Mode_UnifiedMenu.cpp: "bonded remote device", "Build remote items if bonded"
- ✅ System_Utils.cpp: "sent to bonded device" in remote command routing
- ✅ WebPage_Pair.cpp: all internal names (`webBondSendChunk`, `refreshBond`), JS UI strings ("Bonded Device", "bond connect"), JSON field `"bonded"`, console log prefixes `[Bond]`

## Preserved "pair/paired" (ESP-NOW pairing, NOT bonding)

These use "pair" correctly — they refer to the ESP-NOW device registry, not bond mode:

- `isPaired` (local var in v3 callback — checks devices[] array)
- `UnpairedDevice`, `unpairedDevices`, `unpairedDeviceCount`, `MAX_UNPAIRED_DEVICES`
- `updateUnpairedDevice()`, `removeFromUnpairedList()`
- `ReceivedMessage::isPaired`
- "Save/Load named ESP-NOW devices (paired devices with names/keys)"
- "Paired Devices: N" status output
- "Heartbeats sent only to paired devices"
- "No paired devices to broadcast to"
- "Use 'espnow pair' or 'espnow pairsecure'"
- "freshly-paired devices never exchange heartbeats"
- "Verify the MAC is in the paired device list"
- "Device not found. Use 'espnow devices' to see paired devices."
