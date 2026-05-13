# Stream `/api/settings/schema` instead of single-buffer serialize

## Problem

`/api/settings/schema` returns HTTP 500 when many settings modules are
registered. Observed with `I2C_FEATURE_LEVEL=3 (FULL)` — nine sensor modules
each contributing 5–25 entries pushes the serialized JSON over the
`JSON_RESPONSE_SIZE = 32768` ceiling.

Failure path:
- `serializeJson(doc, gJsonResponseBuffer, JSON_RESPONSE_SIZE)` returns a
  length ≥ buffer size
- Handler at `WebServer_Server.cpp:2650` returns 500
- Settings page IIFEs that depend on the schema crash:
  - **Sensors IIFE** never reaches its output-container populate step →
    Output Channels stuck on "Loading output settings..."
  - **Debug IIFE** catches the `.json()` parse error of the 500 body →
    "Error loading debug settings: The string did not match the expected pattern"

Both panels die on the same upstream overflow; no debug flags changed —
this was triggered purely by sensor modules registering schema entries.

## Current state

Buffer left at 32 KB. The bug exists but is tolerated for now: Output
Channels and Debug Controls panels are broken when `I2C_FEATURE_LEVEL=3`.
Setting `I2C_FEATURE_LEVEL=4` with selective sensors trims enough modules
out of the schema to fit. Decision was to defer the streaming fix rather
than bump the buffer to 64 KB, since the buffer (PSRAM) is shared by
many endpoints and most of them use far less than 32 KB — bumping for one
outlier is wasteful.

## Proposed fix: chunked response

ESP-IDF's `httpd_resp_send_chunk()` supports `Transfer-Encoding: chunked`
out of the box. Browser `r.json()` handles chunked transparently — no JS
changes needed.

### Implementation outline

Replace the body of `handleSettingsSchema()` in
[`WebServer_Server.cpp:2534`](../components/hardwareone/WebServer_Server.cpp)
with a streaming loop:

```cpp
esp_err_t handleSettingsSchema(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);

  if (!gJsonResponseBuffer) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  JsonBufferGuard jsonGuard("handleSettingsSchema");
  if (!jsonGuard.held) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

  // Open: {"modules":[
  httpd_resp_send_chunk(req, "{\"modules\":[", 12);

  size_t modCount = 0;
  const SettingsModule** mods = getSettingsModules(modCount);
  for (size_t m = 0; m < modCount; m++) {
    const SettingsModule* mod = mods[m];
    if (!mod) continue;

    // Build ONE module's JSON into the shared buffer
    PSRAM_JSON_DOC(modDoc);
    JsonObject modObj = modDoc.to<JsonObject>();
    modObj["name"] = mod->name;
    modObj["section"] = mod->jsonSection ? mod->jsonSection : mod->name;
    modObj["description"] = mod->description ? mod->description : "";
    if (mod->isConnected) modObj["connected"] = mod->isConnected();

    JsonArray entries = modObj["entries"].to<JsonArray>();
    for (size_t i = 0; i < mod->count; i++) {
      const SettingEntry* e = &mod->entries[i];
      if (!e || !e->jsonKey) continue;
      JsonObject entry = entries.add<JsonObject>();
      entry["key"] = e->jsonKey;
      entry["label"] = e->label ? e->label : e->jsonKey;
      switch (e->type) {
        case SETTING_INT:    entry["type"] = "int";    break;
        case SETTING_FLOAT:  entry["type"] = "float";  break;
        case SETTING_BOOL:   entry["type"] = "bool";   break;
        case SETTING_STRING: entry["type"] = "string"; break;
      }
      if (e->isSecret) entry["secret"] = true;
      if (e->group)    entry["group"]  = e->group;
      if (e->cmdKey)   entry["cmdKey"] = e->cmdKey;
      if ((e->type == SETTING_INT || e->type == SETTING_FLOAT) &&
          (e->minVal != 0 || e->maxVal != 0)) {
        entry["min"] = e->minVal;
        entry["max"] = e->maxVal;
      }
      if (e->options) entry["options"] = e->options;
      switch (e->type) {
        case SETTING_INT:    entry["default"] = e->intDefault;             break;
        case SETTING_FLOAT:  entry["default"] = e->floatDefault;           break;
        case SETTING_BOOL:   entry["default"] = (bool)e->intDefault;       break;
        case SETTING_STRING: entry["default"] = e->stringDefault ? e->stringDefault : ""; break;
      }
    }

    size_t len = serializeJson(modDoc, gJsonResponseBuffer, JSON_RESPONSE_SIZE);
    if (len == 0 || len >= JSON_RESPONSE_SIZE) {
      // One module exceeds the buffer — bail.
      WARN_WEBF("[Schema] Single module '%s' overflowed buffer (%zu B)", mod->name, len);
      httpd_resp_send_chunk(req, NULL, 0);  // close out what we sent
      return ESP_FAIL;
    }

    if (m > 0) httpd_resp_send_chunk(req, ",", 1);
    httpd_resp_send_chunk(req, gJsonResponseBuffer, len);
  }

  // Close: ],"count":N}
  char tail[32];
  int tn = snprintf(tail, sizeof(tail), "],\"count\":%zu}", modCount);
  httpd_resp_send_chunk(req, tail, tn);

  // Terminate chunked response
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}
```

