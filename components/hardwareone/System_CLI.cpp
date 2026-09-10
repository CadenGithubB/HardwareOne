#include <string.h>

#include "OLED_Display.h"
#include "System_CLI.h"
#include "System_CLIMode.h"
#include "System_Command.h"
#include "System_CommandLimits.h"
#include "System_Debug.h"
#include "System_HelpPagerCore.h"
#include "System_Utils.h"
#include "System_WiFi.h"
#include "WebServer_Utils.h"

// External dependencies from main .ino
extern bool gCLIValidateOnly;

namespace {
struct HelpRenderSnapshot;
}

// Forward declarations
static void renderModuleHelp(const CommandModule* module, bool showAll);
static size_t renderModuleHelpPage(const CommandModule* module,
                                   const char* title,
                                   const HelpRenderSnapshot& snapshot,
                                   size_t requestedPage,
                                   bool emit);

// WebMirrorBuf, gWebMirror, gWebMirrorCap defined in WebServer_Utils.h

// ============================================================================
// Global CLI State
// ============================================================================

CLIState gCLIState = CLI_NORMAL;
bool gShowAllCommands = false;
volatile bool gInHelpRender = false;

namespace {

// The body deliberately leaves a fixed envelope for the clear sequence,
// module/page header, navigation footer and capture-inserted newlines. This is
// derived from the real command-result and debug-frame contracts: there is no
// second output buffer or transport-specific limit hidden in the help code.
static constexpr size_t kHelpCaptureChromeReserve = 1536;
static constexpr size_t kHelpBodyCaptureMax =
    (CMD_RESULT_MAX - 1) - kHelpCaptureChromeReserve;
static constexpr size_t kHelpBodyFrameMax = 32;
static constexpr size_t kHelpLineMax = DEBUG_MSG_SIZE - 16;
static constexpr size_t kHelpQueuePaceEvery = 8;
static constexpr size_t kHelpUtf8ScalarMax = 4;

static_assert(CMD_RESULT_MAX > kHelpCaptureChromeReserve + 1,
              "help chrome reserve must leave room for body text");
static_assert(kHelpLineMax < DEBUG_MSG_SIZE,
              "help lines must fit one debug-output frame");
static_assert(kHelpLineMax >= 48 + kHelpUtf8ScalarMax,
              "help lines must leave four bytes after the longest prefix");

struct HelpRenderSnapshot {
  bool connected = true;
  int wifiNetworkCount = 0;
};

struct HelpSessionState {
  const CommandModule* module = nullptr;
  size_t page = 1;
  size_t pageCount = 1;
  HelpRenderSnapshot snapshot;
};

static HelpSessionState sHelpSession;
static char sHelpError[128];

static void resetHelpSession() {
  sHelpSession = HelpSessionState{};
}

static const char* helpError(const char* format, size_t first = 0,
                             size_t second = 0) {
  snprintf(sHelpError, sizeof(sHelpError), format, first, second);
  return sHelpError;
}

static HelpRenderSnapshot captureHelpRenderSnapshot(
    const CommandModule* module) {
  HelpRenderSnapshot snapshot;
  const bool sensor = module && (module->flags & CMD_MODULE_SENSOR) != 0;
  snapshot.connected =
      !sensor || !module->isConnected || module->isConnected();
#if ENABLE_WIFI
  snapshot.wifiNetworkCount = gWifiNetworkCount;
#endif
  return snapshot;
}

}  // namespace

// ============================================================================
// Help mode — CLIMode adapter
// ============================================================================
// The help system is the canonical example of a CLIMode: short, non-blocking
// per-input handlers + persistent state in a side global (gCLIState). This
// adapter routes the dispatcher's per-line callbacks through the new
// CLIMode framework so the SAME machinery can host the upcoming wizard
// migration and confirm prompts.
//
// Implementation strategy for THIS phase: keep the existing
// handleHelpNavigation() logic unchanged underneath; the adapter just maps
// its true/false return into a CLIModeInputResult. The longer-term goal
// is to fold handleHelpNavigation INTO the onInput callback once other
// modes are stable; for now we minimize risk by delegating.

static void helpMode_onEnter(void* /*userData*/) {
  // cmd_help sets the requested page immediately after entry. Keeping this
  // callback side-effect free also makes prepared mode publication safe.
  DEBUGF(DEBUG_CLI, "[climode/help] entered (gCLIState=%d)", (int)gCLIState);
}

static CLIModeInputResult helpMode_onInput(const String& line, void* /*userData*/,
                                           char* out, size_t outSize) {
  // Delegate to the existing help-navigation handler. It writes "OK" into
  // out and returns true if it consumed the input ("back"/"exit"/"clear"/
  // "tail"/"sensors"/<module name>); false if the input is something else
  // and should be dispatched as a normal command.
  if (handleHelpNavigation(line, out, outSize)) {
    // handleHelpNavigation already called exitToNormalBanner() for the
    // back/exit cases, which set gCLIState back to CLI_NORMAL. Detect that
    // and propagate as HANDLED_AND_EXIT so the framework also clears its
    // own active-mode pointer; otherwise we'd be stuck with sActiveMode
    // pointing at us even though help is logically over.
    if (gCLIState == CLI_NORMAL) {
      return CLI_MODE_HANDLED_AND_EXIT;
    }
    return CLI_MODE_HANDLED;
  }
  // Let the registered help handler validate addressed forms while preserving
  // the current page until that validation succeeds. In particular,
  // `help espnow p999` must report an error without tearing down the session;
  // valid `help <module> p<N>` requests atomically replace the state below.
  String trimmed = line;
  trimmed.trim();
  const bool helpVerb = trimmed.length() >= 4 &&
      tolower(static_cast<unsigned char>(trimmed[0])) == 'h' &&
      tolower(static_cast<unsigned char>(trimmed[1])) == 'e' &&
      tolower(static_cast<unsigned char>(trimmed[2])) == 'l' &&
      tolower(static_cast<unsigned char>(trimmed[3])) == 'p';
  const bool addressedHelp = helpVerb &&
      (trimmed.length() == 4 ||
       isspace(static_cast<unsigned char>(trimmed[4])));
  if (addressedHelp) {
    return CLI_MODE_PASSTHROUGH;
  }
  // A normal command from the owning session leaves help before registry
  // dispatch. Foreign sessions never reach this callback, so they neither
  // exit nor mutate the owner's help state.
  return CLI_MODE_PASSTHROUGH_AND_EXIT;
}

