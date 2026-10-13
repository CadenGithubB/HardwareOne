#!/usr/bin/env python3
"""Execute production HTTP-setting handlers, serialization and audit redaction.

Compiles extracted production functions with filesystem/events/HTTP publication
stubbed. Secret encryption is a spy: this verifies the real serializer routes
the token through putSecret, not the cryptographic algorithm itself.
"""

import argparse
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def block(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    tokens = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]')
    depth = 0
    for match in tokens.finditer(source, brace):
        if match[0] == "{":
            depth += 1
        elif match[0] == "}":
            depth -= 1
            if not depth:
                return source[start:match.end()]
    raise AssertionError("unterminated production block: " + signature)


HARNESS = r'''
#include <cassert>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <string>
#include <vector>
#include "Arduino.h"
#include <ArduinoJson.h>
#include "System_LLMHttpCore.h"
using namespace hw1http;
#define ENABLE_LLM_SOURCE_HTTP 1
#define EXT_RAM_BSS_ATTR
#define LLM_DEFAULT_REP_WINDOW 32
#define LLM_SETTING_MAX_CONTEXT 4096
bool validating = false;
#define RETURN_VALID_IF_VALIDATE_CSTR() do { if (validating) return "VALID"; } while (0)
std::vector<std::string> broadcasts, events;
void logBroadcast(const char* format, ...) {
  char out[8192]; va_list arguments; va_start(arguments, format);
  vsnprintf(out, sizeof(out), format, arguments); va_end(arguments);
  broadcasts.emplace_back(out);
}
#define BROADCAST_PRINTF(...) logBroadcast(__VA_ARGS__)
#define ERROR_STORAGEF(...) logBroadcast(__VA_ARGS__)
constexpr int SYSEVT_SETTING_CHANGED = 1;
void systemEventPost(int, const char*, const char* value) { events.emplace_back(value); }
int persists = 0, notifications = 0, publications = 0, secretWrites = 0;
bool requestSettingsPersist() { ++persists; return true; }
void applyMasterEnableDisable(const char*) {}
void webBatchTestStringAppend() {}
std::string publishedToken, publishedUrl, publishedCa;
bool httpLlmConfigure(bool, const char* url, const char*, const char* token, bool, const char* ca) {
  ++publications; publishedUrl = url; publishedToken = token; publishedCa = ca; return true;
}
// INSERT_TYPES
// INSERT_SETTINGS
// INSERT_TABLE
const SettingsModule* gSettingsModules[] = { &llmSettingsModule };
size_t gSettingsModuleCount = 1;
const SettingsModule** getSettingsModules(size_t& count) { count = 1; return gSettingsModules; }
void putSecret(JsonObject object, const char* key, const String& value) {
  ++secretWrites;
  assert(value == gSettings.llmHttpToken);
  object[key] = "AES:test-ciphertext";
}
// INSERT_SETTINGS_IMPLEMENTATION

struct CommandResolution { const void* entry = nullptr; };
int knownEntry;
CommandResolution resolveCommand(const String&) { return {&knownEntry}; }
String redactCmdForAudit(const String&);
String redactCmdForAudit(const String&, const CommandResolution&);
// INSERT_COMMAND_ARGS
// INSERT_REDACTION

int main() {
  constexpr const char* secret = "synthetic-http-gateway-secret";
  const auto* token = llmSettingByCmdKey("llmhttptoken");
  assert(token && token->isSecret);
  assert(std::string(token->group) == "http");
  assert(std::string(token->jsonKey) == "token");

  const char* result = cmd_llm_httptoken(secret);
  assert(!std::strstr(result, secret));
  assert(gSettings.llmHttpToken == secret && publishedToken == secret);
  assert(persists == 1 && publications == 1);
  for (const auto& value : broadcasts) assert(value.find(secret) == std::string::npos);
  for (const auto& value : events) assert(value.find(secret) == std::string::npos);
  result = cmd_llm_httptoken("");
  assert(std::strstr(result, "********") && !std::strstr(result, secret));
  assert(persists == 1 && publications == 1); // Query is not a write/reset.

  JsonDocument disk;
  disk.to<JsonObject>();
  writeRegisteredSettings(disk, nullptr, false);
  assert(secretWrites == 1);
  assert(disk["apps"]["llm"]["http"]["token"] == "AES:test-ciphertext");
  std::string serialized;
  serializeJson(disk, serialized);
  assert(serialized.find(secret) == std::string::npos);
  stripWebSecrets(disk);
  assert(disk["apps"]["llm"]["http"]["token"].isNull());
  assert(!disk["apps"]["llm"]["http"]["enabled"].isNull());

  const std::vector<std::string> lines = {
    "llmhttptoken " + std::string(secret),
    " LLMHTTPTOKEN\t" + std::string(secret),
    "llmhttptoken\r" + std::string(secret),
    "llmhttptoken \"" + std::string(secret) + " trailing\"",
    "llmhttptoken \"" + std::string(secret),
    "espnowremote peer user test-password llmhttptoken " + std::string(secret),
  };
  for (const auto& line : lines) {
    String redacted = redactCmdForAudit(String(line));
    assert(std::string(redacted.c_str()).find(secret) == std::string::npos);
  }
  const std::string sensitiveUrl = "https://operator:" + std::string(secret) + "@agent/v1";
  const auto broadcastCount = broadcasts.size();
  const auto eventCount = events.size();
  assert(std::string(cmd_llm_httpurl(String(sensitiveUrl))).find("Error:") == 0);
  assert(gSettings.llmHttpUrl.length() == 0);
  assert(publications == 1 && persists == 1);
  assert(broadcasts.size() == broadcastCount && events.size() == eventCount);
  String safeUrlAudit = redactCmdForAudit(String("llmhttpurl " + sensitiveUrl));
  assert(std::string(safeUrlAudit.c_str()).find(secret) == std::string::npos);
  assert(!std::strstr(cmd_llm_httptoken("value\r\ninjected"), secret));
  assert(gSettings.llmHttpToken == secret && publications == 1);
  assert(std::string(cmd_llm_httptoken(String(std::string(1025, 'a')))).find("Error:") == 0);
  assert(gSettings.llmHttpToken == secret && publications == 1);
  validating = true;
  cmd_llm_httptoken("replacement");
  assert(gSettings.llmHttpToken == secret && publications == 1 && persists == 1);
  validating = false;
  cmd_llm_httptoken("-");
  assert(gSettings.llmHttpToken.length() == 0 && publishedToken.empty());
  assert(events.size() == 2 && persists == 2 && publications == 2);
  cmd_llm_httpca("-----BEGIN CERTIFICATE-----\\nexample\\n-----END CERTIFICATE-----");
  assert(publishedCa.find('\n') != std::string::npos && publishedCa.find("\\n") == std::string::npos);
  const auto savedWrites = persists;
  const auto savedEvents = events.size();
  cmd_llm_httptoken("-");
  assert(persists == savedWrites && events.size() == savedEvents);
  puts("HTTP settings: secret setter/getter/events, encrypted persistence routing, web exclusion, audit redaction, validation and clear passed");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    commands = (COMPONENT / "System_LLMCommands.cpp").read_text()
    settings = (COMPONENT / "System_Settings.cpp").read_text()
    settings_header = (COMPONENT / "System_Settings.h").read_text()
    utils = (COMPONENT / "System_Utils.cpp").read_text()
    command_header = (COMPONENT / "System_Command.h").read_text()
    command_source = (COMPONENT / "System_Command.cpp").read_text()
    http_source = (COMPONENT / "System_LLMHttp.cpp").read_text()
    table = block(commands, "static const SettingEntry llmSettingEntries[]") + ";\n"
    table += block(commands, "extern const SettingsModule llmSettingsModule") + ";\n"
    types = "\n".join(block(settings_header, name) + ";" for name in (
        "enum SettingType", "struct SettingEntry {", "struct SettingsModule"))
    types = "using ConnectionCheckFunc = bool (*)();\n" + types
    field_types = {"INT": "int", "BOOL": "bool", "FLOAT": "float", "STRING": "String"}
    fields = re.findall(r"SETTING_(\w+),\s*&gSettings\.(\w+)", table)
    state = "struct TestSettings {\n" + "\n".join(
        field_types[kind] + " " + name + "{};" for kind, name in fields) + "\n} gSettings;"
    types += "\n" + re.search(r"(?m)^#define SYSEVT_DETAIL_LEN .+$",
                               (COMPONENT / "System_Events.h").read_text()).group()
    implementation = block(http_source, "bool httpLlmValidateBaseUrl(") + "\n"
    implementation += block((COMPONENT / "System_Utils.h").read_text(), "inline int parseBoolArg(") + "\n"
    implementation += block(settings, "void notifySettingChanged(") + "\n"
    implementation += block(settings, "void finishSettingChange(") + "\n"
    implementation += "\n".join(block(settings_header, signature) for signature in (
        "template<typename T>\ninline void setSetting(",
        "inline void setSetting(String& field, const String&",
        "inline void setSetting(String& field, const char*")) + "\n"
    implementation += "\n".join(block(settings, signature) for signature in (
        "const char* handleSettingCommand(", "static JsonObject jsonPathCreate(",
        "size_t writeRegisteredSettings("))
    implementation += "\nvoid stripWebSecrets(JsonDocument& doc) { bool excludePasswords = true;\n"
    implementation += block(settings, "if (excludePasswords) {") + "\n}\n"
    implementation += "\n".join(block(commands, signature) for signature in (
        "static const SettingEntry* llmSettingByCmdKey(", "void llmHttpApplySettings(",
        "static const char* llmHttpSettingCommand(", "static const char* cmd_llm_httptoken(",
        "static const char* cmd_llm_httpurl(",
        "static const char* cmd_llm_httpca("))
    arg_code = block(command_header, "class CommandArgs") + ";\nconst String CommandArgs::empty_;\n"
    arg_code += "\n".join(block(command_source, signature) for signature in (
        "CommandArgs::CommandArgs(", "void CommandArgs::parse(", "const String& CommandArgs::arg("))
    redactor_start = utils.index("namespace {", utils.index("// Audit / Redaction utilities"))
    redaction = block(utils[redactor_start:], "namespace {") + "\n"
    redaction += block(utils, "String redactCmdForAudit(const String& argsInput) {") + "\n"
    redaction += block(utils, "String redactCmdForAudit(const String& argsInput,\n")
    unit = HARNESS
    for marker, content in (("TYPES", types), ("SETTINGS", state), ("TABLE", table),
                            ("SETTINGS_IMPLEMENTATION", implementation),
                            ("COMMAND_ARGS", arg_code), ("REDACTION", redaction)):
        unit = unit.replace("// INSERT_" + marker + "\n", content + "\n")
    arduino = (HERE / "web_batch_stubs/Arduino.h").read_text()
    arduino = arduino.replace(" private:\n", r'''
  bool reserve(size_t count) { text_.reserve(count); return true; }
  bool equalsIgnoreCase(const String& value) const { return equalsIgnoreCase(value.c_str()); }
  void replace(const char* before, const char* after) {
    size_t offset = 0;
    while ((offset = text_.find(before, offset)) != std::string::npos) {
      text_.replace(offset, std::strlen(before), after); offset += std::strlen(after);
    }
  }
 private:
''')
    with tempfile.TemporaryDirectory(prefix="hw1-llm-http-settings-") as directory:
        tmp = Path(directory)
        (tmp / "Arduino.h").write_text(arduino)
        (tmp / "settings.cpp").write_text(unit)
        command = ["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-sign-compare", "-Wno-unused-function", "-I", str(tmp), "-I", str(COMPONENT),
                   "-I", str(COMPONENT.parent / "hardwareone_libs/ArduinoJson/src"),
                   str(tmp / "settings.cpp"), "-o", str(tmp / "settings")]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True)
        subprocess.run([str(tmp / "settings")], check=True)


if __name__ == "__main__":
    main()