### Key properties

- **Zero buffer bump.** Each iteration serializes only one module into
  the existing 32 KB shared buffer; thermal (the worst case) is ~3 KB.
- **Scales indefinitely.** Adding a 50th sensor module changes nothing.
- **No JS changes.** Browser sees chunked transfer-encoding; `.json()`
  reassembles transparently.
- **Same response shape.** Client receives the exact same `{"modules":[...],"count":N}`
  structure as the single-buffer version.

### Risks / edge cases

- A single module whose serialized form alone exceeds 32 KB would still
  overflow. The handler bails with `ESP_FAIL` and a warn log in that case.
  Today's worst module is thermal at ~3 KB, so this is 10× headroom.
- The shared `gJsonResponseBuffer` mutex (`JsonBufferGuard`) is held for
  the duration of the streaming. No other endpoint can serialize JSON
  concurrently. This matches current behavior.
- Chunked response means status code is committed at the moment the first
  chunk leaves. If serialization of module #5 fails mid-stream, we can't
  retroactively return 500 — the client gets a malformed JSON document.
  The `if (len == 0 || len >= JSON_RESPONSE_SIZE)` guard inside the loop
  catches the predictable failure (single-module overflow).

### Validation

After implementing:
1. Browser DevTools → Network → reload Settings → `/api/settings/schema`
   should show **HTTP 200**, `Transfer-Encoding: chunked`, body size
   matching the full schema (~45 KB with all sensors enabled).
2. Output Channels panel populates correctly.
3. Debug Controls panel populates correctly.
4. Test under all `I2C_FEATURE_LEVEL` values (0, 1, 2, 3, 4) — schema
   should serialize at every size.

## Why defer

- The fix is real refactor work (~50 lines), not a one-line config flip.
- Current workaround (stay on `I2C_FEATURE_LEVEL=4` and disable sensors
  one doesn't need) is acceptable for now.
- Streaming should be implemented *before* adding any more settings
  modules that would push the schema further — otherwise the bug
  surfaces again the moment the next big subsystem registers.

## Related files

- [`components/hardwareone/WebServer_Server.cpp:2534`](../components/hardwareone/WebServer_Server.cpp) — `handleSettingsSchema()` body
- [`components/hardwareone/WebServer_Server.h:13-15`](../components/hardwareone/WebServer_Server.h) — `JSON_RESPONSE_SIZE` definition (no change needed under streaming)
- [`components/hardwareone/WebPage_Settings.h:762,1259`](../components/hardwareone/WebPage_Settings.h) — JS fetch sites (no change needed; chunked is transparent)