static void helpMode_onExit(void* /*userData*/) {
  // If we exit via PASSTHROUGH_AND_EXIT or external cliExitMode(), the
  // dispatcher may not have called exitToNormalBanner() yet. Make sure
  // gCLIState is back to CLI_NORMAL so the rest of the codebase (debug
  // suppression, prompt rendering, web-mirror capture) returns to its
  // default state.
  if (gCLIState != CLI_NORMAL) {
    gCLIState = CLI_NORMAL;
    gShowAllCommands = false;
  }
  resetHelpSession();
  DEBUGF(DEBUG_CLI, "[climode/help] exited");
}

static const CLIMode kHelpMode = {
  "help",
  helpMode_onEnter,
  helpMode_onInput,
  helpMode_onExit,
  nullptr,  // onTick — help is purely input-driven
  nullptr,  // userData — help keeps state in gCLIState/gShowAllCommands
  10u * 60u * 1000u,
};

bool cliHelpModeOwnedBySession(CommandSource source,
                               TransportSessionEpoch epoch) {
  return cliModeOwnedBySession(source, epoch, &kHelpMode);
}

// ============================================================================
// Help Rendering Functions
// ============================================================================

const char* renderHelpMain(bool showAll) {
  broadcastOutput("\033[2J\033[H");
  broadcastOutput("════════════════════════════════════════════════════════════════");
  BROADCAST_PRINTF("  CLI Help Menu%s", showAll ? " (All Commands)" : "");
  broadcastOutput("════════════════════════════════════════════════════════════════");
  broadcastOutput("");
  broadcastOutput("Available Modules:");
  broadcastOutput("");

  // Get command modules and list them dynamically using metadata
  size_t moduleCount;
  const CommandModule* modules = getCommandModules(moduleCount);
  
  // Detection-gated help is deliberate and is KEPT: an inactive sensor module
  // still gets no row and no command block. What changed (2026-08-22) is that
  // its NAME is no longer swallowed entirely — a user cannot type `help gps` for
  // a module they were never told exists, and each sensor's own setup verbs
  // (gpsbus, gpsautostart, ...) now live in that module rather than in `i2c`.
  //
  // The label is "Not active", NOT "Not detected": only the isSensorConnected()
  // predicates are detection results. input / gamepad / anoencoder / camera read
  // a flag that ONLY a successful driver open sets, and microphone reads source
  // reachability — for those, "not detected" would be a false claim.
  char inactive[192];   // 15 module names = 92 chars + 28 separators = 120 worst case
  size_t inactiveLen = 0;
  bool   inactiveTrunc = false;
  inactive[0] = '\0';

  for (size_t i = 0; i < moduleCount; i++) {
    // Skip core modules (CLI internal commands)
    if (modules[i].flags & CMD_MODULE_CORE) {
      continue;
    }

    const char* moduleName = modules[i].name;
    const char* description = modules[i].description ? modules[i].description : "No description";
    const bool  isSensor = (modules[i].flags & CMD_MODULE_SENSOR) != 0;

    // A sensor row with a NULL predicate cannot be tested, so assert neither
    // state — print it plainly. This also matches renderModuleHelp, which
    // already treats a NULL predicate as connected.
    if (!isSensor || !modules[i].isConnected) {
      BROADCAST_PRINTF("  %-12s - %s", moduleName, description);
      continue;
    }

    if (modules[i].isConnected()) {
      BROADCAST_PRINTF("  %-12s - %s (Connected)", moduleName, description);
      continue;
    }

    if (showAll) {
      // `help all` used to print every module with no suffix at all, so it
      // listed the inactive ones without saying which were missing.
      BROADCAST_PRINTF("  %-12s - %s (Not active)", moduleName, description);
      continue;
    }

    const size_t nameLen = strlen(moduleName);
    const size_t sep = inactiveLen ? 2 : 0;
    if (inactiveLen + sep + nameLen < sizeof(inactive)) {
      if (sep) { inactive[inactiveLen++] = ','; inactive[inactiveLen++] = ' '; }
      memcpy(inactive + inactiveLen, moduleName, nameLen);
      inactiveLen += nameLen;
      inactive[inactiveLen] = '\0';
    } else {
      inactiveTrunc = true;
    }
  }

  if (inactiveLen) {
    broadcastOutput("");
    BROADCAST_PRINTF("  Not active: %s%s", inactive, inactiveTrunc ? ", ..." : "");
    broadcastOutput("    (not detected, or detected but not started)");
    broadcastOutput("    'help <name>' still lists their setup verbs - e.g. 'help gps' for gpsbus");
  }
  
  broadcastOutput("");
  broadcastOutput("────────────────────────────────────────────────────────────────");
  broadcastOutput("Core Commands:");
  broadcastOutput("  help [module] [p<N>] - Show a module help page");
  broadcastOutput("  help all       - Show all commands (including hidden)");
  broadcastOutput("  back           - Return to main help menu");
  broadcastOutput("  exit           - Exit help mode");
  broadcastOutput("  clear          - Clear screen");
  broadcastOutput("");
  broadcastOutput("Navigation:");
  broadcastOutput("  • Type a module name to view its commands (e.g., 'wifi')");
  broadcastOutput("  • On long modules, use 'p<N>', 'next', or 'prev' to move pages");
  broadcastOutput("  • Type 'help all' to see all commands (including disconnected)");
  broadcastOutput("────────────────────────────────────────────────────────────────");

  return "OK";
}

