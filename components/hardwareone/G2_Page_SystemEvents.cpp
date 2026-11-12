// =============================================================================
// G2 glasses: the System Events viewer
// =============================================================================
// Part of the glasses client (G2_Glasses.cpp). The shared state and the core
// functions this file uses are declared in G2_GlassesInternal.h.

#include "G2_Glasses.h"
#include "System_Events.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "BLE_Server.h"

#include "G2_Page_Common.h"
#include "G2_GlassesInternal.h"

// ─────────────────────────────────────────────────────────────────────
// System Events viewer — read-only PAGED view of the device's typed event
// ring (the same 48-entry ring the `events` CLI and the OLED notification
// view read). Reached from the System launcher.
//
// Was a live-text compound (a one-row "<- System" list + a text child
// refreshed every second). That shape could only ever show the newest 8
// events, each hard-clipped to 33 characters, and — because the lens list
// widget owns focus — the only tappable thing on the page was the Back row,
// so there was no way to reach the other 40 entries. Now it uses the shared
// TextPager engine (System_TextPager.h) exactly like the Files text viewer:
// the whole ring is snapshotted, wrapped at the lens column width, and split
// into pages that tap/scroll walk through. No list widget, so no focus trap;
// double-tap is the exit.
//
// Events are walked newest-first via systemEventGetBySeq (copies one event by
// seq, no cursor mutation), so the file sink and cursor-drain consumers are
// untouched. The snapshot is taken on entry — paging never re-reads under the
// user's feet — except when a forward tap wraps off the last page back to the
// newest, which re-reads the ring (see g2SysEventsNav).
//
// NOTE: this is the ESP32-side typed event ring, NOT the G2's own native
// firmware notification system (a separate, deferred effort).
// ─────────────────────────────────────────────────────────────────────
#define SYSEV_TEXT_MAX_PAGES     24
#define SYSEV_TEXT_PAGE_BUDGET   176   // < 180 proven-safe single-fragment body
// Displayable ceiling ≈ maxPages × budget. A full ring of long-detail events
// (e.g. text_rx with an 80-byte preview) can outgrow it; the oldest entries
// fall off the end and g2TextPagerRender stamps "...[truncated]".
#define SYSEV_TEXT_BODY_CAP      (SYSEV_TEXT_MAX_PAGES * SYSEV_TEXT_PAGE_BUDGET + 128)

EXT_RAM_BSS_ATTR static char gSysEvBody[SYSEV_TEXT_BODY_CAP];          // wrapped body
static uint16_t              gSysEvPageOff[SYSEV_TEXT_MAX_PAGES + 1];  // page offsets
// Render scratch = header + separator + one page slice. Sized for the
// worst-case slice rather than the budget: a single event whose block alone
// exceeds the budget gets a page to itself and overshoots (see the splitter in
// buildSysEventsPages). Ceiling for one block is ~260 B — line 1 plus a
// 48-byte subject, 80-byte detail, attribution and wrap indents.
EXT_RAM_BSS_ATTR static char gSysEvPageBuf[SYSEV_TEXT_PAGE_BUDGET + 384];  // render scratch
static char                  gSysEvTitle[28] = {0};
static TextPager gSysEvPager = {
    gSysEvBody, gSysEvPageOff, SYSEV_TEXT_MAX_PAGES, SYSEV_TEXT_PAGE_BUDGET,
    /*pageCount=*/0, /*curPage=*/0, /*truncated=*/false };

static bool renderSysEventsPage();
static void g2SysEventsNav(G2TapKind kind);

// Copy printable ASCII only. Event who/subject/detail can carry remote-origin
// bytes (e.g. ESP-NOW chat text in a text_rx detail) and embedded newlines
// that would corrupt the lens text widget — the events.log file sink
// sanitizes the same way before writing. Newlines matter doubly here:
// textWrapAppend honours '\n' as a hard break rather than stripping it, so an
// unsanitized detail would blow apart the two-line-per-event layout.
static void g2SanitizeEventField(const char* in, char* out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; in && in[i] && o + 1 < cap; i++) {
    unsigned char c = (unsigned char)in[i];
    out[o++] = (c >= 0x20 && c < 0x7F) ? (char)c : ' ';
  }
  if (cap) out[o] = '\0';
}

// "45s" / "12m" / "7h", capped so the column never widens past 4.
static void g2SysEvFormatAge(uint32_t ageS, char* out, size_t cap) {
  if (ageS < 60)          snprintf(out, cap, "%lus", (unsigned long)ageS);
  else if (ageS < 3600)   snprintf(out, cap, "%lum", (unsigned long)(ageS / 60));
  else if (ageS < 360000) snprintf(out, cap, "%luh", (unsigned long)(ageS / 3600));
  else                    snprintf(out, cap, "99h+");
}

