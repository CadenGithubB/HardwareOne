#include "recovery_network.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"

#define AUTH_HEADER_MAX 192u
#define ORIGIN_HEADER_MAX 96u
#define STATUS_JSON_MAX 1536u
#define MANIFEST_BODY_MAX 2048u
#define MANIFEST_RECEIVE_TIMEOUT_US (45LL * 1000000LL)
#define AUTH_WINDOW_US (60LL * 1000000LL)
#define AUTH_BLOCK_US (30LL * 1000000LL)
#define AUTH_FAILURE_LIMIT 5u

static const char *TAG = "hw1up_net";

typedef struct {
    httpd_handle_t server;
    esp_netif_t *ap_netif;
    recovery_network_callbacks_t callbacks;
    char token[HW1_RECOVERY_CREDENTIAL_CAPACITY];
    bool event_loop_created;
    bool wifi_initialized;
    uint32_t auth_failures;
    int64_t auth_window_started_us;
    int64_t auth_blocked_until_us;
} recovery_network_context_t;

static recovery_network_context_t s_network;

static void log_http_stack_margin(const char *operation)
{
    ESP_LOGI(TAG, "HTTP task stack high-water after %s: %u bytes",
             operation, (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

static bool constant_time_equals(const char *left, const char *right)
{
    size_t left_len = left == NULL ? 0 : strlen(left);
    size_t right_len = right == NULL ? 0 : strlen(right);
    size_t max_len = left_len > right_len ? left_len : right_len;
    unsigned difference = (unsigned)(left_len ^ right_len);
    size_t i;
    for (i = 0; i < max_len; ++i) {
        unsigned l = i < left_len ? (unsigned char)left[i] : 0;
        unsigned r = i < right_len ? (unsigned char)right[i] : 0;
        difference |= l ^ r;
    }
    return difference == 0;
}

static void record_auth_failure(void)
{
    int64_t now = esp_timer_get_time();
    if (s_network.auth_window_started_us == 0 ||
        now - s_network.auth_window_started_us > AUTH_WINDOW_US) {
        s_network.auth_window_started_us = now;
        s_network.auth_failures = 0;
    }
    ++s_network.auth_failures;
    if (s_network.auth_failures >= AUTH_FAILURE_LIMIT) {
        s_network.auth_blocked_until_us = now + AUTH_BLOCK_US;
        s_network.auth_failures = 0;
        s_network.auth_window_started_us = now;
        ESP_LOGW(TAG, "HTTP authentication rate limit engaged for 30 seconds");
    }
}

static bool authorization_value_valid(httpd_req_t *request)
{
    size_t header_len = httpd_req_get_hdr_value_len(request, "Authorization");
    char header[AUTH_HEADER_MAX];
    unsigned char decoded[HW1_RECOVERY_CREDENTIAL_CAPACITY + 16];
    char expected[HW1_RECOVERY_CREDENTIAL_CAPACITY + 16];
    size_t decoded_len = 0;
    int written;
    if (header_len == 0 || header_len >= sizeof(header) ||
        httpd_req_get_hdr_value_str(request, "Authorization", header,
                                    sizeof(header)) != ESP_OK) {
        return false;
    }
    if (strncmp(header, "Bearer ", 7) == 0) {
        return constant_time_equals(header + 7, s_network.token);
    }
    if (strncmp(header, "Basic ", 6) != 0 ||
        mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
            (const unsigned char *)header + 6, strlen(header + 6)) != 0) {
        return false;
    }
    decoded[decoded_len] = '\0';
    written = snprintf(expected, sizeof(expected), "admin:%s", s_network.token);
    if (written < 0 || (size_t)written >= sizeof(expected)) {
        memset(decoded, 0, sizeof(decoded));
        return false;
    }
    bool valid = constant_time_equals((const char *)decoded, expected);
    memset(decoded, 0, sizeof(decoded));
    memset(expected, 0, sizeof(expected));
    return valid;
}

static bool require_authorization(httpd_req_t *request)
{
    int64_t now = esp_timer_get_time();
    if (now < s_network.auth_blocked_until_us) {
        httpd_resp_set_status(request, "429 Too Many Requests");
        httpd_resp_set_hdr(request, "Retry-After", "30");
        (void)httpd_resp_sendstr(request,
                                 "authentication temporarily rate limited");
        return false;
    }
    if (!authorization_value_valid(request)) {
        record_auth_failure();
        httpd_resp_set_hdr(request, "WWW-Authenticate",
                           "Basic realm=\"HardwareOne Recovery\"");
        (void)httpd_resp_send_err(request, HTTPD_401_UNAUTHORIZED,
                                  "authentication required");
        return false;
    }
    s_network.auth_failures = 0;
    s_network.auth_window_started_us = now;
    if (s_network.callbacks.activity != NULL) {
        s_network.callbacks.activity(s_network.callbacks.context);
    }
    return true;
}

static bool require_safe_mutation_origin(httpd_req_t *request)
{
    size_t header_len = httpd_req_get_hdr_value_len(request, "Origin");
    char origin[ORIGIN_HEADER_MAX];
    char expected[ORIGIN_HEADER_MAX];
    int written;
    if (header_len == 0) {
        /* Non-browser clients such as curl do not normally send Origin. */
        return true;
    }
    if (header_len >= sizeof(origin) ||
        httpd_req_get_hdr_value_str(request, "Origin", origin,
                                    sizeof(origin)) != ESP_OK) {
        httpd_resp_set_status(request, "403 Forbidden");
        (void)httpd_resp_sendstr(request, "invalid recovery request origin");
        return false;
    }
    if (CONFIG_HW1_UPDATER_HTTP_PORT == 80) {
        written = snprintf(expected, sizeof(expected), "http://%s",
                           CONFIG_HW1_UPDATER_AP_IP);
    } else {
        written = snprintf(expected, sizeof(expected), "http://%s:%d",
                           CONFIG_HW1_UPDATER_AP_IP,
                           CONFIG_HW1_UPDATER_HTTP_PORT);
    }
    if (written < 0 || (size_t)written >= sizeof(expected) ||
        strcmp(origin, expected) != 0) {
        httpd_resp_set_status(request, "403 Forbidden");
        (void)httpd_resp_sendstr(request,
                                 "cross-origin recovery action refused");
        return false;
    }
    return true;
}

static esp_err_t root_handler(httpd_req_t *request)
{
    static const char page[] =
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<title>HardwareOne Recovery</title>"
        "<style>body{font:16px system-ui;max-width:46rem;margin:2rem auto;"
        "padding:0 1rem}button,input{font-size:1rem;margin:.35rem 0;padding:.6rem}"
        "pre{white-space:pre-wrap;background:#eee;padding:1rem}</style>"
        "<h1>HardwareOne signed recovery</h1>"
        "<p>Select the signed manifest first, then its matching ESP-IDF image. "
        "The manifest is authenticated and checked before flash is touched; "
        "the streamed image is independently signed and SHA-256 checked.</p>"
        "<label>Signed manifest <input id=m type=file accept=.json></label><br>"
        "<label>Firmware image <input id=f type=file accept=.bin></label><br>"
        "<button onclick=upload()>Verify manifest and install image</button> "
        "<button onclick=go('/apply')>Apply read-only staged pair</button> "
        "<button onclick=go('/cancel')>Cancel / return</button> "
        "<button onclick=confirm('Explicitly allow an older signed version for this transaction?')&&go('/allow-downgrade')>Allow downgrade</button> "
        "<button onclick=go('/reboot')>Reboot recovery</button>"
        "<pre id=s>loading...</pre>"
        "<script>async function refresh(){let r=await fetch('/status');"
        "s.textContent=await r.text()}async function go(p){let r=await fetch(p,{method:'POST'});"
        "s.textContent=await r.text();setTimeout(refresh,800)}async function upload(){"
        "if(!m.files[0]||!f.files[0]){s.textContent='Select both files';return}"
        "s.textContent='Verifying manifest...';let r=await fetch('/manifest',{method:'POST',"
        "headers:{'Content-Type':'application/json'},body:await m.files[0].text()});"
        "let t=await r.text();if(!r.ok){s.textContent=t;return}"
        "s.textContent='Installing signed image... keep power connected';"
        "r=await fetch('/firmware',{method:'PUT',headers:{'Content-Type':'application/octet-stream'},"
        "body:f.files[0]});s.textContent=await r.text()}refresh();setInterval(refresh,3000)</script>";
    if (!require_authorization(request)) {
        return ESP_OK;
    }
    httpd_resp_set_type(request, "text/html");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_handler(httpd_req_t *request)
{
    char status[STATUS_JSON_MAX];
    size_t length = 0;
    if (!require_authorization(request)) {
        return ESP_OK;
    }
    if (s_network.callbacks.status != NULL) {
        length = s_network.callbacks.status(status, sizeof(status),
                                            s_network.callbacks.context);
    }
    if (length >= sizeof(status)) {
        length = sizeof(status) - 1;
    }
    status[length] = '\0';
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, status, (ssize_t)length);
}

static esp_err_t action_handler(httpd_req_t *request)
{
    recovery_action_t action;
    esp_err_t err;
    if (!require_authorization(request)) {
        return ESP_OK;
    }
    if (!require_safe_mutation_origin(request)) {
        return ESP_OK;
    }
    if (strcmp(request->uri, "/apply") == 0) {
        action = RECOVERY_ACTION_APPLY_STAGED;
    } else if (strcmp(request->uri, "/cancel") == 0) {
        action = RECOVERY_ACTION_CANCEL;
    } else if (strcmp(request->uri, "/allow-downgrade") == 0) {
        action = RECOVERY_ACTION_ALLOW_DOWNGRADE;
    } else {
        action = RECOVERY_ACTION_REBOOT;
    }
    err = s_network.callbacks.action != NULL
              ? s_network.callbacks.action(action, s_network.callbacks.context)
              : ESP_ERR_INVALID_STATE;
    if (err != ESP_OK) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        return httpd_resp_sendstr(request, esp_err_to_name(err));
    }
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"accepted\":true}");
}