// Forward declaration (defined below)
static void renderHelpModuleByName(const char* moduleName, const char* title);

const char* renderHelpSystem() {
  renderHelpModuleByName("system", "System Commands");
  return "OK";
}

const char* renderHelpSettings() {
  renderHelpModuleByName("settings", "Settings & Configuration");
  return "OK";
}

const char* renderHelpAutomations() {
  renderHelpModuleByName("automation", "Automations - Scheduled Tasks & Conditional Commands");
  return "OK";
}

const char* renderHelpEspnow() {
  renderHelpModuleByName("espnow", "ESP-NOW - Wireless Peer-to-Peer Communication");
  return "OK";
}

const char* renderHelpWifi() {
  renderHelpModuleByName("wifi", "WiFi Network Management");
  return "OK";
}

const char* renderHelpSensors() {
  broadcastOutput("\033[2J\033[H");
  broadcastOutput("════════════════════════════════════════════════════════════════");
  BROADCAST_PRINTF("  Sensor Commands%s", gShowAllCommands ? " (All Available)" : " (Connected Only)");
  broadcastOutput("════════════════════════════════════════════════════════════════");
  broadcastOutput("");

  // Get command modules and render sensor modules dynamically
  size_t moduleCount;
  const CommandModule* modules = getCommandModules(moduleCount);
  
  // Render all sensor modules dynamically using CMD_MODULE_SENSOR flag
  for (size_t i = 0; i < moduleCount; i++) {
    if (modules[i].flags & CMD_MODULE_SENSOR) {
      renderModuleHelp(&modules[i], gShowAllCommands);
    }
  }

  broadcastOutput("────────────────────────────────────────────────────────────────");
  broadcastOutput("Navigation:");
  broadcastOutput("  • Type 'help sensors' to refresh this list");
  broadcastOutput("  • Type 'help all' to see disconnected sensors too");
  broadcastOutput("  • Type 'back' to return to help menu");
  broadcastOutput("  • Type 'exit' to return to CLI");
  broadcastOutput("────────────────────────────────────────────────────────────────");

  return "OK";
}

// ============================================================================
// CLI Navigation Functions
// ============================================================================

bool handleHelpNavigation(const String& cmd, char* out, size_t outSize) {
  const String safeCmdForTrace = redactCmdForAudit(cmd);
  DEBUGF(DEBUG_CLI, "[handleHelpNavigation] cmd='%s', gCLIState=%d", safeCmdForTrace.c_str(), (int)gCLIState);
  if (gCLIState == CLI_NORMAL) return false;

  gInHelpRender = true;

  // Helper lambda to write OK to out and clear the render flag
  auto respond = [&]() {
    if (out && outSize) {
      strncpy(out, "OK", outSize - 1);
      out[outSize - 1] = '\0';
    }
    gInHelpRender = false;
  };
  auto respondError = [&](const char* message) {
    if (out && outSize) {
      snprintf(out, outSize, "Error: %s", message ? message : "invalid help page");
    }
    gInHelpRender = false;
  };

  String lc = cmd;
  lc.toLowerCase();
  lc.trim();

  // ── Utility commands ─────────────────────────────────────────────────────
  // The help footer documents two distinct verbs:
  //   back  - return to the main help menu (one level up)
  //   exit  - leave help mode entirely (return to normal CLI)
  // The previous code treated them identically (both fully exited). Now
  // "back" only exits when we're already at the main menu; from a module
  // page it re-renders the main menu and stays in help mode.
  if (lc == "exit") {
    broadcastOutput(exitToNormalBanner());
    respond();
    return true;
  }
  if (lc == "back") {
    if (gCLIState == CLI_HELP_MAIN) {
      // Already at the top of the help hierarchy -- nowhere to go up to.
      // Treat as "exit" so a second `back` at the main menu still leaves
      // help mode (matches the legacy behavior at this level).
      broadcastOutput(exitToNormalBanner());
      respond();
      return true;
    }
    // Inside a module page (CLI_HELP_MODULE) -- go back to main menu.
    gCLIState = CLI_HELP_MAIN;
    resetHelpSession();
    renderHelpMain(gShowAllCommands);
    respond();
    return true;
  }
  if (lc == "clear") {
    broadcastOutput("\033[2J\033[H");
    respond();
    return true;
  }
  if (lc == "tail") {
    helpSuppressedTailDump();
    respond();
    return true;
  }

  // ── Sensors: special aggregate view (all sensor modules by flag) ──────────
  if (lc == "sensors") {
    gCLIState = CLI_HELP_MODULE;
    resetHelpSession();
    renderHelpSensors();
    respond();
    return true;
  }

  // ── Page navigation: mode-only, so registry/manifests stay unchanged ─────
  size_t requestedPage = 0;
  bool pageRequest = false;
  if (lc == "next" || lc == "prev") {
    pageRequest = true;
    if (!sHelpSession.module) {
      respondError("select a module before changing pages");
      return true;
    }
    if (lc == "next") {
      if (sHelpSession.page >= sHelpSession.pageCount) {
        respondError("already on the last help page");
        return true;
      }
      requestedPage = sHelpSession.page + 1;
    } else {
      if (sHelpSession.page <= 1) {
        respondError("already on the first help page");
        return true;
      }
      requestedPage = sHelpSession.page - 1;
    }
  } else {
    // Exact module names win before interpreting a p/P prefix. This keeps
    // modules such as "power" addressable while ensuring every other malformed
    // p... spelling remains inside help and receives a page-specific error.
    if (lc.indexOf(' ') == -1) {
      size_t moduleCount = 0;
      const CommandModule* modules = getCommandModules(moduleCount);
      for (size_t i = 0; i < moduleCount; ++i) {
        if ((modules[i].flags & CMD_MODULE_CORE) ||
            !lc.equalsIgnoreCase(modules[i].name)) {
          continue;
        }

        const HelpRenderSnapshot snapshot =
            captureHelpRenderSnapshot(&modules[i]);
        gCLIState = CLI_HELP_MODULE;
        const size_t pageCount = renderModuleHelpPage(
            &modules[i], nullptr, snapshot, 1, true);
        sHelpSession = HelpSessionState{&modules[i], 1, pageCount, snapshot};
        respond();
        return true;
      }
    }

    // A real registered command still leaves help and executes normally even
    // when its name begins with p (perftop, powersave, presence..., etc.). Only
    // an otherwise-unknown p/P-prefixed token is treated as a malformed page.
    if (findCommand(lc)) {
      gInHelpRender = false;
      return false;
    }

    const hw1_help_pager::PageToken parsed =
        hw1_help_pager::parsePageToken(lc.c_str(), lc.length());
    if (parsed.status == hw1_help_pager::PageTokenStatus::Invalid) {
      respondError("page must use p<N> with N starting at 1");
      return true;
    }
    if (parsed.status == hw1_help_pager::PageTokenStatus::Valid) {
      pageRequest = true;
      requestedPage = parsed.page;
    }
  }

  if (pageRequest) {
    if (!sHelpSession.module) {
      respondError("select a module before changing pages");
      return true;
    }
    if (requestedPage == 0 || requestedPage > sHelpSession.pageCount) {
      char detail[80];
      snprintf(detail, sizeof(detail), "page p%zu is out of range (p1-p%zu)",
               requestedPage, sHelpSession.pageCount);
      respondError(detail);
      return true;
    }

    const size_t pageCount = renderModuleHelpPage(
        sHelpSession.module, nullptr, sHelpSession.snapshot, requestedPage,
        true);
    sHelpSession.page = requestedPage;
    sHelpSession.pageCount = pageCount;
    respond();
    return true;
  }

  gInHelpRender = false;
  return false;  // Not a help navigation command
}