// Snapshot the ring into gSysEvBody, newest first, two lines per event:
//
//   3m   peer_online
//        example-peer - 02:00:00:00:00:01
//   1m   login_ok
//        example-user - web  (by web:example-user)
//
// Line 1 is the age column + the kind name; line 2 is indented under it and
// carries subject, detail and attribution, soft-wrapping at the lens column
// width so nothing is clipped. The walk stops at the first seq that has fallen
// off the 48-entry ring. Leaves the pager on page 0 (the newest events).
//
// Paging is done HERE rather than by the shared textSplitPages(): that splitter
// breaks at any '\n' once the byte budget is hit, which lands mid-event and
// orphans a detail line at the top of the next page with no kind above it. This
// page is a list of records, so a page may only ever break BETWEEN events. The
// wrap engine is still the shared one — only the split policy is local.
static void buildSysEventsPages() {
  const uint32_t latest = systemEventLatestSeq();
  const uint32_t nowMs  = millis();
  size_t pos = 0;
  gSysEvBody[0] = '\0';
  bool wrapTrunc = false;
  int  shown = 0;

  // Page accounting: `page` is the index of the page being filled, which starts
  // at gSysEvBody[pageStart]. gSysEvPageOff[page] is already written for it.
  int    page      = 0;
  size_t pageStart = 0;
  gSysEvPageOff[0] = 0;

  SystemEvent e;
  for (uint32_t seq = latest; seq > 0 && shown < SYSEVT_RING_SIZE; seq--) {
    if (!systemEventGetBySeq(seq, &e)) break;   // fell off the ring

    const size_t blockStart   = pos;
    const bool   trunkBefore  = wrapTrunc;
    char age[8];
    g2SysEvFormatAge((nowMs - e.tsMs) / 1000, age, sizeof(age));

    // Line 1 — age column then the kind. Kind names top out around 25 chars,
    // so at 48 columns this never actually wraps; contIndent keeps it aligned
    // with line 2 if a future kind name ever gets long enough.
    char l1[64];
    snprintf(l1, sizeof(l1), "%-4s %s", age, systemEventKindName(e.kind));
    size_t before = pos;
    textWrapAppend(gSysEvBody, sizeof(gSysEvBody), pos, l1,
                   G2_TEXT_DEFAULT_COLS, /*contIndent=*/5, /*stripCtrl=*/true,
                   &wrapTrunc);
    if (pos > before && pos + 1 < sizeof(gSysEvBody)) gSysEvBody[pos++] = '\n';

    char subj[SYSEVT_SUBJECT_LEN], det[SYSEVT_DETAIL_LEN], who[SYSEVT_WHO_LEN];
    g2SanitizeEventField(e.subject, subj, sizeof(subj));
    g2SanitizeEventField(e.detail,  det,  sizeof(det));
    g2SanitizeEventField(e.who,     who,  sizeof(who));

    // Attribution only when it says something. Firmware-posted events are
    // source=system with an empty who, and "(by system)" on four rows out of
    // five is noise that costs a wrapped line each time.
    char attr[48];
    attr[0] = '\0';
    if (who[0]) {
      snprintf(attr, sizeof(attr), "  (by %s:%s)",
               systemEventSourceName(e.source), who);
    } else if (e.source != NOTIF_SOURCE_SYSTEM &&
               e.source != NOTIF_SOURCE_UNKNOWN) {
      snprintf(attr, sizeof(attr), "  (by %s)", systemEventSourceName(e.source));
    }

    // Line 2 — skipped entirely for events that carry no payload at all
    // (espnow_on, power_save_exit, …), which keeps those to one tidy row.
    if (subj[0] || det[0] || attr[0]) {
      char l2[224];
      snprintf(l2, sizeof(l2), "     %s%s%s%s", subj,
               (subj[0] && det[0]) ? " - " : "", det, attr);
      before = pos;
      textWrapAppend(gSysEvBody, sizeof(gSysEvBody), pos, l2,
                     G2_TEXT_DEFAULT_COLS, /*contIndent=*/7, /*stripCtrl=*/true,
                     &wrapTrunc);
      if (pos > before && pos + 1 < sizeof(gSysEvBody)) gSysEvBody[pos++] = '\n';
    }

    // Body full. textWrapAppend only reports truncation when the DESTINATION
    // ran out (l1/l2 are sized so they never truncate themselves), so a flag
    // that flipped during this block means the event landed half-written —
    // roll it back rather than leave a headless fragment on the last page.
    // wrapTrunc stays set, so the render still stamps "...[truncated]".
    if (pos == blockStart) break;
    if (wrapTrunc && !trunkBefore) { pos = blockStart; break; }

    // Did this event push the page past budget? If it wasn't the page's first
    // event, hand it to a fresh page instead of letting it straddle the break.
    // (A single event bigger than the budget stays put and overshoots — one
    // event per page is as small as this can get. gSysEvPageBuf is sized for it.)
    if (blockStart > pageStart && (pos - pageStart) > SYSEV_TEXT_PAGE_BUDGET) {
      if (page + 1 >= SYSEV_TEXT_MAX_PAGES) {
        pos = blockStart;           // no room for another page — drop this event
        wrapTrunc = true;
        break;
      }
      gSysEvPageOff[++page] = (uint16_t)blockStart;
      pageStart = blockStart;
    }
    shown++;
  }
  gSysEvBody[(pos < sizeof(gSysEvBody)) ? pos : (sizeof(gSysEvBody) - 1)] = '\0';

  gSysEvPager.curPage   = 0;
  gSysEvPager.truncated = wrapTrunc;
  if (pos > 0) {
    // A rollback right after a page break leaves the newest page with nothing
    // in it (the event that would have opened it didn't fit). Drop it — an
    // empty slice renders as "(no events yet)", which would read as a bug.
    if (page > 0 && gSysEvPageOff[page] >= (uint16_t)pos) page--;
    gSysEvPager.pageCount   = page + 1;
    gSysEvPageOff[page + 1] = (uint16_t)pos;   // end sentinel
  } else {
    // Nothing posted yet — show a single "(no events yet)" page rather than a
    // zero-page pager the renderer would have to special-case.
    gSysEvPager.pageCount = 1;
    gSysEvPageOff[0]      = 0;
    gSysEvPageOff[1]      = 0;
  }

  // Title carries the lifetime total, so "Events (312)" with 48 on-lens rows
  // makes it obvious the ring is a window, not the whole history.
  snprintf(gSysEvTitle, sizeof(gSysEvTitle), "Events (%lu)",
           (unsigned long)latest);
  DEBUG_G2F("[G2] sysevents: snapshot %d event(s) body=%uB pages=%d trunc=%d",
            shown, (unsigned)pos, gSysEvPager.pageCount,
            gSysEvPager.truncated ? 1 : 0);
}