static esp_err_t manifest_handler(httpd_req_t *request)
{
    uint8_t *body;
    size_t received = 0;
    int64_t deadline_us;
    char reason[192] = {0};
    esp_err_t err = ESP_OK;
    if (!require_authorization(request)) {
        return ESP_OK;
    }
    if (!require_safe_mutation_origin(request)) {
        return ESP_OK;
    }
    if (request->content_len <= 0 ||
        request->content_len > MANIFEST_BODY_MAX) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "manifest body size invalid");
    }
    body = malloc((size_t)request->content_len);
    if (body == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "manifest allocation failed");
    }
    deadline_us = esp_timer_get_time() + MANIFEST_RECEIVE_TIMEOUT_US;
    while (received < (size_t)request->content_len) {
        int got;
        if (esp_timer_get_time() >= deadline_us) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        got = httpd_req_recv(request, (char *)body + received,
                             (size_t)request->content_len - received);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (got <= 0) {
            err = ESP_FAIL;
            break;
        }
        if (esp_timer_get_time() >= deadline_us) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        received += (size_t)got;
    }
    if (err != ESP_OK) {
        memset(body, 0, (size_t)request->content_len);
        free(body);
        if (err == ESP_ERR_TIMEOUT) {
            httpd_resp_set_status(request, "408 Request Timeout");
            return httpd_resp_sendstr(
                request, "manifest receive deadline exceeded");
        }
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "manifest body receive failed");
    }
    err = s_network.callbacks.manifest != NULL
              ? s_network.callbacks.manifest(body, received, reason,
                  sizeof(reason), s_network.callbacks.context)
              : ESP_ERR_INVALID_STATE;
    log_http_stack_margin("manifest");
    memset(body, 0, received);
    free(body);
    if (err != ESP_OK) {
        httpd_resp_set_status(request, "422 Unprocessable Entity");
        return httpd_resp_sendstr(request,
                                  reason[0] != '\0' ? reason
                                                    : esp_err_to_name(err));
    }
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"manifest\":\"verified\"}");
}

