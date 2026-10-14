#!/usr/bin/env python3
"""Exercise the production CLI/web retained-turn serializers with long replies."""
import argparse
from pathlib import Path
import subprocess
import tempfile

from test_llm_http_settings import block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

HARNESS = r'''
#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "Arduino.h"
#include <ArduinoJson.h>
#include "System_LLMReplyLimits.h"
#include "System_LLMUtf8.h"
void webBatchTestStringAppend() {}
#define PSRAM_JSON_DOC(name) JsonDocument name
enum class AllocPref { PreferPSRAM };
void* ps_alloc(size_t n, AllocPref, const char*) { return std::malloc(n); }
enum class ChatTurnRole { USER, ASSISTANT };
struct ChatTurnInfo {
  ChatTurnRole role; uint32_t textLen, tokenCount; uint16_t tokensPerSecX10; bool isStreaming;
};
std::vector<std::string> turns;
int chatGetTurnCount() { return turns.size(); }
bool chatGetTurnInfo(int i, ChatTurnInfo* out) {
  if (i < 0 || size_t(i) >= turns.size()) return false;
  *out = {i % 2 ? ChatTurnRole::ASSISTANT : ChatTurnRole::USER,
          static_cast<uint32_t>(turns[i].size()), 0, 0, false};
  return true;
}
int chatReadTurn(int i, int offset, char* out, int cap) {
  out[0] = 0;
  if (i < 0 || size_t(i) >= turns.size() || offset < 0 || size_t(offset) >= turns[i].size()) return 0;
  const size_t n = std::min(size_t(cap - 1), turns[i].size() - size_t(offset));
  std::memcpy(out, turns[i].data() + offset, n); out[n] = 0; return n;
}
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1;
struct httpd_req_t { std::string output; int sends = 0, failAt = -1; bool terminated = false; };
#define WEB_AUTH_OR_RETURN(req, ctx) do {} while (0)
void httpd_resp_set_type(httpd_req_t*, const char*) {}
esp_err_t httpd_resp_send_chunk(httpd_req_t* req, const char* text, size_t n) {
  if (req->sends++ == req->failAt) return ESP_FAIL;
  if (!text) req->terminated = true;
  else req->output.append(text, n);
  return ESP_OK;
}
// INSERT_PRODUCTION

static std::string readCli(int index) {
  int offset = 0;
  std::string result;
  for (unsigned pages = 0; pages < 100; ++pages) {
    const std::string command = "json " + std::to_string(index) + " " + std::to_string(offset);
    const char* output = cmd_llm_turns(command.c_str());
    assert(std::strlen(output) < 4096);
    JsonDocument doc;
    assert(!deserializeJson(doc, output));
    assert(doc["len"] == turns[index].size());
    result += doc["text"].as<std::string>();
    const int next = doc["next"];
    if (!doc["more"].as<bool>()) { assert(size_t(next) == turns[index].size()); return result; }
    assert(next > offset);
    offset = next;
  }
  assert(false); return {};
}

int main() {
  const std::string notice = "\n[Reply truncated on device]";
  turns = {"A short question", std::string(12000, 'a'), std::string(2047, 's'),
           std::string(kLlmReplyCapacityBytes - notice.size() - 1, 'z') + notice, ""};
  std::string unicode, escaped;
  for (int i = 0; i < 1400; ++i) { unicode += "caf\xc3\xa9 \xf0\x9f\x8c\x8d "; escaped += "\"\\\n\t"; }
  turns.push_back(unicode); turns.push_back(escaped);
  for (size_t i = 0; i < turns.size(); ++i) assert(readCli(i) == turns[i]);
  // Existing short-turn callers still receive their complete text in one call.
  JsonDocument shortTurn;
  assert(!deserializeJson(shortTurn, cmd_llm_turns("json 2")));
  assert(shortTurn["text"].as<std::string>() == turns[2] && !shortTurn["more"].as<bool>());
  JsonDocument end;
  assert(!deserializeJson(end, cmd_llm_turns("json 99")) && end["end"] == true);

  httpd_req_t request;
  assert(handleLLMChatTurns(&request) == ESP_OK && request.terminated);
  JsonDocument history;
  assert(!deserializeJson(history, request.output));
  assert(history.size() == turns.size());
  for (size_t i = 0; i < turns.size(); ++i) assert(history[i]["text"].as<std::string>() == turns[i]);
  // A broken streaming connection is reported, never followed by fake success.
  httpd_req_t broken; broken.failAt = 5;
  assert(handleLLMChatTurns(&broken) == ESP_FAIL && !broken.terminated);
  assert(broken.sends == 6);
  std::puts("Long LLM turns: CLI pagination, escaped/UTF-8 text, web streaming and send failure passed");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    commands = (COMPONENT / 'System_LLMCommands.cpp').read_text()
    header = (COMPONENT / 'System_Command.h').read_text()
    source = (COMPONENT / 'System_Command.cpp').read_text()
    code = block(header, 'class CommandArgs') + ';\nconst String CommandArgs::empty_;\n'
    code += '\n'.join(block(source, name) for name in (
        'CommandArgs::CommandArgs(', 'void CommandArgs::parse(', 'const String& CommandArgs::arg('))
    code += block((COMPONENT / 'System_Utils.cpp').read_text(), 'bool argLeadingTokenIsJson(')
    code += block(commands, 'static const char* cmd_llm_turns(')
    code += block((COMPONENT / 'WebPage_LLM.cpp').read_text(), 'static esp_err_t handleLLMChatTurns(')
    arduino = (HERE / 'web_batch_stubs/Arduino.h').read_text().replace(' private:\n',
        '  long toInt() const { return std::strtol(text_.c_str(), nullptr, 10); }\n private:\n')
    with tempfile.TemporaryDirectory(prefix='hw1-llm-turn-serving-') as directory:
        tmp = Path(directory)
        (tmp / 'Arduino.h').write_text(arduino)
        (tmp / 'test.cpp').write_text(HARNESS.replace('// INSERT_PRODUCTION', code))
        command = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
                   '-Wno-sign-compare', '-I', str(tmp), '-I', str(COMPONENT),
                   '-I', str(COMPONENT.parent / 'hardwareone_libs/ArduinoJson/src'),
                   str(tmp / 'test.cpp'), '-o', str(tmp / 'test')]
        if args.sanitize:
            command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        subprocess.run(command, check=True)
        subprocess.run([str(tmp / 'test')], check=True)


if __name__ == '__main__':
    main()