String exitToNormalBanner() {
  gCLIState = CLI_NORMAL;
  gShowAllCommands = false;  // Reset show all flag
  // Restore hidden history when leaving help
  String banner = "Returned to normal CLI mode.";
  return banner;
}

// ============================================================================
// CLI Command Implementations
// ============================================================================

static void broadcastHelpUsageIndented(const char* usage) {
  if (!usage || !usage[0]) {
    return;
  }

  const char* p = usage;
  while (*p) {
    const char* eol = strchr(p, '\n');
    size_t len = eol ? (size_t)(eol - p) : strlen(p);
    char line[200];
    if (len >= sizeof(line)) {
      len = sizeof(line) - 1;
    }
    memcpy(line, p, len);
    line[len] = '\0';

    if (line[0]) {
      bool hasPercent = (strchr(line, '%') != nullptr);
      bool canFormat = false;
      if (hasPercent) {
        canFormat = true;
        for (const char* q = line; *q; ++q) {
          if (*q != '%') continue;
          ++q;
          if (!*q) { canFormat = false; break; }
          if (*q == '%') continue;
          if (*q == 'd') continue;
          canFormat = false;
          break;
        }
      }

      if (hasPercent && canFormat) {
        char formatted[220];
        formatted[0] = '\0';

#if ENABLE_WIFI
        snprintf(formatted, sizeof(formatted), line, gWifiNetworkCount);
#else
        strncpy(formatted, line, sizeof(formatted) - 1);
        formatted[sizeof(formatted) - 1] = '\0';
#endif

        BROADCAST_PRINTF("  %-28s   %s", "", formatted);
      } else {
        BROADCAST_PRINTF("  %-28s   %s", "", line);
      }
    }

    if (!eol) {
      break;
    }
    p = eol + 1;
  }
}