// Render the pager's current page. Chrome is fixed; only gSysEvTitle's
// contents change per snapshot. Exit is g2ShowSystemMenu so a double-tap
// lands back on the System launcher — the same destination the old "<- System"
// back row had.
static bool renderSysEventsPage() {
  static const G2TextPageChrome chrome = {
      gSysEvTitle,                // title (mutated in place before render)
      "tap/scroll=page 2x=exit",  // multi-page hint
      "tap=back",                 // single-page hint
      "--------------------",     // separator rule
      "(no events yet)" };        // empty-slice text
  return g2TextPagerRender(gSysEvPager, gSysEvPageBuf, sizeof(gSysEvPageBuf),
                           chrome, G2_GEOM_LARGE, g2ShowSystemMenu,
                           g2SysEventsNav);
}

// tap/scroll page navigation. NEXT (tap / scroll-down) walks toward older
// events; PREV (scroll-up) walks back toward newer; both wrap. The snapshot is
// stable for the viewer's lifetime; reopening refreshes it. This keeps the
// navigation mutation transactional with the exact page-swap submission.
static void g2SysEventsNav(G2TapKind kind) {
  const int oldPage = gSysEvPager.curPage;
  textNavPage(gSysEvPager, kind != G2_TAP_PAGE_PREV);
  if (!renderSysEventsPage()) {
    // Overlay/terminal admission can revoke the exact callback after it was
    // dequeued. Keep logical pagination aligned with the still-visible page.
    gSysEvPager.curPage = oldPage;
  }
}

// Stub buildText for the registry's required-field check (hidden pages have no
// CLI text view; the full event list is already the `events` CLI command).
static void g2BuildSysEventsInfo(char* out, size_t cap) {
  if (!out || cap == 0) return;
  snprintf(out, cap, "System Events viewer (paged event ring on lens; 'events' for the CLI list)");
}

bool g2ShowSysEventsPage() {
  buildSysEventsPages();
  if (!renderSysEventsPage()) {
    DEBUG_G2F("[G2] sysevents: text-page render failed");
    return false;
  }
  g2SetHijackPage(G2_HIJACK_PAGE_SYSEVENTS);
  BROADCAST_PRINTF("[G2] Hijack: System Events viewer opened (%d page%s)",
                   gSysEvPager.pageCount,
                   gSysEvPager.pageCount == 1 ? "" : "s");
  return true;
}

// void trampoline for the registry's showMenu slot (g2ShowSysEventsPage
// reports success to its own caller).
static void g2ShowSysEventsMenu() { g2ShowSysEventsPage(); }

const G2PageModule kSysEventsPage = {
  // Hidden — hijackLabel=nullptr keeps it out of the main menu; the only
  // entry is g2ShowSysEventsPage() from the System launcher.
  "sysevents", nullptr,
  "System Events viewer (paged event ring on lens; 'events' for the CLI list)",
  /*buildText=*/  g2BuildSysEventsInfo,
  /*showMenu=*/   g2ShowSysEventsMenu,
  // handleTap MUST stay null. This is a TEXT view with no list rows, and the
  // SysEvent dispatcher defers single taps to the ListEvent path whenever the
  // active page has a handleTap — with one registered, a single-page snapshot
  // would swallow the tap instead of exiting.
  /*handleTap=*/  nullptr,
  G2_HIJACK_PAGE_SYSEVENTS,
  /*liveIntervalMs=*/ 0,          // snapshot page — no live worker
  /*backLabel=*/      nullptr,    // no list chrome to label
  /*prefersTextWidget=*/ false,   // own showMenu — flag is irrelevant
  /*liveRender=*/     nullptr,
};

#endif // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
