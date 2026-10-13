#!/usr/bin/env python3
"""The structured Add-WiFi line reaches cmd_wifiadd unchanged.

UI adapters (the OLED and G2 Add-WiFi forms) hand ssid, password, priority and
hidden to the saved-network owner's wifiAddCommandLine, which refuses a field
the quoted line cannot carry (a `"` or a control character) and otherwise
writes the one `wifiadd` line they submit through their session-fenced
executor path; the registry's admin row, cmd_exec and the password-masking
audit then apply as for any other caller.

Compiled unchanged: the production CommandArgs tokenizer, cmd_wifiadd
(System_WiFi.cpp) and the owner's field rule and line writer
(network/wifi/System_WiFiNetworks.cpp). The saved-network store behind
cmd_wifiadd is a recording double (the owner's own refusals run in
wifi_networks). Checked: every written line parses back to exactly the fields
it was given, for names with spaces, edge spaces, backslashes and quotes of
the other kind; refused fields write nothing; and when the store refuses a
name or password with a control character, cmd_wifiadd says so instead of
reporting a full list. --component replays against another tree.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

HARNESS = r'''
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); std::exit(1); } } while (0)

class String {
  std::string s_;
 public:
  String() = default;
  String(const char* value) : s_(value ? value : "") {}
  String(const std::string& value) : s_(value) {}
  String& operator=(const char* value) { s_ = value ? value : ""; return *this; }
  String& operator+=(const String& other) { s_ += other.s_; return *this; }
  String& operator+=(const char* other) { s_ += other ? other : ""; return *this; }
  char operator[](size_t at) const { return at < s_.size() ? s_[at] : '\0'; }
  bool operator==(const String& other) const { return s_ == other.s_; }
  bool operator==(const char* other) const { return s_ == (other ? other : ""); }
  size_t length() const { return s_.size(); }
  const char* c_str() const { return s_.c_str(); }
  int indexOf(char value) const {
    const auto at = s_.find(value);
    return at == std::string::npos ? -1 : static_cast<int>(at);
  }
  String substring(size_t from, size_t to) const { return s_.substr(from, to - from); }
  int toInt() const { return std::atoi(s_.c_str()); }
  void toLowerCase() { for (auto& c : s_) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
  void trim() {
    const auto first = s_.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { s_.clear(); return; }
    s_ = s_.substr(first, s_.find_last_not_of(" \t\r\n") - first + 1);
  }
};

// INSERT_TOKENIZER

// The saved-network store behind cmd_wifiadd: records what reached it.
struct Added { std::string ssid, pass; int priority; bool hidden; };
static std::vector<Added> added;
static bool storeAccepts = true;
// INSERT_OWNER_TYPES
bool upsertWiFiNetwork(const String& ssid, const String& pass, int priority, bool hidden) {
  added.push_back({ssid.c_str(), pass.c_str(), priority, hidden});
  return storeAccepts;
}
void sortWiFiByPriority() {}
bool saveWiFiNetworks() { return true; }
enum { SYSEVT_WIFI_NET_ADDED = 1 };
void systemEventPost(int, const char*, const char* = nullptr) {}
static char debugBuffer[1024];
static char* getDebugBuffer() { return debugBuffer; }
#define RETURN_VALID_IF_VALIDATE_CSTR() ((void)0)

// INSERT_PRODUCTION

struct Fields { const char* ssid; const char* pass; int priority; bool hidden; };

int main() {
#if HAVE_STRUCTURED_ADD
  static const std::string longPass(64, 'p');
  const Fields cases[] = {
    {"My Net", "p w d", 0, false},
    {"  edge spaces  ", " pass ", 1, false},
    {"back\\slash", "it's-a \\ pass", 3, false},
    {"semi;pipe|amp&", "", 1, false},
    {"Guest", "", 0, true},
    {"ssid-is-exactly-thirty-two-chars", longPass.c_str(), 2, false},
  };
  for (const Fields& f : cases) {
    String line;
    CHECK(wifiAddCommandLine(f.ssid, f.pass, f.priority, f.hidden, line) == WifiFieldProblem::None);
    const std::string text = line.c_str();
    CHECK(text.rfind("wifiadd ", 0) == 0);
    // The registry hands the handler the text after the command name.
    added.clear();
    storeAccepts = true;
    const char* reply = cmd_wifiadd(String(text.substr(std::strlen("wifiadd "))));
    CHECK(reply && std::strncmp(reply, "Saved network '", 15) == 0);
    CHECK(added.size() == 1 && added[0].ssid == f.ssid && added[0].pass == f.pass);
    const bool fourArguments = f.priority > 0 || f.hidden;
    CHECK(added[0].priority == (fourArguments ? (f.priority > 0 ? f.priority : 1) : 0));
    CHECK(added[0].hidden == f.hidden);
  }
  // A field the line cannot carry writes nothing.
  for (const Fields f : {Fields{"x\" \"y", "secret", 1, false}, Fields{"home", "pa\"ss", 0, false},
                         Fields{"Lab\tNet", "secret", 1, false}, Fields{"home", "pa\x01ss", 0, false},
                         Fields{"home\x7f", "secret", 0, false}}) {
    String line = "stale";
    CHECK(wifiAddCommandLine(f.ssid, f.pass, f.priority, f.hidden, line) != WifiFieldProblem::None);
    CHECK(line.length() == 0);
  }
#endif
  // The store refuses a control character: the reply names that reason.
  added.clear();
  storeAccepts = false;
  const char* reply = cmd_wifiadd("\"Lab\tNet\" \"secret\"");
  CHECK(added.size() == 1 && added[0].ssid == "Lab\tNet");
  CHECK(reply && std::strstr(reply, "control characters") != nullptr);
  reply = cmd_wifiadd("\"home\" \"pa\x01ss\" 1 0");
  CHECK(reply && std::strstr(reply, "control characters") != nullptr);
  // The other refusals keep their own replies.
  CHECK(std::strstr(cmd_wifiadd("x a\"b"), "double-quote") != nullptr);  // bare token keeps the quote
  CHECK(std::strstr(cmd_wifiadd("\"home\" \"secret\""), "list is full") != nullptr);
  std::printf("wifi structured add: %d checks passed\n", checks);
  return 0;
}
'''


def tokenizer(component: Path) -> str:
    header = (component / "System_Command.h").read_text()
    source = (component / "System_Command.cpp").read_text()
    utils = (component / "System_Utils.h").read_text()
    pieces = [extract_block(utils, "inline int parseBoolArg(")]
    pieces.append(extract_block(header, "class CommandArgs") + ";")
    pieces.append("const String CommandArgs::empty_;")
    pieces += [extract_block(source, marker) for marker in (
        "CommandArgs::CommandArgs(", "void CommandArgs::parse(", "const String& CommandArgs::arg(",
        "int CommandArgs::argInt(", "bool CommandArgs::argBool(")]
    return "\n\n".join(pieces)


def owner(component: Path) -> tuple[str, str, bool]:
    header = (component / "network/wifi/System_WiFiNetworks.h").read_text()
    source = (component / "network/wifi/System_WiFiNetworks.cpp").read_text()
    capacity = re.search(r"(?m)^#define MAX_WIFI_NETWORKS \d+", header)
    require(capacity is not None, "System_WiFiNetworks.h defines MAX_WIFI_NETWORKS")
    types = capacity[0]
    functions = []
    structured = "enum class WifiFieldProblem" in header
    if structured:
        types += "\n" + extract_block(header, "enum class WifiFieldProblem") + ";"
        functions = [extract_block(source, "WifiFieldProblem wifiNetworkFieldProblem("),
                     extract_block(source, "WifiFieldProblem wifiAddCommandLine(")]
    wifi = (component / "System_WiFi.cpp").read_text()
    require(wifi.count("const char* cmd_wifiadd(") == 1, "System_WiFi.cpp: one cmd_wifiadd")
    functions.append(extract_block(wifi, "const char* cmd_wifiadd("))
    return types, "\n\n".join(functions), structured


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="components/hardwareone directory to read production sources from")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    types, production, structured = owner(args.component)
    unit_text = (f"#define HAVE_STRUCTURED_ADD {int(structured)}\n" +
                 HARNESS.replace("// INSERT_TOKENIZER", tokenizer(args.component))
                        .replace("// INSERT_OWNER_TYPES", types)
                        .replace("// INSERT_PRODUCTION", production))
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wno-unused-parameter"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-wifi-structured-add-") as temporary:
        unit, binary = Path(temporary) / "test.cpp", Path(temporary) / "test"
        unit.write_text(unit_text)
        subprocess.run([args.cxx, *flags, str(unit), "-o", str(binary)], check=True, timeout=120)
        subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