namespace {

using HelpLineVisitor = void (*)(const char* line, size_t length, void* data);

struct HelpLineSource {
  const char* text;
  const char* firstPrefix;
  const char* continuationPrefix;
  bool formatWifiCount;
  bool blankLine;
};

static bool helpUsageFormatIsSafe(const char* text, size_t length) {
  bool hasPercent = false;
  size_t substitutions = 0;
  for (size_t i = 0; i < length; ++i) {
    if (text[i] != '%') continue;
    hasPercent = true;
    if (++i >= length || (text[i] != '%' && text[i] != 'd')) return false;
    if (text[i] == 'd' && ++substitutions > 1) return false;
  }
  return hasPercent;
}

// Walk one physical source line as one or more bounded UTF-8-safe output
// frames. The callback consumes the stack line synchronously; nothing is
// retained and no complete help body is materialized.
static void walkHelpSpan(const char* text, size_t length,
                         const char* firstPrefix,
                         const char* continuationPrefix,
                         bool& firstOutput,
                         HelpLineVisitor visitor, void* data) {
  if (!text || length == 0 || !visitor) return;

  size_t offset = 0;
  while (offset < length) {
    const char* prefix = firstOutput ? firstPrefix : continuationPrefix;
    if (!prefix) prefix = "";
    size_t prefixLength = strlen(prefix);
    if (prefixLength > kHelpLineMax - kHelpUtf8ScalarMax) {
      prefixLength = kHelpLineMax - kHelpUtf8ScalarMax;
    }
    const size_t available = kHelpLineMax - prefixLength;

    const size_t remaining = length - offset;
    size_t take = remaining < available ? remaining : available;
    size_t consumed = take;

    if (take < remaining) {
      // Prefer a whitespace boundary. If a single word is longer than one
      // frame, back up from the hard cut to a UTF-8 code-point boundary.
      size_t breakAt = take;
      while (breakAt > 0 && text[offset + breakAt - 1] != ' ' &&
             text[offset + breakAt - 1] != '\t') {
        --breakAt;
      }
      if (breakAt > 0) {
        take = breakAt - 1;
        while (take > 0 && (text[offset + take - 1] == ' ' ||
                            text[offset + take - 1] == '\t')) {
          --take;
        }
        consumed = breakAt;
        while (offset + consumed < length &&
               (text[offset + consumed] == ' ' ||
                text[offset + consumed] == '\t')) {
          ++consumed;
        }
      } else {
        while (take > 0 && offset + take < length &&
               (static_cast<unsigned char>(text[offset + take]) & 0xC0u) ==
                   0x80u) {
          --take;
        }
        if (take == 0) take = remaining < available ? remaining : available;
        consumed = take;
      }
    }

    char line[DEBUG_MSG_SIZE];
    memcpy(line, prefix, prefixLength);
    memcpy(line + prefixLength, text + offset, take);
    const size_t lineLength = prefixLength + take;
    line[lineLength] = '\0';
    visitor(line, lineLength, data);
    firstOutput = false;
    offset += consumed;
  }
}

static void walkHelpText(const HelpLineSource& source,
                         int wifiNetworkCount,
                         HelpLineVisitor visitor, void* data) {
  if (source.blankLine) {
    visitor("", 0, data);
    return;
  }
  if (!source.text || !source.text[0]) return;

  bool firstOutput = true;
  const char* cursor = source.text;
  while (*cursor) {
    const char* eol = strchr(cursor, '\n');
    const size_t length = eol ? static_cast<size_t>(eol - cursor)
                              : strlen(cursor);
    if (length > 0) {
      // Preserve the legacy, deliberately narrow `%d`/`%%` substitution used
      // by Wi-Fi usage text. Short safe lines are formatted before wrapping;
      // arbitrary percent-bearing help is always treated as plain text.
      if (source.formatWifiCount && length < kHelpLineMax &&
          helpUsageFormatIsSafe(cursor, length)) {
        char format[DEBUG_MSG_SIZE];
        char formatted[DEBUG_MSG_SIZE];
        memcpy(format, cursor, length);
        format[length] = '\0';
#if ENABLE_WIFI
        snprintf(formatted, sizeof(formatted), format, wifiNetworkCount);
#else
        memcpy(formatted, format, length + 1);
#endif
        walkHelpSpan(formatted, strlen(formatted), source.firstPrefix,
                     source.continuationPrefix, firstOutput, visitor, data);
      } else {
        walkHelpSpan(cursor, length, source.firstPrefix,
                     source.continuationPrefix, firstOutput, visitor, data);
      }
    } else {
      // A newline inside a help/usage string is part of its presentation and
      // page cost. Do not manufacture an extra blank for a final trailing
      // newline: the loop ends naturally once cursor reaches the terminator.
      visitor("", 0, data);
    }
    if (!eol) break;
    cursor = eol + 1;
  }
}

struct HelpCostAccumulator {
  hw1_help_pager::Cost cost;
};

static size_t helpLineCaptureBytes(size_t length) {
  // broadcastOutput("") queues a visual frame but command capture appends
  // nothing for a zero-length payload. Emit one space for logical blank lines
  // so serial and captured web output retain the same row.
  return (length ? length : 1) + 1;
}

static void measureHelpLine(const char* /*line*/, size_t length, void* data) {
  auto* accumulator = static_cast<HelpCostAccumulator*>(data);
  accumulator->cost.captureBytes += helpLineCaptureBytes(length);
  ++accumulator->cost.logicalFrames;
}

struct HelpPlacementSink {
  hw1_help_pager::Pager* pager;
  size_t selectedPage;
  bool emit;
  size_t emittedFrames;
};

static void placeHelpLine(const char* line, size_t length, void* data) {
  auto* sink = static_cast<HelpPlacementSink*>(data);
  const hw1_help_pager::Placement placement =
      sink->pager->addItem(
          hw1_help_pager::Cost{helpLineCaptureBytes(length), 1});
  if (!sink->emit || placement.page != sink->selectedPage) return;

  broadcastOutput(length ? line : " ");
  ++sink->emittedFrames;
  if ((sink->emittedFrames % kHelpQueuePaceEvery) == 0) {
    debugQueueBackpressure();
  }
}

static void processHelpGroup(hw1_help_pager::Pager& pager,
                             const HelpLineSource* sources,
                             size_t sourceCount,
                             const HelpRenderSnapshot& snapshot,
                             size_t selectedPage,
                             bool emit,
                             size_t& emittedFrames) {
  HelpCostAccumulator measured;
  for (size_t i = 0; i < sourceCount; ++i) {
    walkHelpText(sources[i], snapshot.wifiNetworkCount, measureHelpLine,
                 &measured);
  }
  if (hw1_help_pager::costIsZero(measured.cost)) return;

  pager.beginGroup(measured.cost);
  HelpPlacementSink sink{&pager, selectedPage, emit, emittedFrames};
  for (size_t i = 0; i < sourceCount; ++i) {
    walkHelpText(sources[i], snapshot.wifiNetworkCount, placeHelpLine, &sink);
  }
  emittedFrames = sink.emittedFrames;
}

static void walkModulePageBody(const CommandModule* module,
                               const HelpRenderSnapshot& snapshot,
                               hw1_help_pager::Pager& pager,
                               size_t selectedPage,
                               bool emit) {
  if (!module) return;
  size_t emittedFrames = 0;

  char upperName[40];
  size_t nameLength = strlen(module->name);
  if (nameLength >= sizeof(upperName) - 11) nameLength = sizeof(upperName) - 12;
  for (size_t i = 0; i < nameLength; ++i) {
    upperName[i] = toupper(static_cast<unsigned char>(module->name[i]));
  }
  upperName[nameLength] = '\0';

  char moduleHeading[64];
  const bool sensor = (module->flags & CMD_MODULE_SENSOR) != 0;
  const bool connected = snapshot.connected;
  if (sensor) {
    snprintf(moduleHeading, sizeof(moduleHeading), "%.28s Commands (%s):",
             upperName, connected ? "Connected" : "Not Connected");
  } else {
    snprintf(moduleHeading, sizeof(moduleHeading), "%.28s Commands:", upperName);
  }

  const HelpLineSource introduction[] = {
      {moduleHeading, "", "", false, false},
      {module->long_description, "", "", false, false},
      {nullptr, "", "", false, true},
  };
  processHelpGroup(pager, introduction,
                   sizeof(introduction) / sizeof(introduction[0]),
                   snapshot, selectedPage, emit, emittedFrames);

  if (sensor) {
    const HelpLineSource status[] = {
        {connected ? "  • Module is active and ready"
                   : "  • Module not detected or not initialized",
         "", "", false, false},
    };
    processHelpGroup(pager, status, 1, snapshot, selectedPage, emit,
                     emittedFrames);
  }

  const size_t count = module->commandCount();
  for (size_t i = 0; i < count; ++i) {
    const CommandEntry& command = module->commands[i];
    if (!command.help) continue;

    char commandPrefix[48];
    char continuationPrefix[48];
    snprintf(commandPrefix, sizeof(commandPrefix), "  %-28.30s - ",
             command.name);
    snprintf(continuationPrefix, sizeof(continuationPrefix), "  %-28s   ", "");
    const HelpLineSource commandGroup[] = {
        {command.help, commandPrefix, continuationPrefix, false, false},
        {command.usage, continuationPrefix, continuationPrefix, true, false},
    };
    processHelpGroup(pager, commandGroup,
                     sizeof(commandGroup) / sizeof(commandGroup[0]),
                     snapshot, selectedPage, emit, emittedFrames);
  }

}

static hw1_help_pager::Pager makeHelpPager() {
  return hw1_help_pager::Pager(hw1_help_pager::Limits{
      kHelpBodyCaptureMax, kHelpBodyFrameMax});
}

}  // namespace

