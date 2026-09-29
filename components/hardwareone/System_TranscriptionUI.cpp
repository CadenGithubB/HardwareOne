#include "System_TranscriptionUI.h"
#if ENABLE_DICTATION
#include "System_Dictation.h"
#include "System_AuthIdentity.h"
#include "System_Command.h"
#include "System_Filesystem.h"
#include "System_Mutex.h"
#include "System_Settings.h"
#include "System_Utils.h"
#include "System_VFS.h"
#include "Transcription_UI_Policy.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <cstdio>

namespace {
// Only cmd_exec uses this response. Never send transcript data via broadcast.
EXT_RAM_BSS_ATTR char response[4096];
const char* reply(JsonDocument& doc) {
  if (!doc["success"].is<bool>()) doc["success"] = true;
  if (doc["error"].isNull()) doc["error"] = "";
  if (doc["transcriptPath"].isNull()) doc["transcriptPath"] = true;
  if (doc.overflowed() || measureJson(doc) >= sizeof(response))
    return "{\"success\":false,\"error\":\"Response too large\",\"transcriptPath\":true}";
  serializeJson(doc, response, sizeof(response));
  return response;
}
const char* error(JsonDocument& doc, const char* why) {
  doc.clear(); doc["success"] = false; doc["error"] = why;
  return reply(doc);
}
bool actor(DictationAppLease& lease) {
  const auto& ctx = currentAuthContext();
  lease.source = ctx.transport;
  lease.epoch = captureTransportSessionEpoch(ctx);
  return ctx.user.length() && lease.epoch &&
      transportSessionEpochIsLive(lease.source, lease.epoch);
}
bool live(const DictationAppLease& lease) {
  return transportSessionEpochIsLive(lease.source, lease.epoch);
}
void putId(JsonDocument& doc, uint64_t id) {
  char text[17]{};
  if (id) snprintf(text, sizeof(text), "%08lx%08lx", (unsigned long)(id >> 32), (unsigned long)(uint32_t)id);
  doc["exchange"] = text;
}
void putStatus(JsonDocument& doc, const DictationAppLease& lease) {
  DictationAppSnapshot app{};
  const bool valid = dictationAppSnapshot(lease, &app);
  const char* why = nullptr;
  const bool available = dictationAvailable(&why);
  putId(doc, valid ? lease.exchange : 0);
  doc["active"] = valid && app.active;
  doc["done"] = valid && app.done;
  doc["busy"] = app.busy;
  doc["available"] = available;
  doc["reason"] = why ? why : "";
  doc["saveDefault"] = gSettings.sttSaveTranscripts;
  doc["continuous"] = valid ? app.status.continuous : bool(ENABLE_LOCAL_STT);
  doc["preparing"] = valid && app.status.preparing;
  doc["captureActive"] = valid && app.status.captureActive;
  doc["inferenceActive"] = valid && app.status.inferenceActive;
  doc["elapsedMs"] = valid ? app.status.elapsedMs : 0;
  doc["level"] = valid ? app.status.level : 0;
  doc["failure"] = valid ? app.status.failure : "";
  doc["saveEnabled"] = valid && app.status.transcript.enabled;
  doc["saveComplete"] = valid && app.status.transcript.complete;
  doc["saveError"] = valid ? app.status.transcript.error : "";
  doc["transcriptPath"] = valid ? app.status.transcript.path : "";
}
const char* commandSession(const String& input) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  JsonDocument doc;
  CommandArgs args(input);
  DictationAppLease lease{};
  if (!actor(lease)) return error(doc, "Sign in to use transcription");
  if (args.unterminatedQuote()) return error(doc, "Invalid arguments");
  const String op = args.count() ? args.arg(0) : "status";
  if (op == "status" && args.count() <= 1) {
    DictationAppLease current{};
    if (dictationAppCurrent(lease.source, lease.epoch, &current)) lease = current;
    putStatus(doc, lease);
  } else if (op == "start" && args.count() == 1) {
    String role;
    if (!getUserAuthorizationRole(currentAuthContext().user, role) || role == "guest")
      return error(doc, "Transcription requires a non-guest account");
    if (!dictationAppBegin(lease.source, lease.epoch, &lease)) {
      const char* why = nullptr; (void)dictationAvailable(&why);
      return error(doc, why ? why : "Transcription is busy");
    }
    putStatus(doc, lease);
  } else {
    if (args.count() < 2 || !TranscriptionUI::parseId(args.arg(1).c_str(), lease.exchange))
      return error(doc, "A valid session ID is required");
    DictationAppSnapshot state{};
    if (!dictationAppSnapshot(lease, &state)) return error(doc, "Session unavailable");
    if (op == "status" && args.count() == 2) putStatus(doc, lease);
    else if ((op == "stop" || op == "cancel") && args.count() == 2) {
      const bool ok = op == "stop" ? dictationAppRequestStop(lease) : dictationAppCancel(lease);
      if (!ok) return error(doc, "Session unavailable");
      putStatus(doc, lease);
    } else if (op == "next" && args.count() == 2) {
      char text[DICTATION_MAX_TEXT + 1]{};
      DictationTextReceipt receipt{};
      const bool available = dictationAppPeekText(lease, text, sizeof(text), &receipt);
      putId(doc, lease.exchange);
      doc["available"] = available; doc["sttText"] = text;
      doc["sequence"] = receipt.sequence; doc["offset"] = receipt.offset;
      doc["length"] = receipt.length;
      TranscriptionUI::clear(text, sizeof(text));
    } else if (op == "ack" && args.count() == 5) {
      uint32_t seq, offset, length;
      if (!TranscriptionUI::parseUnsigned(args.arg(2).c_str(), seq) ||
          !TranscriptionUI::parseUnsigned(args.arg(3).c_str(), offset) ||
          !TranscriptionUI::parseUnsigned(args.arg(4).c_str(), length) ||
          offset > UINT16_MAX || length > DICTATION_MAX_TEXT || !length)
        return error(doc, "Invalid receipt");
      const DictationTextReceipt receipt{lease.exchange, seq, (uint16_t)offset, (uint16_t)length};
      if (!dictationAppCommitText(lease, receipt, length)) return error(doc, "Receipt unavailable");
      putId(doc, lease.exchange);
    } else return error(doc, "Invalid transcription action");
  }
  if (!live(lease)) return error(doc, "Session expired");
  return reply(doc);
}
const char* commandFiles(const String& input) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  JsonDocument doc;
  CommandArgs args(input);
  DictationAppLease owner{};
  if (!actor(owner)) return error(doc, "Sign in to view transcripts");
  if (args.unterminatedQuote()) return error(doc, "Invalid arguments");
  const auto& ctx = currentAuthContext();
  FsLockGuard lock("transcription.browser", pdMS_TO_TICKS(1000));
  if (!lock.held && !isFsLockedByCurrentTask()) return error(doc, "Storage busy");
  uint32_t account = 0;
  if (!getUserIdByUsername(ctx.user.c_str(), account) || !account || !live(owner))
    return error(doc, "Account unavailable");
  if (args.arg(0) == "list" && args.count() == 3 &&
      (args.arg(1) == "internal" || args.arg(1) == "sd")) {
    uint32_t offset;
    if (!TranscriptionUI::parseUnsigned(args.arg(2).c_str(), offset)) return error(doc, "Invalid offset");
    const bool sd = args.arg(1) == "sd";
    if (sd && !VFS::isSDAvailable()) return error(doc, "No SD card");
    char folder[32];
    snprintf(folder, sizeof(folder), "%s/u%lu", sd ? "/sd/stt" : "/stt", (unsigned long)account);
    doc["transcriptPath"] = folder;
    JsonArray rows = doc["entries"].to<JsonArray>();
    uint32_t found = 0, shown = 0; bool more = false;
    if (VFS::existsGuarded(folder, ctx)) {
      File dir = VFS::openGuarded(folder, "r", ctx);
      if (!dir || !dir.isDirectory()) return error(doc, "Folder unavailable");
      for (File file = dir.openNextFile(); file; file = dir.openNextFile()) {
        const char* name = strrchr(file.name(), '/'); name = name ? name + 1 : file.name();
        String lower(name); lower.toLowerCase();
        if (!file.isDirectory() && TranscriptionUI::safeFilename(name) && lower.endsWith(".txt")) {
          char path[128]; snprintf(path, sizeof(path), "%s/%s", folder, name);
          if (canRead(path, ctx) && found++ >= offset) {
            if (shown == 8) { more = true; file.close(); break; }
            JsonObject row = rows.add<JsonObject>();
            row["name"] = name; row["path"] = path; row["bytes"] = file.size(); ++shown;
          }
        }
        file.close();
      }
      dir.close();
    }
    doc["offset"] = offset; doc["nextOffset"] = offset + shown; doc["more"] = more;
  } else if (args.arg(0) == "read" && args.count() == 3 && args.argWasQuoted(1)) {
    uint32_t offset;
    if (!TranscriptionUI::parseUnsigned(args.arg(2).c_str(), offset)) return error(doc, "Invalid offset");
    String path;
    if (!normalizeFsPath(args.arg(1), path) || path.length() >= 128) return error(doc, "Invalid path");
    char internal[32], sd[32];
    snprintf(internal, sizeof(internal), "/stt/u%lu/", (unsigned long)account);
    snprintf(sd, sizeof(sd), "/sd/stt/u%lu/", (unsigned long)account);
    if (!path.startsWith(internal) && !path.startsWith(sd)) return error(doc, "Not your transcript");
    File file = VFS::openGuarded(path, "r", ctx);
    if (!file || file.isDirectory()) return error(doc, "Transcript unavailable");
    const size_t total = file.size();
    if (offset > total || (offset && !file.seek(offset))) { file.close(); return error(doc, "Invalid offset"); }
    char text[513]{};
    const size_t wanted = std::min<size_t>(512, total - offset);
    size_t got = wanted ? file.read(reinterpret_cast<uint8_t*>(text), wanted) : 0;
    file.close();
    if (got != wanted) return error(doc, "Transcript read incomplete");
    if (offset + got < total) got = TranscriptionUI::completePrefix(text, got);
    text[got] = 0;
    doc["sttText"] = text; doc["offset"] = offset; doc["nextOffset"] = offset + got;
    doc["eof"] = offset + got >= total; doc["total"] = total; doc["transcriptPath"] = path;
    TranscriptionUI::clear(text, sizeof(text));
  } else return error(doc, "Invalid transcript request");
  if (!live(owner)) return error(doc, "Session expired");
  return reply(doc);
}
}
const CommandEntry transcriptionUICommands[] = {
  { "transcription", "Transcription interface controls", false, commandSession,
    "transcription start|status [id]|stop <id>|cancel <id>|next <id>|ack <id> <sequence> <offset> <length>" },
  { "transcripts", "Browse your saved transcripts", false, commandFiles,
    "transcripts list internal|sd <offset> | transcripts read \"<path>\" <offset>" },
};
const size_t transcriptionUICommandsCount = sizeof(transcriptionUICommands) / sizeof(transcriptionUICommands[0]);
#endif