static int upload_receive(void *context, uint8_t *buffer, size_t size)
{
    httpd_req_t *request = (httpd_req_t *)context;
    return httpd_req_recv(request, (char *)buffer, size);
}

static esp_err_t firmware_handler(httpd_req_t *request)
{
    recovery_upload_t upload;
    char reason[192] = {0};
    esp_err_t err;
    if (!require_authorization(request)) {
        return ESP_OK;
    }
    if (!require_safe_mutation_origin(request)) {
        return ESP_OK;
    }
    if (request->content_len <= 0) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "firmware body is empty");
    }
    upload.content_length = (size_t)request->content_len;
    upload.receive = upload_receive;
    upload.context = request;
    err = s_network.callbacks.firmware != NULL
              ? s_network.callbacks.firmware(&upload, reason, sizeof(reason),
                                              s_network.callbacks.context)
              : ESP_ERR_INVALID_STATE;
    log_http_stack_margin("firmware");
    if (err != ESP_OK) {
        httpd_resp_set_status(request, "422 Unprocessable Entity");
        return httpd_resp_sendstr(request,
                                  reason[0] != '\0' ? reason
                                                    : esp_err_to_name(err));
    }
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request,
                              "{\"installed\":true,\"rebooting\":true}");
}

static esp_err_t register_handlers(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_handler},
        {.uri = "/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/manifest", .method = HTTP_POST, .handler = manifest_handler},
        {.uri = "/firmware", .method = HTTP_PUT, .handler = firmware_handler},
        {.uri = "/apply", .method = HTTP_POST, .handler = action_handler},
        {.uri = "/cancel", .method = HTTP_POST, .handler = action_handler},
        {.uri = "/allow-downgrade", .method = HTTP_POST,
         .handler = action_handler},
        {.uri = "/reboot", .method = HTTP_POST, .handler = action_handler},
    };
    size_t i;
    for (i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t configure_fixed_ip(esp_netif_t *netif)
{
    esp_netif_ip_info_t address = {0};
    esp_err_t err;
    err = esp_netif_str_to_ip4(CONFIG_HW1_UPDATER_AP_IP, &address.ip);
    if (err != ESP_OK) {
        return err;
    }
    IP4_ADDR(&address.gw, ip4_addr1(&address.ip), ip4_addr2(&address.ip),
             ip4_addr3(&address.ip), ip4_addr4(&address.ip));
    IP4_ADDR(&address.netmask, 255, 255, 255, 0);
    err = esp_netif_dhcps_stop(netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        return err;
    }
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(netif, &address), TAG,
                        "set SoftAP IP");
    return esp_netif_dhcps_start(netif);
}