// Helper function to render help for a specific module
static void renderModuleHelp(const CommandModule* module, bool showAll) {
  const char* moduleName = module->name;
  const CommandEntry* commands = module->commands;
  const size_t count = module->commandCount();
  bool isSensorModule = (module->flags & CMD_MODULE_SENSOR) != 0;
  
  bool isConnected = true;
  if (isSensorModule && module->isConnected) {
    isConnected = module->isConnected();
  }
  
  // Show module if connected or if showing all commands
  if (showAll || isConnected || !isSensorModule) {
    // Module header — stack buffer, no heap allocation
    char upperName[32];
    size_t nameLen = strlen(moduleName);
    if (nameLen >= sizeof(upperName)) nameLen = sizeof(upperName) - 1;
    for (size_t j = 0; j < nameLen; j++) upperName[j] = toupper((unsigned char)moduleName[j]);
    upperName[nameLen] = '\0';

    if (isSensorModule) {
      BROADCAST_PRINTF("%s Commands%s:",
                       upperName,
                       isConnected ? " (Connected)" : " (Not Connected)");
    } else {
      BROADCAST_PRINTF("%s Commands:", upperName);
    }

    // Subsystem overview (the module's "how this works" blurb), printed above the
    // command list on `help <module>` / `help all`. The bare module list stays curt.
    if (module->long_description && module->long_description[0]) {
      broadcastOutput(module->long_description);
      broadcastOutput("");
    }
    
    // Show connection status for sensors
    if (isSensorModule) {
      if (isConnected) {
        broadcastOutput("  • Module is active and ready");
      } else {
        broadcastOutput("  • Module not detected or not initialized");
      }
    }
    
    // List all commands in this module
    for (size_t i = 0; i < count; i++) {
      if (commands[i].help) {
        BROADCAST_PRINTF("  %-28s - %s", commands[i].name, commands[i].help);
        broadcastHelpUsageIndented(commands[i].usage);
      }
    }
    broadcastOutput("");
  }
}

// ============================================================================
// Shared Help Helpers
// ============================================================================

