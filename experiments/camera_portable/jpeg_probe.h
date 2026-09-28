#pragma once
// Experiment-only command; prepare.py inserts this header and command entry
// into private app copies. It is not part of the production command registry.
#include "HAL_JPEG.h"
#include "System_AuthIdentity.h"
#include "System_Command.h"
#include "System_Filesystem.h"
#include "System_Filesystem_Internal.h"
#include "System_MemUtil.h"
#include "System_Mutex.h"
#include "System_VFS.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include <ArduinoJson.h>
#include <memory>
#include <stdlib.h>

static const char* cmd_camerajpegprobe(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const AuthContext& context = currentAuthContext();
  if (context.transport != SOURCE_SERIAL || context.user.length() == 0 || !currentExecIsAdmin())
    return "{\"schema\":1,\"valid\":false,\"error\":\"serial_admin_required\"}";
  if (!filesystemReady)
    return "{\"schema\":1,\"valid\":false,\"error\":\"filesystem_unavailable\"}";
  CommandArgs args(argsInput);
  String path;
  if (args.count() != 1 || args.unterminatedQuote() || requireQuotedPath(args, 0, path))
    return "{\"schema\":1,\"valid\":false,\"error\":\"one_quoted_path_required\"}";
  constexpr size_t kMaxInput = 128u * 1024u;
  size_t length = 0;
  std::unique_ptr<uint8_t, decltype(&free)> bytes(nullptr, &free);
  {
    FsLockGuard lock("camera.jpeg.probe");
    File file = VFS::openGuarded(path, "r", context);
    if (!file || file.isDirectory())
      return "{\"schema\":1,\"valid\":false,\"error\":\"file_unavailable_or_denied\"}";
    length = file.size();
    if (!length || length > kMaxInput)
      return "{\"schema\":1,\"valid\":false,\"error\":\"input_outside_1_to_131072_bytes\"}";
    bytes.reset(static_cast<uint8_t*>(ps_alloc(length, AllocPref::PreferPSRAM, "camera.jpeg.probe")));
    if (!bytes)
      return "{\"schema\":1,\"valid\":false,\"error\":\"input_allocation_failed\"}";
    size_t offset = 0;
    while (offset < length) {
      const size_t got = file.read(bytes.get() + offset, length - offset);
      if (!got || got > length - offset)
        return "{\"schema\":1,\"valid\":false,\"error\":\"file_read_incomplete\"}";
      offset += got;
    }
    file.close();
  } // No filesystem lock is held while the CPU validates entropy.
  hwjpeg::DecodeOptions options;
  options.maxInputBytes = kMaxInput;
  options.maxWidth = 640;
  options.maxHeight = 480;
  options.maxOutputBytes = 640u * 480u * 3u;
  hwjpeg::Info info;
  const char* error = nullptr;
  const int64_t start = esp_timer_get_time();
  const bool valid = hwjpeg::validateSoftware(bytes.get(), length, info, options, &error);
  const int64_t elapsed = esp_timer_get_time() - start;
  bytes.reset();
  JsonDocument doc;
  doc["schema"] = 1;
  doc["valid"] = valid;
  doc["width"] = info.width;
  doc["height"] = info.height;
  doc["length"] = length;
  doc["us"] = elapsed;
  doc["error"] = error;
#if defined(CONFIG_JD_USE_ROM) && CONFIG_JD_USE_ROM
  doc["rom"] = true;
#else
  doc["rom"] = false;
#endif
  static char response[384];
  if (measureJson(doc) >= sizeof(response))
    return "{\"schema\":1,\"valid\":false,\"error\":\"metadata_too_large\"}";
  serializeJson(doc, response, sizeof(response));
  return response;
}
