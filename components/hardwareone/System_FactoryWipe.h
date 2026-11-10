#pragma once

// ============================================================================
// System_FactoryWipe — the handover wipe behind `factoryreset all`
// ============================================================================
// Plain `factoryreset` removes the accounts and their data and keeps the
// device configuration. `factoryreset all` also asks for this wipe: at the
// next boot, before the filesystem is mounted, the whole littlefs partition
// and the NVS partition are erased. That removes saved WiFi networks, MQTT and
// LLM credentials, ESP-NOW identity and keys, Bluetooth bonds, the recovery
// credential, logs and every other stored file. The blank filesystem is then
// formatted by the ordinary first-boot path and first-time setup runs.
//
// It runs at boot because deleting files from the running system is not a
// wipe: tasks could write settings back before the restart, deleted contents
// stay in flash, and the key that protects stored secrets is derived from the
// chip and survives. Erasing the partitions removes the data itself.
//
// Kept: the firmware, the recovery updater, the speech model partition, the
// NVS key partition and the account-id high-water mark (ids stay reserved for
// data on an SD card that was not mounted). Not reached: the SD card, and on
// boards with a radio companion chip the WiFi network that chip last joined.
//
// Order and retry rules: FactoryWipePolicy.h.

// Stores the request. False when it could not be made durable.
bool factoryWipeArm();

// Drops a stored request.
void factoryWipeDisarm();

// Early setup, before initFilesystem(): runs the wipe when one was requested.
void factoryWipeRunIfArmed();

// What factoryWipeRunIfArmed() did this boot, for the system log once it is
// available; nullptr when no wipe was requested.
const char* factoryWipeBootReport();