// Standard navigation footer printed at the bottom of every module help page.
static void broadcastHelpNavFooter(const char* moduleName, size_t page,
                                   size_t pageCount) {
  char line[DEBUG_MSG_SIZE];
  broadcastOutput("────────────────────────────────────────────────────────────────");
  if (pageCount > 1) {
    snprintf(line, sizeof(line), "Page %u of %u",
             static_cast<unsigned>(page),
             static_cast<unsigned>(pageCount));
    broadcastOutput(line);
  }
  broadcastOutput("Navigation:");
  if (pageCount > 1 && page < pageCount) {
    snprintf(line, sizeof(line), "  • Next: 'help %.*s p%u' (or 'next')",
             32, moduleName, static_cast<unsigned>(page + 1));
    broadcastOutput(line);
  }
  if (pageCount > 1 && page > 1) {
    snprintf(line, sizeof(line), "  • Previous: 'help %.*s p%u' (or 'prev')",
             32, moduleName, static_cast<unsigned>(page - 1));
    broadcastOutput(line);
  }
  if (pageCount > 1) {
    snprintf(line, sizeof(line),
             "  • Jump: 'help %.*s p<N>' (or 'p<N>' in help mode)",
             32, moduleName);
    broadcastOutput(line);
  } else {
    snprintf(line, sizeof(line), "  • Type 'help %.*s' to refresh this page",
             32, moduleName);
    broadcastOutput(line);
  }
  broadcastOutput("  • Type 'back' to return to help menu");
  broadcastOutput("  • Type 'exit' to return to CLI");
  broadcastOutput("────────────────────────────────────────────────────────────────");
}

static const CommandModule* findHelpModule(const char* moduleName) {
  if (!moduleName) return nullptr;
  size_t moduleCount = 0;
  const CommandModule* modules = getCommandModules(moduleCount);
  for (size_t i = 0; i < moduleCount; ++i) {
    if (strcmp(modules[i].name, moduleName) == 0) return &modules[i];
  }
  return nullptr;
}

// Plan the body once and, when requested, deterministically replay the same
// item stream to emit only one selected page. The replay holds no page table or
// full text buffer; its only mutable storage is the small Pager state and one
// DEBUG_MSG_SIZE line on the stack.
static size_t renderModuleHelpPage(const CommandModule* module,
                                   const char* title,
                                   const HelpRenderSnapshot& snapshot,
                                   size_t requestedPage,
                                   bool emit) {
  if (!module) return 0;

  hw1_help_pager::Pager planner = makeHelpPager();
  walkModulePageBody(module, snapshot, planner, 0, false);
  const size_t pageCount = planner.pageCount() ? planner.pageCount() : 1;
  if (!emit || requestedPage == 0 || requestedPage > pageCount) {
    return pageCount;
  }

  // Account for frames already queued by the command that led here before
  // adding the page chrome. The body continues pacing every few frames.
  debugQueueBackpressure();
  char line[DEBUG_MSG_SIZE];
  broadcastOutput("\033[2J\033[H");
  broadcastOutput("════════════════════════════════════════════════════════════════");
  if (title) {
    if (pageCount > 1) {
      snprintf(line, sizeof(line), "  %.*s — Page %u/%u", 96, title,
               static_cast<unsigned>(requestedPage),
               static_cast<unsigned>(pageCount));
    } else {
      snprintf(line, sizeof(line), "  %.*s", 96, title);
    }
  } else {
    char upper[40];
    size_t i = 0;
    for (; module->name[i] && i < sizeof(upper) - 1; ++i) {
      upper[i] = toupper(static_cast<unsigned char>(module->name[i]));
    }
    upper[i] = '\0';
    if (pageCount > 1) {
      snprintf(line, sizeof(line), "  %s Module — Page %u/%u", upper,
               static_cast<unsigned>(requestedPage),
               static_cast<unsigned>(pageCount));
    } else {
      snprintf(line, sizeof(line), "  %s Module", upper);
    }
  }
  broadcastOutput(line);
  broadcastOutput("════════════════════════════════════════════════════════════════");
  broadcastOutput("");

  hw1_help_pager::Pager renderer = makeHelpPager();
  walkModulePageBody(module, snapshot, renderer, requestedPage, true);
  broadcastOutput("");
  broadcastHelpNavFooter(module->name, requestedPage, pageCount);
  debugQueueBackpressure();
  return pageCount;
}

// Render help for a single named module looked up from the command registry.
// title may be nullptr — if so the title is derived from the module name.
// All output is broadcast directly; nothing is returned.
static void renderHelpModuleByName(const char* moduleName, const char* title) {
  const CommandModule* module = findHelpModule(moduleName);
  if (module) {
    const HelpRenderSnapshot snapshot = captureHelpRenderSnapshot(module);
    (void)renderModuleHelpPage(module, title, snapshot, 1, true);
  } else {
    broadcastOutput("\033[2J\033[H");
    BROADCAST_PRINTF("  (No commands registered for module '%s')", moduleName);
    broadcastHelpNavFooter(moduleName, 1, 1);
  }
}

struct HelpModeEntryState {
  CLIState cliState;
  bool showAll;
  HelpSessionState session;
};

static void commitHelpModeEntry(void* data) {
  const auto* entry = static_cast<const HelpModeEntryState*>(data);
  gCLIState = entry->cliState;
  gShowAllCommands = entry->showAll;
  sHelpSession = entry->session;
}

static const char* prepareInteractiveHelp(bool interactive,
                                          const HelpModeEntryState& entry) {
  if (!interactive) return nullptr;
  if (cliModeCurrentCommandOwns(&kHelpMode)) {
    commitHelpModeEntry(const_cast<HelpModeEntryState*>(&entry));
    return nullptr;
  }
  if (cliInModeActive()) {
    return "Error: another interactive session is already active.";
  }
  HelpModeEntryState copy = entry;
  if (!cliEnterModePrepared(&kHelpMode, commitHelpModeEntry, &copy)) {
    return "Error: this command source cannot open an interactive session.";
  }
  return nullptr;
}

