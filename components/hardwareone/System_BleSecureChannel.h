#ifndef SYSTEM_BLE_SECURE_CHANNEL_H
#define SYSTEM_BLE_SECURE_CHANNEL_H

#include <Arduino.h>
#include "System_BuildConfig.h"

#if ENABLE_BLUETOOTH

#include <stdint.h>
#include <stddef.h>
#include "System_User.h"   // TransportSessionEpoch

// ============================================================================
// HardwareOne Secure Channel v1 — app-layer encryption over the BLE GATT
// command service (NOT BLE link-layer bonding). Mirrors the ESP-NOW model:
// X25519 ephemeral key agreement + a pre-shared key (PSK), HKDF-SHA256, and
// ChaCha20-Poly1305-IETF AEAD. No OS pairing, no bonds — works identically on
// every phone/OS (the GrapheneOS removeBond trap does not apply).
//
// WIRE FORMAT (build the Android side to match byte-for-byte). All integers
// big-endian. Each GATT write/notify = type(1) || body:
//   0x01 HELLO        body = appEphPub(32) || appNonce(16)
//   0x02 HELLO_ACK    body = devEphPub(32) || devNonce(16)
//   0x03 CONFIRM      body = ct(2) || tag(16)     (AEAD of "ok", key K_c2d, ctr 0)
//   0x04 CONFIRM_ACK  body = ct(2) || tag(16)     (AEAD of "ok", key K_d2c, ctr 0)
//   0x10 DATA         body = ctr(8) || ct(N) || tag(16)
//   0x05 REJECT       body = reason(1)            (device->app only; handshake refused)
//                     reason 0x01 = no passphrase set on device (run `blesecret`)
//                     reason 0x02 = auth failed (wrong passphrase or tampering/MITM)
//                     reason 0x03 = no channel (the G2 client host's console on the
//                                   phone pipe's link only): a DATA frame reached a
//                                   console session that holds no channel, because
//                                   the device renewed the session while the link
//                                   stayed up (bledisconnect, bleconsole off/on,
//                                   blesecret cleared and set again).
//                                   Run the handshake again, then log in. Sent once
//                                   per session, for its first such frame.
//     REJECT is sent best-effort right before the device aborts a handshake, so
//     the app shows a real cause instead of a generic timeout. Treat any other
//     reason byte as a generic auth failure.
//
//   ss  = X25519(ownEphPriv, peerEphPub)
//   PSK = PBKDF2-HMAC-SHA256(passphrase, salt="HW1-SC-v1", iters=100000, 32B)
//   K   = HKDF-SHA256(ikm = ss||PSK, salt = appNonce||devNonce, info="HW1-SC-v1", 64B)
//         K_c2d = K[0:32]  (app->device)   K_d2c = K[32:64]  (device->app)
//   AEAD nonce(12) = dirTag(4) || ctr(8);  dirTag = 0x00000000 (c2d) / 0x00000001 (d2c)
//   per-direction ctr strictly increasing (CONFIRM=0, DATA=1,2,3,...) -> replay-safe
//
// login still runs *inside* the channel (confidentiality vs. authorization).
// ============================================================================

enum BleScResult {
  BLE_SC_NOT_A_FRAME = 0,   // input isn't a channel frame (plaintext command)
  BLE_SC_CONSUMED,          // handshake frame handled; reply already sent
  BLE_SC_PLAINTEXT_READY,   // DATA frame decrypted; out[] holds the command line
  BLE_SC_BINARY_READY,      // DATA frame decrypted; out[] holds an app binary envelope
  BLE_SC_ERROR,             // malformed / auth fail / replay — ignore the input
};

// Reset per-connection channel state (call on connect AND disconnect).
void bleScReset(uint16_t connId);

// True once the handshake for this connection has completed (CONFIRM verified).
bool bleScEstablished(uint16_t connId);

// True if the operator requires a secure channel (setting + a secret is set).
// When true, plaintext commands are refused and only DATA frames are executed.
bool bleScRequired();

// Drop the cached PSK so it's re-derived from the (changed) passphrase.
void bleScInvalidatePsk();

// Create the tx mutex. Call once during BLE init before any traffic.
void bleScInit();

// Eagerly derive + cache the PSK (PBKDF2 is ~expensive). Call from a large-stack,
// non-time-critical context (BLE init, or the cmd_exec task when the secret is
// set) so the per-connection handshake on BTC_TASK never pays the PBKDF2 cost.
void bleScWarmPsk();

// Process one inbound GATT write. Handshake frames are handled internally:
// `epoch` is the session epoch captured with the frame, and every handshake
// reply (HELLO_ACK, CONFIRM_ACK, REJECT) goes to that (connId, epoch) only,
// through the directed raw sender. A DATA frame is decrypted into
// `out` with length in *outLen. Ordinary command plaintext is NUL-terminated
// and returns BLE_SC_PLAINTEXT_READY. A recognized binary application envelope
// is byte-exact (not text-filtered) and returns BLE_SC_BINARY_READY.
BleScResult bleScHandleInbound(uint16_t connId, TransportSessionEpoch epoch, const uint8_t* data,
                               size_t len, char* out, size_t outCap, size_t* outLen);

// REJECT reason 0x03 to this (connId, epoch) only, through the directed raw
// sender: the app's DATA frame found no channel on its console session (the G2
// client host renewed it under a live link). Call from cmd_exec, never BTC.
void bleScRejectNoChannel(uint16_t connId, TransportSessionEpoch epoch);

// Encrypt a plaintext reply as one or more DATA frames and notify them to this
// connection's current session only. Chunks to fit the negotiated MTU. Returns
// false if not established or the connection has no live session epoch.
// blocking=false try-locks the tx mutex (best-effort console mirror); true waits (command
// results). See the .cpp for why the debug path must not block.
// binaryFrame=true stamps the app-frame header with ver=0x02 so the app treats the
// reassembled message as an opaque byte blob (no UTF-8 decode / no console echo) — used by
// `fileread ... bin` to ship raw file bytes without base64. Payload is still AEAD-encrypted.
bool bleScSendEncrypted(uint16_t connId, const char* plaintext, size_t len, bool blocking = true,
                        bool binaryFrame = false);

// A binding captured while the caller holds the BLE lifecycle mutex. The slot
// generation changes on every allocation, reset and new handshake, so a sender
// that has since released that mutex still refuses to encrypt for a
// replacement peer or a re-keyed channel. `epoch` is the session the frames are
// addressed to. bleScBind is true only for an established channel and a
// non-zero epoch; the caller has already matched the epoch to the connection.
struct BleScBinding { uint16_t connId = 0; TransportSessionEpoch epoch = 0; uint32_t generation = 0; };
bool bleScBind(uint16_t connId, TransportSessionEpoch epoch, BleScBinding* out);
// bleScSendEncrypted with the binding re-validated before every fragment (slot
// generation and live session epoch), so the lifecycle mutex need not be held
// across the paced transfer. Every fragment goes to binding.connId only, and the
// directed sender re-checks binding.epoch under the lifecycle mutex.
bool bleScSendEncryptedBound(const BleScBinding& binding, const char* plaintext, size_t len,
                             bool blocking, bool binaryFrame = false);

#endif // ENABLE_BLUETOOTH
#endif // SYSTEM_BLE_SECURE_CHANNEL_H
