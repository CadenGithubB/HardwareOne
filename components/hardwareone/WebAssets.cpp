#include "System_BuildConfig.h"
#if ENABLE_HTTP_SERVER
#include "WebAssets.h"
#if ENABLE_WEB_ESPNOW
#include "WebAssets_Generated.h"
#include <cstddef>
#include <cstring>
#include <strings.h>

// WEB_ASSET_HANDLER_BEGIN: compiled unchanged by the focused host tests.
// This route serves immutable program code, like /api/icon. It deliberately
// contains no user/session values; page and API authorization stay unchanged.
// Ordinary synchronous <script src> preserves the original script order.
static bool webAssetAcceptsGzip(const char* value) {
  int gzip = -1, wildcard = -1;
  while (value && *value) {
    while (*value == ' ' || *value == '\t' || *value == ',') ++value;
    const char* end = std::strchr(value, ',');
    if (!end) end = value + std::strlen(value);
    const char* codingEnd = value;
    while (codingEnd < end && *codingEnd != ';' && *codingEnd != ' ' && *codingEnd != '\t') ++codingEnd;
    const size_t codingSize = static_cast<size_t>(codingEnd - value);
    const bool isGzip = codingSize == 4 && strncasecmp(value, "gzip", 4) == 0;
    const bool isWildcard = codingSize == 1 && *value == '*';
    if (isGzip || isWildcard) {
      int quality = 1000;
      const char* p = codingEnd;
      while (p < end && (*p == ' ' || *p == '\t')) ++p;
      if (p < end) {
        // Accept only the HTTP qvalue grammar; malformed preferences fail closed.
        if (*p++ != ';') quality = 0;
        while (p < end && (*p == ' ' || *p == '\t')) ++p;
        if (p == end || (*p != 'q' && *p != 'Q')) quality = 0;
        else ++p;
        while (p < end && (*p == ' ' || *p == '\t')) ++p;
        if (p == end || *p++ != '=') quality = 0;
        while (p < end && (*p == ' ' || *p == '\t')) ++p;
        if (p == end || (*p != '0' && *p != '1')) quality = 0;
        else {
          const bool one = *p++ == '1';
          int parsed = one ? 1000 : 0;
          if (p < end && *p == '.') {
            ++p;
            int place = 100;
            while (p < end && *p >= '0' && *p <= '9' && place) {
              const int digit = *p++ - '0';
              if (one && digit) quality = 0;
              if (!one) parsed += digit * place;
              place /= 10;
            }
          }
          while (p < end && (*p == ' ' || *p == '\t')) ++p;
          if (p != end) quality = 0;
          else if (quality) quality = parsed;
        }
      }
      int& preference = isGzip ? gzip : wildcard;
      if (preference < 0 || quality < preference) preference = quality;
    }
    value = *end ? end + 1 : end;
  }
  return gzip >= 0 ? gzip > 0 : wildcard > 0;
}

static esp_err_t handleEspNowCoreAsset(httpd_req_t* req) {
  // Fixed URL: always refetch after a firmware update, avoiding old program code.
  if (httpd_resp_set_hdr(req, "Cache-Control", "no-store") != ESP_OK ||
      httpd_resp_set_hdr(req, "Vary", "Accept-Encoding") != ESP_OK ||
      httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff") != ESP_OK) return ESP_FAIL;
  char encoding[256];
  const size_t length = httpd_req_get_hdr_value_len(req, "Accept-Encoding");
  bool accepted = false;
  if (length < sizeof(encoding)) {
    const esp_err_t read = httpd_req_get_hdr_value_str(req, "Accept-Encoding", encoding, sizeof(encoding));
    // An absent field accepts any coding (RFC 9110); an explicit empty field does not.
    accepted = read == ESP_ERR_NOT_FOUND || (read == ESP_OK && webAssetAcceptsGzip(encoding));
  }
  if (!accepted) {
    httpd_resp_set_status(req, "406 Not Acceptable");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "This static asset requires gzip support.", HTTPD_RESP_USE_STRLEN);
  }
  if (httpd_resp_set_type(req, "application/javascript; charset=utf-8") != ESP_OK ||
      httpd_resp_set_hdr(req, "Content-Encoding", "gzip") != ESP_OK) return ESP_FAIL;
  return httpd_resp_send(req,
      reinterpret_cast<const char*>(hw1::web_assets::kEspNowCoreGzip),
      hw1::web_assets::kEspNowCoreGzipSize);
}
// WEB_ASSET_HANDLER_END
#endif

void registerWebAssetHandlers(httpd_handle_t server) {
#if ENABLE_WEB_ESPNOW
  static const httpd_uri_t asset = {
    .uri = hw1::web_assets::kEspNowCoreUri, .method = HTTP_GET,
    .handler = handleEspNowCoreAsset, .user_ctx = nullptr
  };
  httpd_register_uri_handler(server, &asset);
#else
  (void)server;
#endif
}
#endif