static const char* cmd_help(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!gWebMirror.buf) { gWebMirror.init(gWebMirrorCap); }

  const bool interactive = cliModeCurrentInvocationCanInteract();
  CommandArgs args(argsInput);
  if (args.unterminatedQuote() || args.count() > 2) {
    return "Error: usage is help [<module> [p<N>]|sensors|all|tail].";
  }

  // ── Plain "help" — enter / return to main help menu ──────────────────────
  if (args.count() == 0) {
    const HelpModeEntryState entry{
        CLI_HELP_MAIN, false, HelpSessionState{}};
    if (const char* error = prepareInteractiveHelp(interactive, entry)) {
      return error;
    }
    if (interactive) {
      gWebMirror.clear();
      DEBUGF(DEBUG_CLI, "[cmd_help] gCLIState -> CLI_HELP_MAIN");
    }
    gInHelpRender = true;
    renderHelpMain(false);
    gInHelpRender = false;
    return "OK";
  }

  String topic = args.arg(0);
  topic.toLowerCase();

  // ── Meta-commands ─────────────────────────────────────────────────────────
  if (topic == "all") {
    if (args.count() != 1) {
      return "Error: help all does not take a page.";
    }
    const HelpModeEntryState entry{
        CLI_HELP_MAIN, true, HelpSessionState{}};
    if (const char* error = prepareInteractiveHelp(interactive, entry)) {
      return error;
    }
    gInHelpRender = true;
    renderHelpMain(true);
    gInHelpRender = false;
    return "OK";
  }
  if (topic == "tail") {
    if (args.count() != 1) {
      return "Error: help tail does not take a page.";
    }
    gInHelpRender = true;
    helpSuppressedTailDump();
    gInHelpRender = false;
    return "OK";
  }

  // ── Sensors: aggregate view across all sensor modules ─────────────────────
  if (topic == "sensors") {
    if (args.count() != 1) {
      return "Error: aggregate sensor help is not paged; use help <sensor> p<N>.";
    }
    const HelpModeEntryState entry{
        CLI_HELP_MODULE, false, HelpSessionState{}};
    if (const char* error = prepareInteractiveHelp(interactive, entry)) {
      return error;
    }
    gInHelpRender = true;
    renderHelpSensors();
    gInHelpRender = false;
    return "OK";
  }

  // ── Dynamic lookup: match any registered module name ─────────────────────
  size_t moduleCount = 0;
  const CommandModule* modules = getCommandModules(moduleCount);
  const CommandModule* module = nullptr;
  for (size_t i = 0; i < moduleCount; ++i) {
    if (topic.equalsIgnoreCase(modules[i].name)) {
      module = &modules[i];
      break;
    }
  }
  if (!module) {
    return "Error: unknown help topic; type help to list available modules.";
  }

  size_t requestedPage = 1;
  if (args.count() == 2) {
    const String& token = args.arg(1);
    const hw1_help_pager::PageToken parsed =
        hw1_help_pager::parsePageToken(token.c_str(), token.length());
    if (parsed.status != hw1_help_pager::PageTokenStatus::Valid) {
      return "Error: page must use p<N> with N starting at 1 (for example, p2).";
    }
    requestedPage = parsed.page;
  }

  const HelpRenderSnapshot snapshot = captureHelpRenderSnapshot(module);
  const size_t pageCount =
      renderModuleHelpPage(module, nullptr, snapshot, 1, false);
  if (requestedPage > pageCount) {
    return helpError("Error: page p%zu is out of range (p1-p%zu).",
                     requestedPage, pageCount);
  }

  const HelpModeEntryState entry{
      CLI_HELP_MODULE, false,
      HelpSessionState{module, requestedPage, pageCount, snapshot}};
  if (const char* error = prepareInteractiveHelp(interactive, entry)) {
    return error;
  }

  gInHelpRender = true;
  const size_t renderedPageCount =
      renderModuleHelpPage(module, nullptr, snapshot, requestedPage, true);
  gInHelpRender = false;
  if (interactive) sHelpSession.pageCount = renderedPageCount;
  return "OK";
}

static const char* cmd_back(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (cliModeCurrentCommandOwns(&kHelpMode)) {
    gCLIState = CLI_HELP_MAIN;
    resetHelpSession();
    gInHelpRender = true;
    renderHelpMain(gShowAllCommands);
    gInHelpRender = false;
    return "OK";
  }
  return "Not in help mode.";
}

static const char* cmd_exit(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (cliModeCurrentCommandOwns(&kHelpMode)) {
    String banner = exitToNormalBanner();
    broadcastOutput(banner);
    helpSuppressedPrintAndReset();
    return "OK";
  }
  return "Already in normal CLI mode.";
}

static const char* cmd_clear(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!gWebMirror.buf) { gWebMirror.init(gWebMirrorCap); }
  gWebMirror.clear();
  return "\033[2J\033[H"
         "CLI history cleared.";
}

// ============================================================================
// CLI Command Registry
// ============================================================================

// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
const CommandEntry cliCommands[] = {
  { "help", "Display help menu (module results use p<N> pages)", false, cmd_help,
    "Usage: help [<module> [p<N>]|sensors|all|tail]\n"
    "  Example: help espnow p2\n"
    "  In help mode: p<N>, next, prev, back, exit" },
  { "back", "Return to main help menu", false, cmd_back },
  { "exit", "Exit help mode", false, cmd_exit },
  { "clear", "Clear CLI history", false, cmd_clear },
};

const size_t cliCommandsCount = sizeof(cliCommands) / sizeof(cliCommands[0]);

// Registration handled by gCommandModules[] in System_Utils.cpp
