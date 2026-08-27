# Resisting local reflashing: Secure Boot, flash encryption, and credentials

Design analysis, 2026-08-05. Produced by a 5-agent pass (silicon mechanisms, OTA-architecture
interaction, credential layer, adversarial irreversibility review, synthesis), each grounded in
ESP-IDF 5.5.1 sources and this repo. No eFuse has been burned. Nothing here is a decision.

## The core constraint

No stored credential can block local reflashing on ESP32-S3, because the decision "may this chip enter serial download mode" is made by mask ROM from eFuse bits before any flash byte is read - your PIN, your NVS, your factory updater and your app are all downstream of that decision. The only lever that actually stops a cable is burning DIS_DOWNLOAD_MODE (BLK0 bit 128), which is permanent and blocks you identically to an attacker; Secure Boot is a boot gate and flash encryption is a confidentiality gate, and neither has any concept of a user-supplied secret. So the honest decomposition of your goal is two separate things: a PIN that gates the recovery console and the remote arming paths (pure software, reversible, ships today, and is where the real hole currently is), and an eFuse burn that gates the cable (permanent, not PIN-conditional, and only safe once the software layer exists and has been hardware-tested). A PIN can gate WHEN the burn happens; it can never gate what the burn enforces afterward, and no factory reset can undo it. Two ordering traps make the permanent half unforgiving: Secure Boot enabled without flash encryption in the SAME build permanently forecloses flash encryption on that board (verified in esp32s3/secure_boot_secure_features.c - rd_dis_now is unconditionally true when FE is not compiled in, burning WR_DIS_RD_DIS on first boot), and CONFIG_SECURE_ROM_DL_MODE_ENABLED=y in sdkconfig.ota:509 is a pre-armed default that silently burns ENABLE_SECURITY_DOWNLOAD the moment CONFIG_SECURE_BOOT=y makes the choice at Kconfig.projbuild:1123-1127 visible. Everything below Tier 4 is worth doing now; everything at Tier 4 and above should wait for a fleet and real key custody.

## Tiers

### Tier 0: Key custody and stale-config hygiene

**REVERSIBLE**

**Gives:** A signing key that survives a reboot, with a rehearsed restore, and a build tree with no dead key paths. Correction to the tracks: the key is NOT gone - /Users/morgan/.hardwareone/keys/hw1-ota-signing.pem exists (2484 bytes, mode 0600), and sdkconfig.ota:500 and updater/sdkconfig.feathers3:500 both point at it. Only updater/sdkconfig.feathers3_fe:500 still names /private/tmp/hw1-updater-test-key.pem, and updater/CMakeLists.txt:74-92 strips that line and substitutes HW1_OTA_SIGNING_KEY with a FATAL_ERROR if unreadable, so it is inert today but is a live footgun the moment anyone builds that board outside the wrapper.

**Costs:** An hour. An encrypted offline backup (not iCloud-synced, not /tmp), a written restore procedure that has actually been executed once, and a decision about the maybe-keep/ directory sitting next to the key.

**Prerequisites:** None. This is the cheapest item on the entire list and everything from Tier 4 onward is existential without it.

### Tier 1: Recovery console credential gate plus physical-presence reset - the PIN the user actually asked for, delivered in software

**REVERSIBLE**

**Gives:** This is where the user's stated goal genuinely lands. An `unlock <credential>` session gate in console_task (updater/main/updater_main.c:1789) covering setpin, apply, allowdowngrade and resetjournal, additive to the existing serial-only restrictions on CONTROL_START_NETWORK (:1726-1735) and CONTROL_RESET_JOURNAL (:1744-1750), with status/help/cancel/reboot left ungated forever. Paired with a GPIO0-held-~10s-after-boot credential reset (never sampled at reset - GPIO0 is the download-mode strapping pin). Closes the real hole: today five seconds of physical access lets anyone `setpin` themselves in, and store_recovery_pin (:522) writes that string to both ap_pass and auth_token, converting transient local access into persistent remote ownership of the recovery SoftAP. Also fixes note_activity(NULL) at :1814, which fires before parsing and lets any garbage line pin the device in recovery indefinitely.