esp_err_t recovery_network_start(
    const recovery_credentials_t *credentials,
    const recovery_network_callbacks_t *callbacks)
{
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t wifi = {0};
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    uint8_t mac[6];
    char ssid[33];
    size_t pass_len;
    esp_err_t err;
    esp_err_t event_err;
    if (credentials == NULL || callbacks == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    pass_len = strlen(credentials->ap_password);
    if (pass_len < 12 || pass_len > 63 ||
        strlen(credentials->auth_token) != pass_len ||
        !constant_time_equals(credentials->ap_password,
                              credentials->auth_token)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_network.server != NULL || s_network.ap_netif != NULL ||
        s_network.wifi_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_network, 0, sizeof(s_network));
    s_network.callbacks = *callbacks;
    snprintf(s_network.token, sizeof(s_network.token), "%s",
             credentials->auth_token);

    err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        goto fail;
    }
    event_err = esp_event_loop_create_default();
    if (event_err != ESP_OK && event_err != ESP_ERR_INVALID_STATE) {
        err = event_err;
        ESP_LOGE(TAG, "default event loop creation failed: %s",
                 esp_err_to_name(err));
        goto fail;
    }
    s_network.event_loop_created = event_err == ESP_OK;
    s_network.ap_netif = esp_netif_create_default_wifi_ap();
    if (s_network.ap_netif == NULL) {
        err = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG, "default SoftAP netif creation failed");
        goto fail;
    }
    err = configure_fixed_ip(s_network.ap_netif);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "fixed SoftAP IP configuration failed: %s",
                 esp_err_to_name(err));
        goto fail;
    }
    err = esp_wifi_init(&init);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        goto fail;
    }
    s_network.wifi_initialized = true;
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi RAM storage selection failed: %s",
                 esp_err_to_name(err));
        goto fail;
    }
    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP mode selection failed: %s",
                 esp_err_to_name(err));
        goto fail;
    }
    err = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP MAC read failed: %s", esp_err_to_name(err));
        goto fail;
    }
    snprintf(ssid, sizeof(ssid), "HW1-Recovery-%02X%02X", mac[4], mac[5]);
    wifi.ap.ssid_len = strlen(ssid);
    if (wifi.ap.ssid_len >= sizeof(wifi.ap.ssid)) {
        err = ESP_ERR_INVALID_SIZE;
        ESP_LOGE(TAG, "generated SoftAP SSID is too long");
        goto fail;
    }
    memcpy(wifi.ap.ssid, ssid, wifi.ap.ssid_len);
    snprintf((char *)wifi.ap.password, sizeof(wifi.ap.password), "%s",
             credentials->ap_password);
    wifi.ap.channel = CONFIG_HW1_UPDATER_AP_CHANNEL;
    wifi.ap.max_connection = 2;
    wifi.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wifi.ap.pmf_cfg.capable = true;
    wifi.ap.pmf_cfg.required = false;
    err = esp_wifi_set_config(WIFI_IF_AP, &wifi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP configuration failed: %s", esp_err_to_name(err));
        goto fail;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP start failed: %s", esp_err_to_name(err));
        goto fail;
    }

    http.stack_size = 12288;
    http.server_port = CONFIG_HW1_UPDATER_HTTP_PORT;
    http.max_uri_handlers = 9;
    http.max_open_sockets = 3;
    http.lru_purge_enable = true;
    /* Keep blocking receives short enough for handler-level absolute
     * deadlines to take effect promptly. */
    http.recv_wait_timeout = 5;
    http.send_wait_timeout = 15;
    err = httpd_start(&s_network.server, &http);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(err));
        goto fail;
    }
    err = register_handlers(s_network.server);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP handler registration failed: %s",
                 esp_err_to_name(err));
        goto fail;
    }
    ESP_LOGI(TAG, "Authenticated recovery SoftAP %s at http://%s/", ssid,
             CONFIG_HW1_UPDATER_AP_IP);
    return ESP_OK;

fail:
    recovery_network_stop();
    return err;
}

void recovery_network_stop(void)
{
    if (s_network.server != NULL) {
        esp_err_t err = httpd_stop(s_network.server);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "HTTP server stop failed during cleanup: %s",
                     esp_err_to_name(err));
        }
        s_network.server = NULL;
    }
    if (s_network.wifi_initialized) {
        esp_err_t err = esp_wifi_stop();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "WiFi stop failed during cleanup: %s",
                     esp_err_to_name(err));
        }
        err = esp_wifi_deinit();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "WiFi deinit failed during cleanup: %s",
                     esp_err_to_name(err));
        }
        s_network.wifi_initialized = false;
    }
    if (s_network.ap_netif != NULL) {
        esp_netif_destroy_default_wifi(s_network.ap_netif);
        s_network.ap_netif = NULL;
    }
    if (s_network.event_loop_created) {
        esp_err_t err = esp_event_loop_delete_default();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "default event loop deletion failed during cleanup: %s",
                     esp_err_to_name(err));
        }
        s_network.event_loop_created = false;
    }
    memset(&s_network, 0, sizeof(s_network));
}
