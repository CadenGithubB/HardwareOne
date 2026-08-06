#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HW1_RECOVERY_CREDENTIAL_CAPACITY 64u

/* Shared by the HTTP /status handler and the serial `status` console command so
 * the two buffers can never drift apart.  Both render the SAME JSON document;
 * when one grew past the other's buffer the console silently truncated. */
#define STATUS_JSON_MAX 2048u

typedef struct {
    char ap_password[HW1_RECOVERY_CREDENTIAL_CAPACITY];
    char auth_token[HW1_RECOVERY_CREDENTIAL_CAPACITY];
} recovery_credentials_t;

/* Live authentication-throttle counters for the /status document, so an
 * operator can see that the AP is being attacked rather than merely slow. */
void recovery_network_auth_stats(uint32_t *blocked_peers,
                                 uint32_t *failures_total);

typedef enum {
    RECOVERY_ACTION_APPLY_STAGED = 1,
    RECOVERY_ACTION_CANCEL = 2,
    RECOVERY_ACTION_REBOOT = 3,
    RECOVERY_ACTION_ALLOW_DOWNGRADE = 4,
} recovery_action_t;

typedef int (*recovery_upload_recv_fn)(void *context, uint8_t *buffer,
                                       size_t size);
typedef struct {
    size_t content_length;
    recovery_upload_recv_fn receive;
    void *context;
} recovery_upload_t;

typedef esp_err_t (*recovery_action_callback_t)(recovery_action_t action,
                                                 void *context);
typedef size_t (*recovery_status_callback_t)(char *buffer, size_t buffer_size,
                                             void *context);
typedef void (*recovery_activity_callback_t)(void *context);
typedef esp_err_t (*recovery_manifest_callback_t)(const uint8_t *body,
                                                   size_t body_size,
                                                   char *reason,
                                                   size_t reason_size,
                                                   void *context);
typedef esp_err_t (*recovery_firmware_callback_t)(recovery_upload_t *upload,
                                                   char *reason,
                                                   size_t reason_size,
                                                   void *context);

typedef struct {
    recovery_action_callback_t action;
    recovery_status_callback_t status;
    recovery_activity_callback_t activity;
    recovery_manifest_callback_t manifest;
    recovery_firmware_callback_t firmware;
    void *context;
} recovery_network_callbacks_t;

esp_err_t recovery_network_start(
    const recovery_credentials_t *credentials,
    const recovery_network_callbacks_t *callbacks);
void recovery_network_stop(void);

#ifdef __cplusplus
}
#endif