**Costs:** Does not block reflashing at all - the gate lives in the binary an attacker replaces. Throttle state is RAM-resident, so a physical attacker resets to clear it; document it as anti-casual-guessing, not anti-physical. The console credential is echoed in the operator's host terminal scrollback. Keep the ap_pass == auth_token equality invariant (recovery_network.c:510-516) for now: WPA2-PSK needs cleartext, so a PBKDF2 verifier buys nothing while the PSK sits in the same partition, and the equality check is a real half-write integrity guard.

**Prerequisites:** The gate and the hatch must land together and be hardware-tested as one change. Gate without hatch bricks on a forgotten passphrase once Tier 5 exists; hatch without gate leaves setpin open. Also add `apply <first-8-hex-of-sha256>` digest echo (status already prints it) - cheapest real win in the track, binds intent to content, no credential handling - and note it breaks any tooling or documented procedure that types a bare `apply`.

### Tier 2: Remote-path hardening and observability for otapin

**REVERSIBLE**

**STATUS: the transport half SHIPPED (2026-08-05).** `cmdOtaPin` now carries the
same `ORIGIN_SERIAL` gate as `cmdOtaResetJournal`, covering both `otapin <pass>`
and `otapin clear confirm`. It was implemented as a flat origin restriction
rather than the "destruction requires the current credential" rule below,
because the credential-re-entry variant breaks the legitimate "I forgot it,
start over" flow and a flat serial gate does not. The observability half - the
SYSEVT members, the redaction fix, and the NVS counters - is still open, and the
paragraph below still describes it accurately.

**Gives (transport half, shipped):** Inverted a backwards asymmetry: otaresetjournal was hard-restricted to ORIGIN_SERIAL while the strictly more destructive `otapin clear confirm` had only requiresSuperAdmin and was reachable from WEB, BLUETOOTH, MQTT, ESPNOW, G2_HIJACK, LOCAL_DISPLAY and VOICE. Since start_network refuses without a credential (updater_main.c:1290-1296), one hijacked super-admin command from anywhere takes recovery offline entirely, leaving the unauthenticated console as the only way in. Rule: overwrite always allowed, destruction requires current credential plus trusted origin. Plus two SYSEVT_FAM_SECURITY members (credential set / cleared - System_Events.h:227-233 has the right family and no OTA-credential member), a fixed redaction rule at System_Utils.cpp:1112 (MASK_AFTER_TOKEN_POS currently renders `otapin clear confirm` and `otapin <secret>` identically), and small monotonic counters in the hw1up NVS namespace.

**Costs:** Requiring the current credential for `otapin clear` breaks the legitimate 'I forgot it, start over' flow, so it cannot ship before Tier 1's hatch. Adding SYSEVT members touches an X-macro table where drift is a build error, so it must land atomically across every consumer. NVS records are erasable by the same cable - this is forensic and operational, not preventive; its real value is the accident case.

**Prerequisites:** Tier 1's physical-presence hatch must already exist and be tested.

### Tier 3: Layout migration, build-contract rewrite, and virtual-eFuse rehearsal - the last moment any of this is cheap

**REVERSIBLE**

**Gives:** Makes the permanent tiers physically possible. Measure the actual signed bootloader for the intended SB+FE config: check_sizes.py enforces CONFIG_PARTITION_TABLE_OFFSET(0x9000) - CONFIG_BOOTLOADER_OFFSET_IN_FLASH(0x0) = 36,864 bytes on the signed binary; the FE bootloader is 31,584 today and secure-pad-v2 makes that ALIGN_UP(x,4096)+4096 = exactly 36,864 with zero headroom before a byte of SBv2 verification code. Raising the table offset collides with nvs at 0xA000, so the whole layout slides: both CSVs, the four hard-coded offsets at updater/main/updater_preflight.c:19-24, the constants in tools/ota/check_ota_builds.py, and both _layout_id strings. Also rewrite the build contract (CMakeLists.txt:420-432 requires CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT / ..._ON_UPDATE_NO_SECURE_BOOT, both `depends on !SECURE_BOOT`; :433-443 forbids CONFIG_SECURE_BOOT outright) to assert CONFIG_SECURE_SIGNED_APPS / CONFIG_SECURE_SIGNED_ON_UPDATE / CONFIG_SECURE_BOOT_V2_ENABLED instead. And fix the live bug: littlefs-flash and encrypted-littlefs-flash are absent from _unsafe_ota_target (CMakeLists.txt:515-524) - verified - so `idf.py littlefs-flash` writes a plaintext filesystem into the encrypted littlefs partition on feathers3_fe today.

**Costs:** A destructive full-flash migration and a layout-id bump. Rehearsal via CONFIG_EFUSE_VIRTUAL / CONFIG_EFUSE_VIRTUAL_KEEP_IN_FLASH / CONFIG_BOOTLOADER_EFUSE_SECURE_VERSION_EMULATE cannot cover the final step, because CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE depends on !EFUSE_VIRTUAL || IDF_CI_BUILD.

**Prerequisites:** Must complete BEFORE any eFuse burn - after DIS_DOWNLOAD_MODE the layout can never move, and a bootloader overflow discovered post-burn is unfixable. Set CONFIG_SECURE_FLASH_REQUIRE_ALREADY_ENABLED=y on any build that must not encrypt a board by accident.

### Tier 4: Secure Boot v2 plus flash encryption Release - one build, one decision, sacrificial board first

**PERMANENT**

**Gives:** ROM-verified bootloader and app on every boot (trust root moves from the running app's own signature block into eFuse), firmware confidentiality, an encrypted partition table which incidentally closes the MD5-only partition-table gap in flash_partitions.c, and NVS encryption so the recovery credential is no longer plaintext at 0xA000. Burns SECURE_BOOT_EN (BLK0 bit 116), the key digests into BLOCK_KEY blocks, DIS_DIRECT_BOOT (129), DIS_USB_JTAG (118), HARD_DIS_JTAG (51), SOFT_DIS_JTAG (48-50), SPI_BOOT_CRYPT_CNT (82-84), DIS_DOWNLOAD_MANUAL_ENCRYPT (52), DIS_DOWNLOAD_ICACHE/DCACHE (42/43).

**Costs:** Does NOT stop a cable writing flash - this is a boot gate, not a write gate, and delivers zero of the literal stated goal. Under Release FE there is no cable path to write any app image (DIS_DOWNLOAD_MANUAL_ENCRYPT blocks --encrypt, and the device-generated XTS key is read-protected at birth so host pre-encryption is impossible), so a corrupt factory partition becomes a permanently dead board and the v0.99.8 unbrickable claim becomes false. Release mode also stops ESP-IDF generating encrypted-* targets entirely, silently disarming every FE guard rail in the root CMakeLists (lines 573, 652, 684 all test MODE_DEVELOPMENT and fall through to plaintext targets). JTAG gone forever. USB-OTG DFU gone; USB-Serial-JTAG and UART0 survive. ESPTOOLPY_NO_STUB makes all flashing slow and stubless. Power loss during the 16MB in-place encryption on first boot is a documented cable-only recovery (bootloader_utility.c:655-676).

**Prerequisites:** Tiers 0-3 complete. SB and FE MUST be in the same build - this is not a preference. Explicitly pin the ROM download-mode choice by hand in sdkconfig and assert it in the CMake contract. Burn all three digest slots externally with espefuse, or sign the bootloader with three keys, before enabling - the revoke loop at secure_boot_v2/secure_boot.c:345-354 keys off how many signature blocks are on the BOOTLOADER image, not on what you intended. Leave CONFIG_SECURE_BOOT_ENABLE_AGGRESSIVE_KEY_REVOKE off. Powered hub, no laptop battery, no hot-plugging. Budget at least two sacrificial FeatherS3 boards.

### Tier 5: DIS_DOWNLOAD_MODE - the literal goal, and a one-way door with no key

**PERMANENT**

**Gives:** The only mechanism on the chip that stops a physically-present attacker writing your flash. Burns BLK0 bit 128 via CONFIG_SECURE_DISABLE_ROM_DL_MODE, or at runtime via esp_efuse_disable_rom_download_mode() - which is callable with neither Secure Boot nor FE enabled, and is therefore the actual PIN-gated hook if you want one. Note the crucial distinction: ENABLE_SECURITY_DOWNLOAD (bit 133) is NOT a substitute - Secure Download Mode still permits basic flash write, per its own Kconfig help. Only bit 128 closes the cable.

**Costs:** Every form of cable recovery, forever. If ota_0 and factory both become unbootable the board is landfill. Bypassable with a clip or hot air if the SPI flash is an external die rather than in-package - unverified for FeatherS3 and worth confirming, though flash encryption is what makes that bypass useless. Users will set a PIN, forget it, and expect a factory reset to restore access; no mechanism can honor that, and no support process can undo it, so the UI wrapping this must say so in unmistakable terms before the burn.

**Prerequisites:** Tier 1's console gate AND physical-presence hatch shipped and hardware-tested - burning this while the console accepts setpin/apply/allowdowngrade/resetjournal from anyone with a cable does not close the door, it relocates it to a door with no lock and no possibility of ever adding one from outside. Never burn DIS_USB_SERIAL_JTAG (bit 119): ESP-IDF never burns it automatically (secure_boot_secure_features.c burns DIS_USB_JTAG 118 and HARD_DIS_JTAG 51, not 119), but a hand-rolled espefuse command would remove the transport that both the recovery console and the hatch depend on.

### Tier 6: Anti-rollback - recommended AGAINST, listed so the decision is explicit

**PERMANENT**

**Gives:** Bootloader refusal of any app whose secure_version is below the eFuse floor. On a factory + single-ota_0 layout this buys close to nothing, because there is no older app slot to roll back to.

**Costs:** Breaks the unbrickable invariant on the FIRST successful OTA, not eventually. Correcting the ota-interaction track: check_anti_rollback IS applied to the factory partition - bootloader_utility.c:595-601 loops `for (index = start_index; index >= FACTORY_INDEX; index--)` and index_to_partition (:277-279) returns bs->factory for FACTORY_INDEX. updater/ is a separate project with its own sdkconfig, so its image carries secure_version=0; the moment the app boots ESP_OTA_IMG_VALID the bootloader burns the floor to 1 and the recovery updater stops loading, silently, discovered only when a unit needs recovery. Also makes `allowdowngrade confirm` a self-wipe: esp_ota_set_boot_partition erases the whole ota_0 partition and returns ESP_ERR_OTA_SMALL_SEC_VER (esp_ota_ops.c:625-632). SECURE_VERSION is BLK0 bit 142, 16 bits, unary via __builtin_popcount - 16 lifetime bumps, burned automatically with no confirmation.

**Prerequisites:** If ever adopted: lockstep CONFIG_BOOTLOADER_APP_SECURE_VERSION across the root project and updater/ as a build-time assertion. Otherwise keep CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=n and harden updater_validate_not_downgrade() and the HW1_OTA_REQUEST_ALLOW_DOWNGRADE path in software instead.

## Blockers before ANY permanent step

- Signing key custody: back up /Users/morgan/.hardwareone/keys/hw1-ota-signing.pem offline with a restore procedure you have actually executed once, decide what maybe-keep/ is, and clean the dead /private/tmp/hw1-updater-test-key.pem string out of updater/sdkconfig.feathers3_fe:500. Under Secure Boot, key loss is fleet-permanent and unrecoverable.

- Secure Boot and flash encryption must be decided and enabled in ONE build. esp_secure_boot_enable_secure_features() in esp-idf/components/bootloader_support/src/esp32s3/secure_boot_secure_features.c sets rd_dis_now=true unconditionally and only downgrades it inside #ifdef CONFIG_SECURE_FLASH_ENC_ENABLED; with FE not compiled in it burns WR_DIS_RD_DIS on first boot, after which esp_efuse_write_key() can never read-protect an XTS-AES key and flash encryption is permanently impossible on that board. Reject any phased plan that lands Secure Boot before FE.

- Explicitly pin the ROM download-mode choice in sdkconfig by hand before enabling Secure Boot, and assert it in the CMake contract. CONFIG_SECURE_ROM_DL_MODE_ENABLED=y (sdkconfig.ota:509) plus `default SECURE_ENABLE_SECURE_ROM_DL_MODE if SECURE_ROM_DL_MODE_ENABLED` (Kconfig.projbuild:1125) means the choice materialises already armed the instant CONFIG_SECURE_BOOT=y satisfies its dependency at :1127, and first boot burns ENABLE_SECURITY_DOWNLOAD (BLK0 bit 133) with nobody having chosen it - killing espefuse.py on that chip before provisioning is finished.

- Burn all three SECURE_BOOT_DIGEST slots externally with espefuse, or sign the bootloader with three keys, at provisioning. The revoke loop at secure_boot_v2/secure_boot.c:345-354 is unconditional at that site and keys off boot_key_digests.num_digests - the number of signature blocks appended to the BOOTLOADER image. One key means SECURE_BOOT_KEY_REVOKE1 and REVOKE2 (BLK0 bits 86/87) burn on first boot and rotation is gone forever.

- Relocate the partition table and complete the destructive migration BEFORE any burn. Measure the actual signed bootloader for the intended SB+FE config first: check_sizes.py enforces 36,864 bytes and the current FE bootloader signs to exactly that with zero headroom. Moving the table cascades through partitions_ota_no_sr_16mb.csv, updater/partitions.csv, updater/main/updater_preflight.c:19-24, tools/ota/check_ota_builds.py and both _layout_id strings, and is impossible after DIS_DOWNLOAD_MODE.

- Authenticate the recovery serial console AND ship the GPIO0 physical-presence credential reset together, hardware-tested, before any download-mode burn. Both, not one. Gate-without-hatch turns a forgotten passphrase into a permanent brick because recovery_network_start refuses to raise the AP without a credential; hatch-without-gate leaves setpin open to anyone with a cable.

- Fix note_activity(NULL) at updater/main/updater_main.c:1814 - it fires before the line is parsed, so any unrecognized input refreshes the CONFIG_HW1_UPDATER_IDLE_TIMEOUT_SEC timer and process_idle_timeout (:1628) never fires. Once auth exists, failed attempts must not refresh activity.

- Add littlefs-flash and encrypted-littlefs-flash to the _unsafe_ota_target list at CMakeLists.txt:515-524. Verified absent. This is a live data-corruption bug on feathers3_fe today - littlefs_create_partition_image() calls esptool_py_flash_target_image() directly and never consults the CSV encrypted flag - and it is independent of every burn on this roadmap.

- Rewrite the build contract before it can pass. CMakeLists.txt:420-432 requires CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT and CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT (both `depends on !SECURE_BOOT`) and :433-443 forbids CONFIG_SECURE_BOOT outright; the same three appear in check_common_config() in tools/ota/check_ota_builds.py. Re-express against CONFIG_SECURE_SIGNED_APPS / CONFIG_SECURE_SIGNED_ON_UPDATE / CONFIG_SECURE_BOOT_V2_ENABLED. The guard rails are doing their job and will hard-stop the build today.

- Do not enable CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK, or if ever adopted, lockstep CONFIG_BOOTLOADER_APP_SECURE_VERSION between the root project and updater/ as a build-time assertion. check_anti_rollback IS applied to FACTORY_INDEX (bootloader_utility.c:595-601, index_to_partition at :277-279), so the recovery updater stops loading one successful OTA after the feature is turned on.

- Leave CONFIG_SECURE_BOOT_ENABLE_AGGRESSIVE_KEY_REVOKE off. Its own Kconfig help warns of permanent bricking; on a design whose premise is surviving a bad flash, it converts a transient read error into landfill.

- Set CONFIG_SECURE_FLASH_REQUIRE_ALREADY_ENABLED=y on any build that must not encrypt a board by accident, and rehearse the sequence with CONFIG_EFUSE_VIRTUAL / CONFIG_EFUSE_VIRTUAL_KEEP_IN_FLASH / CONFIG_BOOTLOADER_EFUSE_SECURE_VERSION_EMULATE, accepting that MODE_RELEASE depends on !EFUSE_VIRTUAL and so cannot be rehearsed. Budget at least two sacrificial FeatherS3 boards.

- Guarantee power stability for the first FE boot: powered hub, no laptop battery, no hot-plugging. The encrypt-flash-contents window spans an in-place encryption of 16MB and is a documented cable-only-recovery hazard (bootloader_utility.c:655-676).

- Never burn DIS_USB_SERIAL_JTAG (BLK0 bit 119). ESP-IDF never burns it automatically - secure_boot_secure_features.c burns DIS_USB_JTAG (118) and HARD_DIS_JTAG (51), not 119 - but a hand-rolled espefuse command would remove the transport the recovery console and the physical-presence hatch both live on. Exclude it explicitly from any burn checklist.

- Guard the feathers3_fe bench board: it is two eFuse writes (DIS_DOWNLOAD_MANUAL_ENCRYPT + SPI_BOOT_CRYPT_CNT to 7) from being permanently uncable-flashable, because MODE_DEVELOPMENT already burned DIS_DOWNLOAD_ICACHE/DCACHE. Note esp_flash_encryption_set_release_mode() also burns ENABLE_SECURITY_DOWNLOAD unconditionally on S3 and abort()s if the readback is not RELEASE - an abort in the app is a boot loop, not a clean error.

## Recommendation

Do Tiers 0 through 3 now. Do not do Tiers 4, 5 or 6 on this project yet.

Tier 0 is an hour and removes the only genuinely existential risk on the list. Tier 1 is the one that actually answers the user's question in the form the user can have today: an authenticated recovery console plus a physical-presence reset. It is worth being blunt with them that this is not a lesser version of what they asked for - the unauthenticated console is the real hole. Right now anyone with five seconds and a cable runs `setpin`, and store_recovery_pin writes that string to both the WPA2 PSK and the HTTP Basic password, converting transient physical access into persistent remote ownership of the recovery AP. No eFuse fixes that. Tier 1 does, it is free, and it must exist and be debugged before any permanent burn regardless, because after Tier 5 that console is the only local write path in existence.

Tier 2 fixed a genuine inversion: `otapin clear confirm` was reachable from web, BLE, MQTT, ESP-NOW, voice and the G2 hijack path, while the strictly less destructive `otaresetjournal` was serial-only. One hijacked super-admin command took recovery offline from anywhere on the planet. That was a bigger real-world exposure than anything a cable-wielding attacker represents for a device on the user's own bench. The transport restriction shipped 2026-08-05; what remains open is observability - a credential change still leaves no durable trace, and the log redaction renders `otapin <secret>` and `otapin clear confirm` identically as `otapin ***`.

Tier 3 deserves doing now specifically because it is the last moment it is cheap. The partition-table relocation is a destructive migration today and an impossibility after Tier 5, and the bootloader is already at exactly the 36,864-byte ceiling. The littlefs-flash gap is corrupting data on the bench board today, independent of any of this.

Hold Tiers 4 and 5. The honest reasons: one bench board and no fleet means the entire cost of a mistake lands on the only device you can develop on, and Tier 4's prerequisites alone (three digests burned externally, layout migrated, contract rewritten, two sacrificial boards, a powered hub) are more work than everything below it combined. There is also a shape-of-product argument - Tier 5 delivers the literal request and simultaneously destroys the unbrickable invariant that is the headline feature of v0.99.8. That trade may well be right for a shipped product with a support channel; it is close to indefensible for a pre-1.0 project with one board. And the specific thing the user should understand before ever committing: a PIN can trigger the burn but cannot reverse it, so the support case "customer forgot their PIN, factory reset it" has no answer and never will.

Revisit Tiers 4 and 5 when three things are simultaneously true: the signing key lives somewhere with real custody and a tested restore, there is more than one board so a brick is an incident rather than a stoppage, and Tier 1 has been running on hardware long enough that you trust the hatch. Tier 6 I would simply decide against and write down, so it stops resurfacing.

