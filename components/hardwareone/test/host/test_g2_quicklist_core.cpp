// Host tests for G2_QuickListCore.h: the "Wear Detection", "Silent Mode",
// "Auto Brightness", "Head-Up Display" and "Brightness" control rows in the
// glasses' own QuickList (sid 0x0C).
//
// Literal 2.3.2.14 captures (CAPTURE.md): the tick report (kTickReal); the
// Even app's FULL_UPDATE (its first item, kAppFullItemA) and FULL_UPDATE ACKs
// (kAppFullAck*); the HardwareOne FULL_UPDATEs sent with g2probe 0c 2 on
// 2026-11-21 (kHwFull255, ACKed by kHwFullAck255 and shown on the lens in index
// order; kHwFull1). Protocol records kept from the earlier per-row push: the
// Even app's two cmd 1 ITEMs with their ACKs (kAppItem*, kAppAck*) and the
// HardwareOne ITEM replay (kPushOnCaptured250); the other kPush* vectors were
// encoded from that captured ITEM layout. Their item bodies are the items a
// FULL_UPDATE carries (testGoldens), but HardwareOne no longer sends cmd 1.
// The settings frames themselves (silent, auto-brightness, brightness-level
// and head-up writes and ACKs, BASIC f14/f18/f2/f7, cmd 3 push) are tested in
// g2_settings_protocol against the literal captures.
#include "../../G2_QuickListCore.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace hw1_g2_quicklist;

static int gFailures = 0;
#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                   #cond);                                                  \
      gFailures++;                                                          \
    }                                                                       \
  } while (0)

using Bytes = std::vector<uint8_t>;

static Bytes hex(const char* h) {
  Bytes out;
  for (size_t i = 0; h[i] && h[i + 1]; i += 2) {
    unsigned v = 0;
    std::sscanf(h + i, "%2x", &v);
    out.push_back((uint8_t)v);
  }
  return out;
}

// ---- cmd 1 ITEM protocol records (the item inside is the FULL_UPDATE item) ----
static const char* kPushOn =
    "080110011A23088180C4C1041000180020002A135765617220446574656374696F6E3A204F6E003802";
static const char* kPushOff =
    "080110021A24088180C4C1041000180020002A145765617220446574656374696F6E3A204F6666003802";
static const char* kPushUnknown200 =
    "080110C8011A22088180C4C1041000180020002A125765617220446574656374696F6E3A203F003802";
static const char* kAck1 = "080110011A06088180C4C104";
static const char* kSilentPushOn1 =
    "080110011A20088280C4C1041001180020002A1053696C656E74204D6F64653A204F6E003802";
static const char* kSilentPushOff2 =
    "080110021A21088280C4C1041001180020002A1153696C656E74204D6F64653A204F6666003802";
static const char* kSilentPushUnknown3 =
    "080110031A1F088280C4C1041001180020002A0F53696C656E74204D6F64653A203F003802";
static const char* kAutoBrightPushOn1 =
    "080110011A24088380C4C1041002180020002A144175746F204272696768746E6573733A204F6E003802";
static const char* kAutoBrightPushOff2 =
    "080110021A25088380C4C1041002180020002A154175746F204272696768746E6573733A204F6666003802";
static const char* kAutoBrightPushUnknown3 =
    "080110031A23088380C4C1041002180020002A134175746F204272696768746E6573733A203F003802";
static const char* kHeadUpPushOn1 =
    "080110011A24088480C4C1041003180020002A14486561642D557020446973706C61793A204F6E003802";
static const char* kHeadUpPushOff2 =
    "080110021A25088480C4C1041003180020002A15486561642D557020446973706C61793A204F6666003802";
static const char* kHeadUpPushUnknown3 =
    "080110031A23088480C4C1041003180020002A13486561642D557020446973706C61793A203F003802";
static const char* kBrightnessPushAuto1 =
    "080110011A21088580C4C1041004180020002A114272696768746E6573733A204175746F003802";
static const char* kBrightnessPush25_2 =
    "080110021A20088580C4C1041004180020002A104272696768746E6573733A20323525003802";
static const char* kBrightnessPush100_3 =
    "080110031A21088580C4C1041004180020002A114272696768746E6573733A2031303025003802";
static const char* kBrightnessPush60_4 =
    "080110041A20088580C4C1041004180020002A104272696768746E6573733A20363025003802";
static const char* kBrightnessPushUnknown5 =
    "080110051A1E088580C4C1041004180020002A0E4272696768746E6573733A203F003802";
static const char* kTickReal = "080210042214080310011A0E088180C4C104180120A9B084D806";
// HW1 replay 2026-11-21 (A7:73): g2probe 0c 1 <body>, the probe added 08 01 10 FA 01.
static const char* kPushOnCaptured250 =
    "080110FA011A23088180C4C1041000180020002A135765617220446574656374696F6E3A204F6E003802";
// Even app, 2026-11-20: rename "line a" -> "line a2" (magic 68) and add "lined" (magic 146).
static const char* kAppItemRename68 = "080110441A1808E692D3ED021000180020002A086C696E65206132003802";
static const char* kAppAck68 = "080110441A0608E692D3ED02";
static const char* kAppItemAdd146 = "08011092011A1608FCC392F1021000180020002A066C696E6564003802";
static const char* kAppAck146 = "08011092011A0608FCC392F102";

// ---- cmd 2 FULL_UPDATE captures ----------------------------------------------
// Even app, connect 2026-11-20: 08 02 10 0c 22 6f 08 01 10 04, then its first
// item, 1a 17 { uid 766822758, index 0, "line a" } (three more items follow).
static const char* kAppFullHead12 = "0802100C226F08011004";
static const char* kAppFullItemA = "08E692D3ED021000180020002A076C696E65206100" "3802";
static const char* kAppFullAck12 = "0802100C220408011004";
static const char* kAppFullAck70 = "08021046220408011002";
static const char* kAppFullAck71 = "08021047220408011001";
// HW1, 2026-11-21 (owner's glasses 2.3.2.14): g2probe 0c 2 <body>, the probe
// added 08 02 10 <magic>. Magic 255 (a two-byte varint), 195 bytes, one
// fragment; ACKed with kHwFullAck255 (pbLen 11) and the lens showed the rows in
// index order. Titles: Wear Detection: On / Silent Mode: Off / Auto Brightness:
// On / Head-Up Display: On / Brightness: Auto.
static const char* kHwFull255 =
    "080210FF01"
    "22BB01080110051A23088180C4C1041000180020002A135765617220446574656374696F6E3A204F6E003802"
    "1A21088280C4C1041001180020002A1153696C656E74204D6F64653A204F6666003802"
    "1A24088380C4C1041002180020002A144175746F204272696768746E6573733A204F6E003802"
    "1A24088480C4C1041003180020002A14486561642D557020446973706C61793A204F6E003802"
    "1A21088580C4C1041004180020002A114272696768746E6573733A204175746F003802";
static const char* kHwFullAck255 = "080210FF01220408011005";
// Magic 1, Auto Brightness: Off and Brightness: 35% (194 bytes): the push
// after which the wearer ticked Head-Up and the write ran.
static const char* kHwFull1 =
    "08021001"
    "22BB01080110051A23088180C4C1041000180020002A135765617220446574656374696F6E3A204F6E003802"
    "1A21088280C4C1041001180020002A1153696C656E74204D6F64653A204F6666003802"
    "1A25088380C4C1041002180020002A154175746F204272696768746E6573733A204F6666003802"
    "1A24088480C4C1041003180020002A14486561642D557020446973706C61793A204F6E003802"
    "1A20088580C4C1041004180020002A104272696768746E6573733A20333525003802";

// ---- raw protobuf builders for mutations ----------------------------------
static void putVarint(Bytes& b, uint64_t v) {
  do {
    uint8_t c = (uint8_t)(v & 0x7F);
    v >>= 7;
    if (v) c |= 0x80;
    b.push_back(c);
  } while (v);
}
static void putField(Bytes& b, uint32_t field, uint64_t v) {
  putVarint(b, (uint64_t)field << 3);
  putVarint(b, v);
}
static void putBytes(Bytes& b, uint32_t field, const Bytes& v) {
  putVarint(b, ((uint64_t)field << 3) | 2);
  putVarint(b, v.size());
  b.insert(b.end(), v.begin(), v.end());
}
static Bytes str(const char* s, size_t n) { return Bytes(s, s + n); }
static Bytes cstr(const char* s) { return Bytes(s, s + std::strlen(s) + 1); }   // with its NUL

struct ItemSpec {
  uint32_t uid = kWearUid, index = 0, completed = 0, ts = 0, tsType = 2;
  Bytes title = str("Wear Detection: On\0", 19);
  Bytes extra;
};
static Bytes itemBytes(const ItemSpec& r) {
  Bytes inner;
  putField(inner, 1, r.uid);
  putField(inner, 2, r.index);
  putField(inner, 3, r.completed);
  putField(inner, 4, r.ts);
  putBytes(inner, 5, r.title);
  putField(inner, 7, r.tsType);
  inner.insert(inner.end(), r.extra.begin(), r.extra.end());
  return inner;
}
// The five rows with these titles, index 0..4.
struct FullSpec {
  uint32_t cmd = 2, magic = 1, dataType = 1, total = 5;
  ItemSpec items[kRowCount];
  size_t count = kRowCount;
  Bytes bodyExtra, outerExtra;
  FullSpec() {
    const char* t[kRowCount] = {"Wear Detection: On", "Silent Mode: Off", "Auto Brightness: On", "Head-Up Display: On",
                                "Brightness: Auto"};
    for (uint8_t i = 0; i < kRowCount; ++i) {
      items[i].uid = rowUid((Control)i);
      items[i].index = i;
      items[i].title = cstr(t[i]);
    }
  }
};
static Bytes full(const FullSpec& f) {
  Bytes body;
  putField(body, 1, f.dataType);
  putField(body, 2, f.total);
  for (size_t i = 0; i < f.count; ++i) putBytes(body, 3, itemBytes(f.items[i]));
  body.insert(body.end(), f.bodyExtra.begin(), f.bodyExtra.end());
  Bytes out;
  putField(out, 1, f.cmd);
  putField(out, 2, f.magic);
  putBytes(out, 4, body);
  out.insert(out.end(), f.outerExtra.begin(), f.outerExtra.end());
  return out;
}

// The item body inside a cmd 1 ITEM (08 01 10 <magic> 1a <len> <item>).
static Bytes itemOfItemMsg(const Bytes& m) {
  size_t i = 3;
  while (i < m.size() && (m[i] & 0x80)) ++i;   // the magic varint
  i += 1;
  if (i + 2 > m.size() || m[i] != 0x1A) return Bytes();
  return Bytes(m.begin() + (long)i + 2, m.end());
}
static Bytes item(uint32_t uid, uint32_t index, const char* title) {
  uint8_t buf[kItemBodyMax];
  const size_t n = encodeListItem(ListItem{uid, index, title}, buf, sizeof buf);
  return Bytes(buf, buf + n);
}
static Bytes rows(uint8_t magic, const char* t0, const char* t1, const char* t2, const char* t3, const char* t4,
                  size_t cap = kPbMax) {
  const char* t[kRowCount] = {t0, t1, t2, t3, t4};
  uint8_t buf[kPbMax];
  const size_t n = encodeRows(magic, t, buf, cap);
  return Bytes(buf, buf + n);
}
static Bytes rowsOf(uint8_t magic, const Policy& p) {
  Title t[kRowCount];
  for (uint8_t i = 0; i < kRowCount; ++i) t[i] = rowTitle((Control)i, p.row[i].next, p.row[i].nextLevel);
  return rows(magic, t[0].text, t[1].text, t[2].text, t[3].text, t[4].text);
}

// The first pending row in list order (a test convenience; the owner sends
// all five rows whenever any is pending).
static bool nextPendingRow(const Policy& p, Control* out) {
  for (uint8_t i = 0; i < kRowCount; ++i)
    if (p.row[i].pending) { *out = (Control)i; return true; }
  return false;
}

// The titles of a FULL_UPDATE, in item order (empty on a parse error).
static std::vector<std::string> decodeTitles(const Bytes& b) {
  std::vector<std::string> out;
  detail::Reader r{b.data(), b.size(), 0};
  while (!r.atEnd()) {
    uint32_t f = 0;
    uint8_t wt = 0;
    if (!r.tag(&f, &wt)) return {};
    if (f != 4 || wt != 2) { if (!r.skip(wt)) return {}; continue; }
    const uint8_t* body = nullptr;
    size_t n = 0;
    if (!r.lenDelim(&body, &n)) return {};
    detail::Reader in{body, n, 0};
    while (!in.atEnd()) {
      if (!in.tag(&f, &wt)) return {};
      if (f != 3 || wt != 2) { if (!in.skip(wt)) return {}; continue; }
      const uint8_t* it = nullptr;
      size_t itn = 0;
      if (!in.lenDelim(&it, &itn)) return {};
      detail::Reader ir{it, itn, 0};
      while (!ir.atEnd()) {
        if (!ir.tag(&f, &wt)) return {};
        if (f == 5 && wt == 2) {
          const uint8_t* t = nullptr;
          size_t tn = 0;
          if (!ir.lenDelim(&t, &tn) || !tn) return {};
          out.emplace_back((const char*)t, tn - 1);
        } else if (!ir.skip(wt)) {
          return {};
        }
      }
    }
  }
  return out;
}

// ---- 1. goldens ------------------------------------------------------------
static void testGoldens() {
  CHECK(kWearUid == 0x48310001u && kSilentUid == 0x48310002u && kAutoBrightUid == 0x48310003u &&
        kHeadUpUid == 0x48310004u && kBrightnessUid == 0x48310005u);
  for (uint8_t i = 0; i < kRowCount; ++i) CHECK(rowIndex((Control)i) == i);
  // The captured app item, byte for byte.
  CHECK(item(766822758u, 0, "line a") == hex(kAppFullItemA));
  // ... inside a FULL_UPDATE: 08 02 10 0c 22 <len> 08 01 10 <count> 1a 17 <item>.
  {
    const ListItem one{766822758u, 0, "line a"};
    uint8_t buf[kPbMax];
    const size_t n = encodeFullUpdate(12, &one, 1, buf, sizeof buf);
    Bytes want = hex("0802100C221D080110011A17");   // f4 body 2 + 2 + 2 + 23 = 29
    const Bytes a = hex(kAppFullItemA);
    want.insert(want.end(), a.begin(), a.end());
    CHECK(Bytes(buf, buf + n) == want);
    // The app's header with its four items differs only in the lengths and count.
    const Bytes head = hex(kAppFullHead12);
    CHECK(head[0] == buf[0] && head[1] == buf[1] && head[2] == buf[2] && head[3] == buf[3] && head[4] == buf[4] &&
          head[6] == buf[6] && head[7] == buf[7] && head[8] == buf[8]);
    // ... and it is not ours: refused by the wire whitelist.
    CHECK(!validateFullUpdatePb(buf, n));
  }
  // The rename and add ITEMs carry the same item layout.
  CHECK(itemOfItemMsg(hex(kAppItemRename68)) == item(766822758u, 0, "line a2"));
  CHECK(itemOfItemMsg(hex(kAppItemAdd146)) == item(774152700u, 0, "lined"));
  // So do the earlier per-row ITEM vectors: each row's item is unchanged.
  struct G { Control c; Shown s; uint8_t level; const char* hex; };
  const G items[] = {
      {Control::Wear, Shown::On, 0, kPushOn},
      {Control::Wear, Shown::Off, 0, kPushOff},
      {Control::Wear, Shown::Unknown, 0, kPushUnknown200},
      {Control::Wear, Shown::On, 0, kPushOnCaptured250},
      {Control::Silent, Shown::On, 0, kSilentPushOn1},
      {Control::Silent, Shown::Off, 0, kSilentPushOff2},
      {Control::Silent, Shown::Unknown, 0, kSilentPushUnknown3},
      {Control::AutoBright, Shown::On, 0, kAutoBrightPushOn1},
      {Control::AutoBright, Shown::Off, 0, kAutoBrightPushOff2},
      {Control::AutoBright, Shown::Unknown, 0, kAutoBrightPushUnknown3},
      {Control::HeadUp, Shown::On, 0, kHeadUpPushOn1},
      {Control::HeadUp, Shown::Off, 0, kHeadUpPushOff2},
      {Control::HeadUp, Shown::Unknown, 0, kHeadUpPushUnknown3},
      {Control::Brightness, Shown::On, 0, kBrightnessPushAuto1},
      {Control::Brightness, Shown::Off, 25, kBrightnessPush25_2},
      {Control::Brightness, Shown::Off, 100, kBrightnessPush100_3},
      {Control::Brightness, Shown::Off, 60, kBrightnessPush60_4},
      {Control::Brightness, Shown::Unknown, 0, kBrightnessPushUnknown5},
  };
  for (const G& g : items) {
    const Bytes want = itemOfItemMsg(hex(g.hex));
    CHECK(!want.empty());
    CHECK(item(rowUid(g.c), rowIndex(g.c), rowTitle(g.c, g.s, g.level).text) == want);
  }
  // The HW1 FULL_UPDATEs sent on the glasses, byte for byte, through the whitelist.
  {
    const Bytes want = hex(kHwFull255);
    CHECK(want.size() == 195);
    const Bytes got = rows(255, "Wear Detection: On", "Silent Mode: Off", "Auto Brightness: On",
                           "Head-Up Display: On", "Brightness: Auto");
    CHECK(got == want);
    uint8_t magic = 0;
    CHECK(validateFullUpdatePb(want.data(), want.size(), &magic) && magic == 255);
    CHECK(full([] { FullSpec f; f.magic = 255; return f; }()) == want);   // the raw builder agrees
    // One fragment: under the 232-byte chunk.
    CHECK(want.size() < 232);
  }
  {
    const Bytes want = hex(kHwFull1);
    CHECK(want.size() == 194);
    Policy p{};
    const SwitchState st[kRowCount] = {{true, true, 0}, {true, false, 0}, {true, false, 0}, {true, true, 0},
                                       {true, false, 35}};
    for (uint8_t i = 0; i < kRowCount; ++i) setNext(p.row[i], (Control)i, st[i]);
    CHECK(rowsOf(1, p) == want);   // "Brightness: 35%": a level the glasses hold
    uint8_t magic = 0;
    CHECK(validateFullUpdatePb(want.data(), want.size(), &magic) && magic == 1);
  }
  // Every row "?": the shortest titles; still one f4 length of two bytes.
  {
    const Bytes q = rows(7, "Wear Detection: ?", "Silent Mode: ?", "Auto Brightness: ?", "Head-Up Display: ?",
                         "Brightness: ?");
    FullSpec f;
    f.magic = 7;
    const char* t[kRowCount] = {"Wear Detection: ?", "Silent Mode: ?", "Auto Brightness: ?", "Head-Up Display: ?",
                                "Brightness: ?"};
    for (uint8_t i = 0; i < kRowCount; ++i) f.items[i].title = cstr(t[i]);
    CHECK(q == full(f));
    CHECK(q.size() == 186 && q[4] == 0x22 && q[5] == 0xB3 && q[6] == 0x01);   // f4 length 179 = B3 01
    CHECK(validateFullUpdatePb(q.data(), q.size()));
  }
}

// ---- 2. varints >= 128 ---------------------------------------------------------
static void testVarints() {
  CHECK(detail::varintLen(0) == 1 && detail::varintLen(127) == 1 && detail::varintLen(128) == 2 &&
        detail::varintLen(16383) == 2 && detail::varintLen(16384) == 3 && detail::varintLen(kWearUid) == 5);
  // Magic 127 is one byte, 128 two (80 01), 255 two (FF 01).
  const char* t[kRowCount] = {"Wear Detection: On", "Silent Mode: Off", "Auto Brightness: On", "Head-Up Display: On",
                              "Brightness: Auto"};
  uint8_t buf[kPbMax];
  size_t n = encodeRows(127, t, buf, sizeof buf);
  CHECK(n == 194 && buf[2] == 0x10 && buf[3] == 0x7F && buf[4] == 0x22 && buf[5] == 0xBB && buf[6] == 0x01);
  n = encodeRows(128, t, buf, sizeof buf);
  CHECK(n == 195 && buf[3] == 0x80 && buf[4] == 0x01 && buf[5] == 0x22 && buf[6] == 0xBB && buf[7] == 0x01);
  CHECK(validateFullUpdatePb(buf, n));
  // An f4 body of exactly 127 and 128 bytes: its length goes from 7F to 80 01.
  // (Five rows are at least 179 bytes, so the generic encoder with three items.)
  // Each item adds 18 bytes plus its title with NUL; data_type + total_count 4.
  {
    const std::string x(31, 'x'), y(31, 'y'), z4(4, 'z'), z5(5, 'z');
    ListItem it[3] = {{kWearUid, 0, x.c_str()}, {kSilentUid, 1, y.c_str()}, {kAutoBrightUid, 2, z4.c_str()}};
    n = encodeFullUpdate(1, it, 3, buf, sizeof buf);   // 4 + 3 * 18 + 32 + 32 + 5 = 127
    CHECK(n == 4 + 2 + 127 && buf[4] == 0x22 && buf[5] == 0x7F && buf[6] == 0x08);
    it[2].title = z5.c_str();                          // 128
    n = encodeFullUpdate(1, it, 3, buf, sizeof buf);
    CHECK(n == 4 + 3 + 128 && buf[4] == 0x22 && buf[5] == 0x80 && buf[6] == 0x01 && buf[7] == 0x08);
  }
  // The longest possible update fits kPbMax exactly.
  {
    const std::string l(31, 'L');
    const char* lt[kRowCount] = {l.c_str(), l.c_str(), l.c_str(), l.c_str(), l.c_str()};
    n = encodeRows(255, lt, buf, sizeof buf);
    CHECK(n == kPbMax && validateFullUpdatePb(buf, n));
    CHECK(encodeRows(255, lt, buf, kPbMax - 1) == 0);
  }
}

// ---- 3. encoder rejects ------------------------------------------------------
static void testEncoderRejects() {
  uint8_t buf[kPbMax];
  const char* ok[kRowCount] = {"Wear Detection: On", "Silent Mode: Off", "Auto Brightness: On", "Head-Up Display: On",
                               "Brightness: Auto"};
  CHECK(encodeRows(0, ok, buf, sizeof buf) == 0);           // magic 0
  CHECK(encodeRows(1, nullptr, buf, sizeof buf) == 0);
  CHECK(encodeRows(1, ok, nullptr, sizeof buf) == 0);
  CHECK(encodeRows(1, ok, buf, 193) == 0 && encodeRows(1, ok, buf, 194) == 194);     // cap one short / exact
  CHECK(encodeRows(255, ok, buf, 194) == 0 && encodeRows(255, ok, buf, 195) == 195);
  const char* bad[] = {nullptr, "", "Wear\x01" "Detection", "Wear\x7F"};
  for (const char* b : bad) {
    const char* t[kRowCount] = {ok[0], ok[1], b, ok[3], ok[4]};
    CHECK(encodeRows(1, t, buf, sizeof buf) == 0);
  }
  const std::string t31(31, 'A'), t32(32, 'A');   // +NUL = 32 / 33 bytes
  {
    const char* t[kRowCount] = {ok[0], ok[1], ok[2], ok[3], t31.c_str()};
    CHECK(encodeRows(1, t, buf, sizeof buf) > 0);
    t[4] = t32.c_str();
    CHECK(encodeRows(1, t, buf, sizeof buf) == 0);
  }
  const ListItem one{kWearUid, 0, "Wear Detection: On"};
  CHECK(encodeFullUpdate(1, &one, 0, buf, sizeof buf) == 0);              // no items
  ListItem six[kRowCount + 1];
  for (auto& i : six) i = one;
  CHECK(encodeFullUpdate(1, six, kRowCount + 1, buf, sizeof buf) == 0);   // more than five
  CHECK(encodeListItem(ListItem{kWearUid, 0, "Wear Detection: On"}, buf, 34) == 0);
  CHECK(encodeListItem(ListItem{kWearUid, 0, "Wear Detection: On"}, buf, 35) == 35);
}

// ---- 4. validator rejects single mutations ----------------------------------
static void testValidatorRejects() {
  const FullSpec base;
  const Bytes good = full(base);
  auto rejects = [](const Bytes& b) { return !validateFullUpdatePb(b.data(), b.size()); };
  CHECK(!rejects(good));
  { FullSpec f = base; f.cmd = 1; CHECK(rejects(full(f))); }
  { FullSpec f = base; f.magic = 0; CHECK(rejects(full(f))); }
  { FullSpec f = base; f.magic = 256; CHECK(rejects(full(f))); }
  { FullSpec f = base; f.dataType = 3; CHECK(rejects(full(f))); }   // MODIFY
  { FullSpec f = base; f.dataType = 2; CHECK(rejects(full(f))); }   // ADD
  { FullSpec f = base; f.total = 4; CHECK(rejects(full(f))); }
  { FullSpec f = base; f.count = 4; f.total = 4; CHECK(rejects(full(f))); }   // four rows
  { FullSpec f = base; f.count = 4; CHECK(rejects(full(f))); }                 // four rows, total 5
  { FullSpec f = base; f.bodyExtra = {0x28, 0x00}; CHECK(rejects(full(f))); }
  { FullSpec f = base; f.outerExtra = {0x28, 0x00}; CHECK(rejects(full(f))); }
  for (uint8_t i = 0; i < kRowCount; ++i) {
    { FullSpec f = base; f.items[i].tsType = 1; CHECK(rejects(full(f))); }
    { FullSpec f = base; f.items[i].completed = 1; CHECK(rejects(full(f))); }
    { FullSpec f = base; f.items[i].ts = 1; CHECK(rejects(full(f))); }
    { FullSpec f = base; f.items[i].index = (i + 1) % kRowCount; CHECK(rejects(full(f))); }
    { FullSpec f = base; f.items[i].uid = kUidBase + 6; CHECK(rejects(full(f))); }
    { FullSpec f = base; f.items[i].uid = rowUid((Control)((i + 1) % kRowCount)); CHECK(rejects(full(f))); }
    { FullSpec f = base; f.items[i].extra = {0x40, 0x00}; CHECK(rejects(full(f))); }
    { FullSpec f = base; f.items[i].title = str("Row", 3); CHECK(rejects(full(f))); }           // no NUL
    { FullSpec f = base; f.items[i].title = str("Row\0\0", 5); CHECK(rejects(full(f))); }       // two NULs
    { FullSpec f = base; f.items[i].title = str("R\x01w\0", 4); CHECK(rejects(full(f))); }      // control byte
    { FullSpec f = base; f.items[i].title = str("\0", 1); CHECK(rejects(full(f))); }            // empty
    { FullSpec f = base; f.items[i].title = Bytes(33, 'A'); f.items[i].title.back() = 0; CHECK(rejects(full(f))); }
  }
  // Rows swapped (both with their own index): the order is fixed.
  { FullSpec f = base; std::swap(f.items[0], f.items[1]); CHECK(rejects(full(f))); }
  // Any printable title is the row's own business (e.g. "Brightness: 35%").
  { FullSpec f = base; f.items[4].title = cstr("Brightness: 35%"); CHECK(!rejects(full(f))); }
  // A non-minimal varint for the magic (01 written as 81 00).
  {
    Bytes b = good;
    b[3] = 0x81;
    b.insert(b.begin() + 4, 0x00);
    CHECK(rejects(b));
  }
  // The f4 length written non-minimally (3 bytes for a 2-byte value).
  {
    Bytes b = good;   // 08 02 10 01 22 BB 01 ...
    CHECK(b[4] == 0x22 && b[5] == 0xBB && b[6] == 0x01);
    b[6] = 0x81;
    b.insert(b.begin() + 7, 0x00);
    CHECK(rejects(b));
  }
  for (size_t n = 0; n < good.size(); ++n) CHECK(!validateFullUpdatePb(good.data(), n));
  CHECK(!validateFullUpdatePb(nullptr, 0));
  Bytes longer = good;
  longer.resize(kPbMax + 1, 0);
  CHECK(rejects(longer));
  // The cmd 1 ITEM is never sent any more.
  const Bytes itemMsg = hex(kPushOn);
  CHECK(rejects(itemMsg));
}

// ---- 5. ACK -------------------------------------------------------------------
static void testAck() {
  Ack ack{};
  const Bytes hw = hex(kHwFullAck255);
  CHECK(hw.size() == 11);   // pbLen 11: the magic is a two-byte varint
  CHECK(parseAck(hw.data(), hw.size(), ack) && ack.magic == 255 && ack.haveTotal && ack.totalCount == 5);
  const Bytes five = hex("08021001220408011005");   // 08 02 10 m 22 04 08 01 10 05
  CHECK(parseAck(five.data(), five.size(), ack) && ack.magic == 1 && ack.haveTotal && ack.totalCount == 5);
  const Bytes app12 = hex(kAppFullAck12), app70 = hex(kAppFullAck70), app71 = hex(kAppFullAck71);
  CHECK(parseAck(app12.data(), app12.size(), ack) && ack.magic == 12 && ack.totalCount == 4);
  CHECK(parseAck(app70.data(), app70.size(), ack) && ack.magic == 70 && ack.totalCount == 2);
  CHECK(parseAck(app71.data(), app71.size(), ack) && ack.magic == 71 && ack.totalCount == 1);
  for (size_t n = 0; n < five.size(); ++n) {   // cut anywhere but after the magic: refused
    Ack t{};
    CHECK(parseAck(five.data(), n, t) == (n == 4));
  }
  // Without the f4 body: cmd + magic is enough (no total_count).
  const Bytes bare = hex("08021001");
  CHECK(parseAck(bare.data(), bare.size(), ack) && ack.magic == 1 && !ack.haveTotal);
  // Unknown fields are tolerated.
  Bytes extra = five;
  extra.push_back(0x28);
  extra.push_back(0x07);
  CHECK(parseAck(extra.data(), extra.size(), ack) && ack.magic == 1);
  // A MODIFY body (the tick report's) is not an ACK of ours.
  const Bytes mod = hex("08021001220408031005");
  CHECK(!parseAck(mod.data(), mod.size(), ack));
  // cmd 1 ITEM ACKs (the earlier per-row push) are not ACKs of a FULL_UPDATE.
  const Bytes a1 = hex(kAck1), a68 = hex(kAppAck68), a146 = hex(kAppAck146);
  CHECK(!parseAck(a1.data(), a1.size(), ack) && !parseAck(a68.data(), a68.size(), ack) &&
        !parseAck(a146.data(), a146.size(), ack));
  // Magic 0, magic 256.
  CHECK(!parseAck(hex("08021000220408011005").data(), 10, ack));
  const Bytes m256 = hex("0802108002220408011005");
  CHECK(!parseAck(m256.data(), m256.size(), ack));
}

// ---- 6. tick report -------------------------------------------------------------
static void testTickReport() {
  const Bytes real = hex(kTickReal);
  TickReport rep;
  CHECK(parseTickReport(real.data(), real.size(), rep));
  CHECK(rep.glassesMagic == 4);
  CHECK(rep.dataType == 3);
  CHECK(rep.totalCount == 1);
  CHECK(rep.count == 1);
  CHECK(!rep.truncated);
  CHECK(rep.items[0].uid == 0x48310001u);
  CHECK(rep.items[0].completed);
  CHECK(rep.items[0].timestamp == 1795233833u);

  // Without `10 04`: no glasses counter.
  Bytes noMagic = real;
  noMagic.erase(noMagic.begin() + 2, noMagic.begin() + 4);
  CHECK(parseTickReport(noMagic.data(), noMagic.size(), rep));
  CHECK(rep.glassesMagic == 0);
  CHECK(rep.count == 1);

  // 11 items: the first 10 kept, truncated.
  Bytes data;
  putField(data, 1, 3);
  putField(data, 2, 11);
  for (uint32_t i = 0; i < 11; ++i) {
    Bytes item;
    putField(item, 1, kUidBase + 1 + i);
    putField(item, 3, 1);
    putField(item, 4, 1000 + i);
    putBytes(data, 3, item);
  }
  Bytes many;
  putField(many, 1, 2);
  putField(many, 2, 5);
  putBytes(many, 4, data);
  CHECK(parseTickReport(many.data(), many.size(), rep));
  CHECK(rep.count == 10);
  CHECK(rep.truncated);
  CHECK(rep.totalCount == 11);
  CHECK(rep.items[9].uid == kUidBase + 10);
  CHECK(rep.items[9].timestamp == 1009);

  // data_type 1 is not a tick.
  Bytes type1 = real;
  CHECK(type1[7] == 0x03);
  type1[7] = 0x01;
  CHECK(!parseTickReport(type1.data(), type1.size(), rep));

  // Wrong outer cmd.
  Bytes cmd1 = real;
  cmd1[1] = 0x01;
  CHECK(!parseTickReport(cmd1.data(), cmd1.size(), rep));

  for (size_t n = 0; n < real.size(); ++n) CHECK(!parseTickReport(real.data(), n, rep));

  // An item without `18 01` is not completed.
  const Bytes notDone = hex("080210042212080310011A0C088180C4C10420A9B084D806");
  CHECK(parseTickReport(notDone.data(), notDone.size(), rep));
  CHECK(rep.count == 1);
  CHECK(!rep.items[0].completed);
  CHECK(rep.items[0].uid == kWearUid);
}

// ---- 6. dedup ---------------------------------------------------------------------
static void testDedup() {
  Dedup d{};
  CHECK(!dedupSeen(d, 4, kWearUid));
  CHECK(dedupSeen(d, 4, kWearUid));
  CHECK(!dedupSeen(d, 5, kWearUid));
  CHECK(!dedupSeen(d, 4, kWearUid + 1));
  dedupClear(d);
  CHECK(!dedupSeen(d, 4, kWearUid));

  dedupClear(d);
  for (uint32_t i = 0; i < kDedupSlots; ++i) CHECK(!dedupSeen(d, 100 + i, kWearUid));
  for (uint32_t i = 0; i < kDedupSlots; ++i) CHECK(dedupSeen(d, 100 + i, kWearUid));
  CHECK(!dedupSeen(d, 200, kWearUid));      // the 17th evicts the first
  CHECK(!dedupSeen(d, 100, kWearUid));      // so the first is new again
  dedupClear(d);
  CHECK(d.used == 0);
  CHECK(!dedupSeen(d, 101, kWearUid));
}

// ---- 7. decideItem ------------------------------------------------------------------
static ReportItem item(uint32_t uid, bool completed) { return ReportItem{uid, completed, 0}; }

static void testDecide() {
  Policy p{};
  linkReset(p);
  const uint32_t kForeign = kUidBase + 6;
  // Foreign: not inserted into dedup.
  CHECK(decideItem(p, true, 1, item(kForeign, true), 0).action == TickAction::Foreign);
  CHECK(p.dedup.used == 0);
  CHECK(decideItem(p, true, 1, item(kWearUid, false), 0).action == TickAction::NotCompleted);
  CHECK(decideItem(p, false, 1, item(kWearUid, true), 0).action == TickAction::Disabled);
  CHECK(p.dedup.used == 0);
  // Shown Unknown -> Refresh; the same (counter, uid) again -> Duplicate.
  Decision d = decideItem(p, true, 1, item(kWearUid, true), 0);
  CHECK(d.action == TickAction::Refresh && d.control == Control::Wear);
  CHECK(decideItem(p, true, 1, item(kWearUid, true), 0).action == TickAction::Duplicate);
  // The silent row is ours too, with its own dedup key; unknown -> refresh.
  d = decideItem(p, true, 1, item(kSilentUid, true), 0);
  CHECK(d.action == TickAction::Refresh && d.control == Control::Silent);
  // A job in flight -> Busy, and that row is re-pushed unchanged afterwards.
  noteJobStarted(p, 1000);
  CHECK(p.jobInFlight && p.everJob && p.lastJobMs == 1000);
  d = decideItem(p, true, 2, item(kWearUid, true), 1100);
  CHECK(d.action == TickAction::Busy && d.control == Control::Wear);
  CHECK(rowOf(p, Control::Wear).pending && !rowOf(p, Control::Silent).pending);
  // The auto-brightness row: unknown here, but the job slot is shared -> Busy.
  d = decideItem(p, true, 2, item(kAutoBrightUid, true), 1100);
  CHECK(d.action == TickAction::Busy && d.control == Control::AutoBright);
  CHECK(rowOf(p, Control::AutoBright).pending && rowOf(p, Control::AutoBright).next == Shown::Unknown);
  p.jobInFlight = false;
  // Unknown auto-brightness row -> Refresh (a read, no write).
  linkReset(p);
  d = decideItem(p, true, 2, item(kAutoBrightUid, true), 0);
  CHECK(d.action == TickAction::Refresh && d.control == Control::AutoBright);

  // Apply: target is the opposite of what that row shows.
  linkReset(p);
  rowOf(p, Control::Wear).shown = Shown::On;
  rowOf(p, Control::Silent).shown = Shown::Off;
  d = decideItem(p, true, 3, item(kWearUid, true), 0);
  CHECK(d.action == TickAction::Apply && !d.targetOn && d.control == Control::Wear);
  d = decideItem(p, true, 4, item(kSilentUid, true), 0);
  CHECK(d.action == TickAction::Apply && d.targetOn && d.control == Control::Silent);
  rowOf(p, Control::Wear).shown = Shown::Off;
  rowOf(p, Control::Silent).shown = Shown::On;
  d = decideItem(p, true, 5, item(kWearUid, true), 0);
  CHECK(d.action == TickAction::Apply && d.targetOn);
  d = decideItem(p, true, 6, item(kSilentUid, true), 0);
  CHECK(d.action == TickAction::Apply && !d.targetOn);
  // A silent row whose title is "?" refreshes even when wear is known.
  rowOf(p, Control::Silent).shown = Shown::Unknown;
  CHECK(decideItem(p, true, 7, item(kSilentUid, true), 0).action == TickAction::Refresh);
  // Auto brightness: the target is the opposite of what its row shows.
  rowOf(p, Control::AutoBright).shown = Shown::On;
  d = decideItem(p, true, 8, item(kAutoBrightUid, true), 0);
  CHECK(d.action == TickAction::Apply && !d.targetOn && d.control == Control::AutoBright);
  rowOf(p, Control::AutoBright).shown = Shown::Off;
  d = decideItem(p, true, 9, item(kAutoBrightUid, true), 0);
  CHECK(d.action == TickAction::Apply && d.targetOn && d.control == Control::AutoBright);
  rowOf(p, Control::AutoBright).shown = Shown::Unknown;
  CHECK(decideItem(p, true, 10, item(kAutoBrightUid, true), 0).action == TickAction::Refresh);
  // Head-up: the opposite of what its row shows; "?" refreshes.
  rowOf(p, Control::HeadUp).shown = Shown::On;
  d = decideItem(p, true, 30, item(kHeadUpUid, true), 0);
  CHECK(d.action == TickAction::Apply && !d.targetOn && d.control == Control::HeadUp);
  rowOf(p, Control::HeadUp).shown = Shown::Off;
  d = decideItem(p, true, 31, item(kHeadUpUid, true), 0);
  CHECK(d.action == TickAction::Apply && d.targetOn && d.control == Control::HeadUp);
  rowOf(p, Control::HeadUp).shown = Shown::Unknown;
  CHECK(decideItem(p, true, 32, item(kHeadUpUid, true), 0).action == TickAction::Refresh);
  // Brightness: Auto or a level -> Apply (the job steps from its own pre-read;
  // targetOn is unused and false); "?" refreshes.
  rowOf(p, Control::Brightness).shown = Shown::On;
  d = decideItem(p, true, 33, item(kBrightnessUid, true), 0);
  CHECK(d.action == TickAction::Apply && !d.targetOn && d.control == Control::Brightness);
  rowOf(p, Control::Brightness).shown = Shown::Off;
  rowOf(p, Control::Brightness).shownLevel = 60;
  d = decideItem(p, true, 34, item(kBrightnessUid, true), 0);
  CHECK(d.action == TickAction::Apply && !d.targetOn && d.control == Control::Brightness);
  rowOf(p, Control::Brightness).shown = Shown::Unknown;
  CHECK(decideItem(p, true, 35, item(kBrightnessUid, true), 0).action == TickAction::Refresh);

  // Rate limit (shared by both rows): a job at t, a tick at t + 4999 ->
  // RateLimited (re-push unchanged), at t + 5000 -> Apply.
  linkReset(p);
  rowOf(p, Control::Wear).shown = Shown::Off;
  rowOf(p, Control::Silent).shown = Shown::On;
  const uint32_t t = 50000;
  noteJobStarted(p, t);
  p.jobInFlight = false;
  CHECK(decideItem(p, true, 10, item(kWearUid, true), t + 4999).action == TickAction::RateLimited);
  CHECK(rowOf(p, Control::Wear).pending && rowOf(p, Control::Wear).next == Shown::Off);
  CHECK(decideItem(p, true, 14, item(kSilentUid, true), t + 4999).action == TickAction::RateLimited);
  CHECK(rowOf(p, Control::Silent).pending && rowOf(p, Control::Silent).next == Shown::On);
  rowOf(p, Control::AutoBright).shown = Shown::On;
  CHECK(decideItem(p, true, 15, item(kAutoBrightUid, true), t + 4999).action == TickAction::RateLimited);
  CHECK(rowOf(p, Control::AutoBright).pending && rowOf(p, Control::AutoBright).next == Shown::On);
  d = decideItem(p, true, 11, item(kWearUid, true), t + 5000);
  CHECK(d.action == TickAction::Apply && d.targetOn);

  // millis wrap: 0xFFFFF000 -> 0x00000388 is 5000 ms.
  linkReset(p);
  rowOf(p, Control::Wear).shown = Shown::On;
  noteJobStarted(p, 0xFFFFF000u);
  p.jobInFlight = false;
  CHECK(decideItem(p, true, 12, item(kWearUid, true), 0x00000387u).action == TickAction::RateLimited);
  d = decideItem(p, true, 13, item(kWearUid, true), 0x00000388u);
  CHECK(d.action == TickAction::Apply && !d.targetOn);

  // Disabled and NotCompleted come before the dedup check: a later enabled
  // tick with the same counter still counts.
  linkReset(p);
  CHECK(decideItem(p, false, 20, item(kWearUid, true), 0).action == TickAction::Disabled);
  CHECK(decideItem(p, true, 20, item(kWearUid, true), 0).action == TickAction::Refresh);

  // noteRepush keeps a newer pending state.
  linkReset(p);
  rowOf(p, Control::Silent).shown = Shown::Off;
  rowOf(p, Control::Silent).next = Shown::On;
  rowOf(p, Control::Silent).pending = true;
  noteRepush(p, Control::Silent);
  CHECK(rowOf(p, Control::Silent).next == Shown::On);

  // linkReset clears every row and dedup.
  rowOf(p, Control::Wear).shown = Shown::On;
  rowOf(p, Control::Wear).next = Shown::Off;
  rowOf(p, Control::Wear).magic = 7;
  rowOf(p, Control::Silent).magic = 8;
  rowOf(p, Control::AutoBright).magic = 9;
  linkReset(p);
  for (const Row& r : p.row)
    CHECK(r.shown == Shown::Unknown && r.next == Shown::Unknown && r.magic == 0 && !r.pending);
  CHECK(p.dedup.used == 0);
}

// ---- 8. afterRead / afterApply ---------------------------------------------------------
static WearResult res(WearStage s, bool known = false, bool on = false) {
  WearResult r{};
  r.stage = s;
  r.known = known;
  r.on = on;
  return r;
}

// Head-up defaults to known off; the Brightness row follows auto brightness as
// a real read makes it (brightnessRowState): "Auto" when on, the level (default
// 50; -1 = no level) when off, "?" when auto brightness is unknown.
static ReadResult rd(bool readOk, bool full, bool wearKnown, bool wearOn, bool silentKnown, bool silentOn,
                     bool autoKnown = false, bool autoOn = false, bool headKnown = true, bool headOn = false,
                     int level = 50) {
  ReadResult r{};
  r.readOk = readOk;
  r.fullProfile = full;
  r.sw[rowSlot(Control::Wear)] = SwitchState{wearKnown, wearOn, 0};
  r.sw[rowSlot(Control::Silent)] = SwitchState{silentKnown, silentOn, 0};
  r.sw[rowSlot(Control::AutoBright)] = SwitchState{autoKnown, autoOn, 0};
  r.sw[rowSlot(Control::HeadUp)] = SwitchState{headKnown, headOn, 0};
  r.sw[rowSlot(Control::Brightness)] =
      brightnessRowState(SwitchState{autoKnown, autoOn, 0}, level >= 0, (uint8_t)(level < 0 ? 0 : level));
  return r;
}

static void testAfter() {
  // One read gives both rows, and both are pushed.
  Policy p{};
  p.jobInFlight = true;
  CHECK(afterRead(p, rd(true, true, true, true, true, false), true) == After::Push);
  CHECK(!p.jobInFlight);
  CHECK(rowOf(p, Control::Wear).next == Shown::On && rowOf(p, Control::Silent).next == Shown::Off);
  CHECK(rowOf(p, Control::Wear).pending && rowOf(p, Control::Silent).pending);
  CHECK(afterRead(p, rd(true, true, true, false, true, true), true) == After::Push);
  CHECK(rowOf(p, Control::Wear).next == Shown::Off && rowOf(p, Control::Silent).next == Shown::On);
  // Silent unknown (a partial reply), wear known: only silent gets "?".
  CHECK(afterRead(p, rd(true, false, true, true, false, false), true) == After::Push);
  CHECK(rowOf(p, Control::Wear).next == Shown::On && rowOf(p, Control::Silent).next == Shown::Unknown);
  // Read not answered: every row "?".
  CHECK(afterRead(p, rd(false, false, true, true, true, true, true, true), true) == After::Push);
  CHECK(rowOf(p, Control::Wear).next == Shown::Unknown && rowOf(p, Control::Silent).next == Shown::Unknown);
  CHECK(rowOf(p, Control::AutoBright).next == Shown::Unknown);
  // One read feeds every row: auto brightness on (f18 = 1) and off (f18
  // left out of a full reply, known off), and unknown (a partial reply).
  CHECK(afterRead(p, rd(true, true, true, true, true, false, true, true), true) == After::Push);
  CHECK(rowOf(p, Control::Wear).next == Shown::On && rowOf(p, Control::Silent).next == Shown::Off &&
        rowOf(p, Control::AutoBright).next == Shown::On);
  CHECK(rowOf(p, Control::Wear).pending && rowOf(p, Control::Silent).pending && rowOf(p, Control::AutoBright).pending);
  CHECK(afterRead(p, rd(true, true, true, true, true, false, true, false), true) == After::Push);
  CHECK(rowOf(p, Control::AutoBright).next == Shown::Off);
  CHECK(afterRead(p, rd(true, false, true, true, false, false, false, false), true) == After::Push);
  CHECK(rowOf(p, Control::Wear).next == Shown::On && rowOf(p, Control::AutoBright).next == Shown::Unknown);
  p.jobInFlight = true;
  rowOf(p, Control::Wear).next = Shown::On;
  clearPending(p);
  CHECK(afterRead(p, rd(true, true, true, true, true, true), false) == After::Unsupported);
  CHECK(!p.jobInFlight && rowOf(p, Control::Wear).next == Shown::On && !anyPending(p));

  struct Case { WearStage s; bool known; After want; };
  const Case cases[] = {
      {WearStage::NotReady, false, After::Drop},
      {WearStage::LostAfterSend, false, After::Drop},
      {WearStage::LostDuringReadback, false, After::Drop},
      {WearStage::FwRejected, false, After::Unsupported},
      {WearStage::Done, true, After::Push},
      {WearStage::AlreadySet, true, After::Push},
      {WearStage::Done, false, After::ReadThenPush},
      {WearStage::Stale, false, After::ReadThenPush},
      {WearStage::PreReadFailed, false, After::ReadThenPush},
      {WearStage::BuilderRejected, false, After::ReadThenPush},
      {WearStage::SendFailed, false, After::ReadThenPush},
  };
  for (const Control c : {Control::Wear, Control::Silent, Control::AutoBright, Control::HeadUp,
                          Control::Brightness}) {
    for (const Case& k : cases) {
      Policy q{};
      q.jobInFlight = true;
      CHECK(afterApply(q, c, res(k.s, k.known, true)) == k.want);
      CHECK(!q.jobInFlight);
      CHECK(rowOf(q, c).next == (k.want == After::Push ? Shown::On : Shown::Unknown));
      CHECK(rowOf(q, c).pending == (k.want == After::Push));
      // Only the written row, and its linked brightness row (testBrightnessApply).
      const Control linked = c == Control::AutoBright ? Control::Brightness
                           : c == Control::Brightness ? Control::AutoBright : c;
      for (uint8_t i = 0; i < kRowCount; ++i) {
        if ((Control)i == c || (Control)i == linked) continue;
        CHECK(!q.row[i].pending && q.row[i].next == Shown::Unknown);
      }
      if (linked != c && k.want != After::Push) CHECK(!rowOf(q, linked).pending);
      CHECK(stageName(k.s)[0] != '\0');
    }
  }
  CHECK(stageName(WearStage::AutoOffUnacked)[0] != '\0');
  Policy q{};
  CHECK(afterApply(q, Control::Silent, res(WearStage::Done, true, false)) == After::Push);
  CHECK(rowOf(q, Control::Silent).next == Shown::Off);
  CHECK(afterApply(q, Control::AutoBright, res(WearStage::Done, true, false)) == After::Push);
  CHECK(rowOf(q, Control::AutoBright).next == Shown::Off);

  // pushSent: every row shows its next title with the update's magic.
  Policy r{};
  rowOf(r, Control::Wear).next = Shown::Off;
  rowOf(r, Control::Wear).pending = true;
  rowOf(r, Control::Silent).next = Shown::On;
  CHECK(pushStep(r, 0) == PushStep::Send);
  pushSent(r, 9, 0);
  for (uint8_t i = 0; i < kRowCount; ++i) CHECK(r.row[i].magic == 9 && !r.row[i].acked && !r.row[i].pending);
  CHECK(rowOf(r, Control::Wear).shown == Shown::Off && rowOf(r, Control::Silent).shown == Shown::On);
  CHECK(rowOf(r, Control::AutoBright).shown == Shown::Unknown && r.push.awaitingAck && r.push.magic == 9);

  // noteAck: the update's magic, total_count 5 when present; one ACK covers all rows.
  CHECK(!noteAck(r, Ack{8, true, 5}));     // another magic
  CHECK(!noteAck(r, Ack{9, true, 4}));     // another list (four items)
  CHECK(!noteAck(r, Ack{0, false, 0}));
  for (uint8_t i = 0; i < kRowCount; ++i) CHECK(!r.row[i].acked);
  CHECK(noteAck(r, Ack{9, true, 5}));
  for (uint8_t i = 0; i < kRowCount; ++i) CHECK(r.row[i].acked);
  CHECK(!r.push.awaitingAck && pushStep(r, 10) == PushStep::Idle);
  CHECK(!noteAck(r, Ack{9, true, 5}));     // a second ACK of the same update
  Policy r2{};
  rowOf(r2, Control::HeadUp).pending = true;
  pushSent(r2, 10, 0);
  CHECK(noteAck(r2, Ack{10, false, 0}));   // no total_count: the magic is enough
  Policy none{};
  CHECK(!noteAck(none, Ack{0, false, 0}) && !noteAck(none, Ack{1, true, 5}));

  // markUnknown / clearPending.
  markUnknown(r, Control::Silent);
  CHECK(rowOf(r, Control::Silent).next == Shown::Unknown && rowOf(r, Control::Silent).pending);
  clearPending(r);
  CHECK(!anyPending(r));
}

// ---- 8b. silent change pushes (sid 0x09 cmd 3 f5{f2}) ----------------------
static void testChangePush() {
  // A live row: the first push of a new state re-adds it; repeats (the glasses
  // send 2-3) are duplicates, whether the push is still pending or done.
  Policy p{};
  Row& s = rowOf(p, Control::Silent);
  s.shown = s.next = Shown::Off;
  s.magic = 5;
  CHECK(noteChangePush(p, Control::Silent, true, true) == ChangeAction::Push);
  CHECK(s.next == Shown::On && s.pending);
  CHECK(noteChangePush(p, Control::Silent, true, true) == ChangeAction::Duplicate);
  notePushed(p, Control::Silent, 6);
  CHECK(noteChangePush(p, Control::Silent, true, true) == ChangeAction::Duplicate);
  CHECK(!s.pending);
  // Off again (the zero is explicit in the push).
  CHECK(noteChangePush(p, Control::Silent, false, true) == ChangeAction::Push);
  CHECK(s.next == Shown::Off && s.pending);
  // Coalescing: on then off before the push went out -> one pending push of off.
  CHECK(noteChangePush(p, Control::Silent, true, true) == ChangeAction::Push);
  CHECK(noteChangePush(p, Control::Silent, false, true) == ChangeAction::Push);
  CHECK(s.next == Shown::Off && s.pending);
  // A "?" row learns its state from a push.
  linkReset(p);
  rowOf(p, Control::Silent).magic = 3;
  CHECK(noteChangePush(p, Control::Silent, true, true) == ChangeAction::Push);
  CHECK(rowOf(p, Control::Silent).next == Shown::On);
  // Not live (row not added on this link, or feed off): noted, no push.
  linkReset(p);
  CHECK(noteChangePush(p, Control::Silent, true, false) == ChangeAction::Noted);
  CHECK(rowOf(p, Control::Silent).next == Shown::On && !rowOf(p, Control::Silent).pending);
  CHECK(noteChangePush(p, Control::Silent, true, false) == ChangeAction::Duplicate);
  CHECK(!anyPending(p));
  CHECK(!rowOf(p, Control::Wear).pending);   // the wear row is untouched

  // Held while a job runs; the newest value wins; applied once the job ends.
  linkReset(p);
  rowOf(p, Control::Silent).shown = rowOf(p, Control::Silent).next = Shown::Off;
  rowOf(p, Control::Silent).magic = 4;
  HeldChange h{};
  ChangeAction a = ChangeAction::Noted;
  CHECK(!takeChange(p, h, Control::Silent, true, &a));   // nothing held
  noteJobStarted(p, 0);
  holdChange(h, true);
  holdChange(h, false);
  holdChange(h, true);
  CHECK(!takeChange(p, h, Control::Silent, true, &a));   // job in flight: still held
  CHECK(h.pending && !anyPending(p));
  p.jobInFlight = false;
  CHECK(takeChange(p, h, Control::Silent, true, &a));
  CHECK(a == ChangeAction::Push && rowOf(p, Control::Silent).next == Shown::On && !h.pending);
  CHECK(!takeChange(p, h, Control::Silent, true, &a));
  // A held push equal to what a silent write just read back is a duplicate.
  linkReset(p);
  rowOf(p, Control::Silent).magic = 4;
  noteJobStarted(p, 0);
  holdChange(h, true);
  WearResult w{};
  w.stage = WearStage::Done;
  w.known = w.on = true;
  CHECK(afterApply(p, Control::Silent, w) == After::Push);
  CHECK(takeChange(p, h, Control::Silent, true, &a) && a == ChangeAction::Duplicate);
  // Delayed read-back (2026-11-21): both read-backs still said off after a
  // silent ON write. The glasses' push (on) came during the job: held, and
  // applied in the owner lap right after the job's result, so it has the last
  // word and the row ends On.
  linkReset(p);
  rowOf(p, Control::Silent).shown = rowOf(p, Control::Silent).next = Shown::Off;
  rowOf(p, Control::Silent).magic = 4;
  noteJobStarted(p, 0);
  holdChange(h, true);
  w = WearResult{};
  w.stage = WearStage::Done;
  w.known = w.rechecked = true;   // read back off twice
  CHECK(afterApply(p, Control::Silent, w) == After::Push && rowOf(p, Control::Silent).next == Shown::Off);
  CHECK(takeChange(p, h, Control::Silent, true, &a) && a == ChangeAction::Push);
  CHECK(rowOf(p, Control::Silent).next == Shown::On && rowOf(p, Control::Silent).pending);
  // The other order: the push comes after the job reported (and its Off row
  // went out); it is taken in its own lap and pushes On.
  linkReset(p);
  rowOf(p, Control::Silent).shown = rowOf(p, Control::Silent).next = Shown::Off;
  rowOf(p, Control::Silent).magic = 4;
  noteJobStarted(p, 0);
  CHECK(afterApply(p, Control::Silent, w) == After::Push);
  notePushed(p, Control::Silent, 5);
  holdChange(h, true);
  CHECK(takeChange(p, h, Control::Silent, true, &a) && a == ChangeAction::Push);
  CHECK(rowOf(p, Control::Silent).next == Shown::On && rowOf(p, Control::Silent).pending);
  CHECK(changeActionName(ChangeAction::Push)[0] != '\0');
}

// ---- 9. helpers -----------------------------------------------------------------------
static void testHelpers() {
  CHECK(nextMagic(255) == 1);
  CHECK(nextMagic(0) == 1);
  CHECK(nextMagic(1) == 2);
  CHECK(std::strcmp(wearTitle(Shown::On), "Wear Detection: On") == 0);
  CHECK(std::strcmp(wearTitle(Shown::Off), "Wear Detection: Off") == 0);
  CHECK(std::strcmp(wearTitle(Shown::Unknown), "Wear Detection: ?") == 0);
  // Every row's titles (the level is ignored by all but the Brightness row).
  struct T { Control c; Shown s; uint8_t level; const char* want; };
  const T titles[] = {
      {Control::Wear, Shown::On, 0, "Wear Detection: On"},
      {Control::Wear, Shown::Off, 0, "Wear Detection: Off"},
      {Control::Wear, Shown::Unknown, 0, "Wear Detection: ?"},
      {Control::Silent, Shown::On, 0, "Silent Mode: On"},
      {Control::Silent, Shown::Off, 0, "Silent Mode: Off"},
      {Control::Silent, Shown::Unknown, 0, "Silent Mode: ?"},
      {Control::AutoBright, Shown::On, 0, "Auto Brightness: On"},
      {Control::AutoBright, Shown::Off, 0, "Auto Brightness: Off"},
      {Control::AutoBright, Shown::Unknown, 0, "Auto Brightness: ?"},
      {Control::HeadUp, Shown::On, 0, "Head-Up Display: On"},
      {Control::HeadUp, Shown::Off, 0, "Head-Up Display: Off"},
      {Control::HeadUp, Shown::Unknown, 0, "Head-Up Display: ?"},
      {Control::HeadUp, Shown::Off, 60, "Head-Up Display: Off"},
      {Control::Brightness, Shown::On, 0, "Brightness: Auto"},
      {Control::Brightness, Shown::On, 64, "Brightness: Auto"},
      {Control::Brightness, Shown::Off, 25, "Brightness: 25%"},
      {Control::Brightness, Shown::Off, 50, "Brightness: 50%"},
      {Control::Brightness, Shown::Off, 75, "Brightness: 75%"},
      {Control::Brightness, Shown::Off, 100, "Brightness: 100%"},
      {Control::Brightness, Shown::Off, 60, "Brightness: 60%"},
      {Control::Brightness, Shown::Off, 15, "Brightness: 15%"},
      {Control::Brightness, Shown::Off, 0, "Brightness: 0%"},
      {Control::Brightness, Shown::Off, 255, "Brightness: 255%"},
      // The glasses keep only even levels: a level that matches a step shows
      // the step (24 -> 25%, 74 -> 75%); any other level shows as read.
      {Control::Brightness, Shown::Off, 24, "Brightness: 25%"},
      {Control::Brightness, Shown::Off, 26, "Brightness: 26%"},
      {Control::Brightness, Shown::Off, 49, "Brightness: 49%"},
      {Control::Brightness, Shown::Off, 74, "Brightness: 75%"},
      {Control::Brightness, Shown::Unknown, 40, "Brightness: ?"},
  };
  for (const T& t : titles) {
    CHECK(std::strcmp(rowTitle(t.c, t.s, t.level).text, t.want) == 0);
    uint8_t buf[kItemBodyMax];
    CHECK(encodeListItem(ListItem{rowUid(t.c), rowIndex(t.c), rowTitle(t.c, t.s, t.level).text}, buf, sizeof buf) > 0);
  }
  CHECK(std::strcmp(rowTitle(Control::Silent, Shown::On).text, "Silent Mode: On") == 0);
  CHECK(std::strcmp(headUpTitle(Shown::On), "Head-Up Display: On") == 0);
  CHECK(std::strcmp(controlName(Control::AutoBright), "auto brightness") == 0);
  CHECK(std::strcmp(controlName(Control::HeadUp), "head-up display") == 0);
  CHECK(std::strcmp(controlName(Control::Brightness), "brightness") == 0);
  CHECK(kRowCount == 5 && rowSlot(Control::AutoBright) == 2 && rowSlot(Control::HeadUp) == 3 &&
        rowSlot(Control::Brightness) == 4);
  Control c = Control::Wear;
  CHECK(controlForUid(kSilentUid, &c) && c == Control::Silent);
  CHECK(controlForUid(kWearUid, &c) && c == Control::Wear);
  CHECK(controlForUid(kAutoBrightUid, &c) && c == Control::AutoBright);
  CHECK(controlForUid(kHeadUpUid, &c) && c == Control::HeadUp);
  CHECK(controlForUid(kBrightnessUid, &c) && c == Control::Brightness);
  CHECK(!controlForUid(kUidBase, nullptr) && !controlForUid(kUidBase + 6, nullptr));
  for (uint8_t i = 0; i < kRowCount; ++i) {   // uid, index and slot agree for every row
    CHECK(controlForUid(rowUid((Control)i), &c) && c == (Control)i && rowIndex(c) == i);
    CHECK(rowUid((Control)i) == (kUidBase | (uint32_t)(i + 1)));
  }
  CHECK(shownFrom(false, true) == Shown::Unknown);
  CHECK(shownFrom(true, true) == Shown::On);
  CHECK(shownFrom(true, false) == Shown::Off);
  const Bytes a = hex(kAck1);
  char out[2 * 12 + 1];
  CHECK(formatHex(a.data(), a.size(), out, sizeof out) == 24);
  CHECK(std::strcmp(out, kAck1) == 0);
  char small[5];
  formatHex(a.data(), a.size(), small, sizeof small);
  CHECK(std::strcmp(small, "0801") == 0);
  // A zeroed Policy is a valid link-down state.
  Policy z;
  std::memset(&z, 0, sizeof z);
  CHECK(z.row[0].shown == Shown::Unknown && z.row[1].next == Shown::Unknown && z.row[2].next == Shown::Unknown &&
        z.row[3].next == Shown::Unknown && z.row[4].next == Shown::Unknown && z.row[4].nextLevel == 0 &&
        !z.jobInFlight &&
        z.dedup.used == 0 && !anyPending(z));
  HeldChange hz;
  std::memset(&hz, 0, sizeof hz);
  CHECK(!hz.pending);
}

// ---- 9. re-read after "?" (backoff) -------------------------------------------------
static RecoveryGate openGate() {
  RecoveryGate g{};
  g.phaseIdle = g.enabled = g.firmwareExact = true;
  return g;
}

static void testRecovery() {
  // The schedule: 2 s, 5 s, 15 s, 30 s, then every 60 s.
  const uint32_t want[] = {2000, 5000, 15000, 30000, 60000, 60000, 60000};
  for (uint8_t i = 0; i < 7; ++i) CHECK(recoveryDelayMs(i) == want[i]);
  CHECK(recoveryDelayMs(255) == 60000);

  // A zeroed Recovery (and Policy) schedules nothing.
  Recovery z;
  std::memset(&z, 0, sizeof z);
  CHECK(!z.armed && z.step == 0 && !recoveryDue(z, 0) && recoveryWaitMs(z, 12345) == UINT32_MAX);

  // A failed read (every row "?") schedules the first re-read 2 s later.
  Policy p{};
  const uint32_t t0 = 100000;
  CHECK(afterRead(p, rd(false, false, false, false, false, false), true) == After::Push);
  CHECK(anyUnknown(p));
  recoveryAfterRead(p, t0);
  CHECK(p.recovery.armed && p.recovery.step == 1 && p.recovery.delayMs == 2000);
  CHECK(recoveryWaitMs(p.recovery, t0) == 2000 && recoveryWaitMs(p.recovery, t0 + 1500) == 500);
  CHECK(!recoveryDue(p.recovery, t0 + 1999) && recoveryDue(p.recovery, t0 + 2000));
  CHECK(recoveryWaitMs(p.recovery, t0 + 2500) == 0);
  // A second failure while one waits changes nothing (no faster, no later).
  recoveryAfterRead(p, t0 + 1000);
  recoveryArmIfUnknown(p, t0 + 1000);
  CHECK(p.recovery.fromMs == t0 && p.recovery.step == 1 && p.recovery.delayMs == 2000);

  // The pushes of the "?" rows go first, and nothing runs before it is due.
  const RecoveryGate open = openGate();
  RecoveryGate g = open;
  g.otherWork = true;
  CHECK(decideRecovery(p, g, t0 + 5000) == RecoveryAction::Wait);
  CHECK(decideRecovery(p, open, t0 + 1999) == RecoveryAction::Wait);
  // Never while a job runs.
  p.jobInFlight = true;
  CHECK(decideRecovery(p, open, t0 + 5000) == RecoveryAction::Wait);
  p.jobInFlight = false;
  // Due, but held: settling/unsupported, feed off (no manual push), another
  // firmware, a voice or QuickNote session. Held keeps the schedule.
  for (int which = 0; which < 4; ++which) {
    g = open;
    if (which == 0) g.phaseIdle = false;
    if (which == 1) g.enabled = false;
    if (which == 2) g.firmwareExact = false;
    if (which == 3) g.busy = true;
    CHECK(decideRecovery(p, g, t0 + 2000) == RecoveryAction::Blocked);
    CHECK(p.recovery.armed && p.recovery.step == 1);
  }
  CHECK(decideRecovery(p, open, t0 + 2000) == RecoveryAction::Read);
  // Read submitted; it fails again: the next one waits 5 s, then 15, 30, 60, 60.
  uint32_t now = t0 + 2000;
  const uint32_t next[] = {5000, 15000, 30000, 60000, 60000, 60000};
  for (uint32_t d : next) {
    recoveryTaken(p.recovery);
    CHECK(!p.recovery.armed && decideRecovery(p, open, now) == RecoveryAction::None);
    CHECK(afterRead(p, rd(false, false, false, false, false, false), true) == After::Push);
    now += 3000;   // the read job's own time
    recoveryAfterRead(p, now);
    CHECK(p.recovery.armed && p.recovery.delayMs == d);
    CHECK(decideRecovery(p, open, now + d - 1) == RecoveryAction::Wait);
    now += d;
    CHECK(decideRecovery(p, open, now) == RecoveryAction::Read);
  }
  // A read that gives every row a state resets the backoff.
  recoveryTaken(p.recovery);
  CHECK(afterRead(p, rd(true, true, true, true, true, false, true, true), true) == After::Push);
  recoveryAfterRead(p, now);
  CHECK(!p.recovery.armed && p.recovery.step == 0);
  CHECK(decideRecovery(p, open, now + 100000) == RecoveryAction::None);
  // The next failure starts at 2 s again.
  CHECK(afterRead(p, rd(false, false, false, false, false, false), true) == After::Push);
  recoveryAfterRead(p, now);
  CHECK(p.recovery.armed && p.recovery.delayMs == 2000);
  // A partial read (one row still "?") is not a success: the schedule goes on.
  recoveryTaken(p.recovery);
  CHECK(afterRead(p, rd(true, false, true, true, false, false, true, false), true) == After::Push);
  recoveryAfterRead(p, now);
  CHECK(p.recovery.armed && p.recovery.delayMs == 5000);
  // Rows given a state some other way (a change push, a tick's write) clear it.
  for (uint8_t i = 0; i < kRowCount; ++i) p.row[i].next = Shown::Off;
  CHECK(decideRecovery(p, open, now) == RecoveryAction::Cleared);
  CHECK(!p.recovery.armed && p.recovery.step == 0);
  // A write or a timeout that leaves a row "?" schedules a re-read; one that
  // leaves none does nothing (and never resets a running schedule).
  recoveryArmIfUnknown(p, now);
  CHECK(!p.recovery.armed);
  markUnknown(p, Control::Wear);
  recoveryArmIfUnknown(p, now);
  CHECK(p.recovery.armed && p.recovery.delayMs == 2000);
  // linkReset clears the schedule with the rows.
  linkReset(p);
  CHECK(!p.recovery.armed && p.recovery.step == 0 && recoveryWaitMs(p.recovery, now) == UINT32_MAX);

  // Wrap-safe around the 32-bit millisecond counter.
  Policy w{};
  markUnknown(w, Control::Silent);
  recoveryArmIfUnknown(w, 0xFFFFFC00u);   // 1024 ms before the wrap
  CHECK(!recoveryDue(w.recovery, 0xFFFFFFFFu) && recoveryWaitMs(w.recovery, 0xFFFFFFFFu) == 2000 - 1023);
  CHECK(!recoveryDue(w.recovery, 0x000003CFu) && recoveryWaitMs(w.recovery, 0x000003CFu) == 1);
  CHECK(recoveryDue(w.recovery, 0x000003D0u) && recoveryWaitMs(w.recovery, 0x00000400u) == 0);

  // Read diagnostics helpers.
  ReadDiag d{};
  CHECK(!readMade(d) && !readAnswered(d) && !readNeededResend(d));
  d.attempts = 1; d.answeredAttempt = 1;
  CHECK(readMade(d) && readAnswered(d) && !readNeededResend(d));
  d.attempts = 2;
  CHECK(readNeededResend(d));
  d.answeredAttempt = 0; d.attempts = 3;
  CHECK(readMade(d) && !readAnswered(d) && !readNeededResend(d));
  // The results carry them, zeroed.
  WearResult wr{};
  ReadResult rr{};
  CHECK(!readMade(wr.preRead) && !readMade(wr.readBack) && !readMade(rr.diag));
  // The delayed read-back's recheck (2026-11-21), as `g2 list status` shows it.
  CHECK(!readMade(wr.recheck) && recheckOf(wr) == Recheck::None && !strcmp(recheckSuffix(Recheck::None), ""));
  wr.rechecked = true;
  CHECK(recheckOf(wr) == Recheck::Unanswered && !strcmp(recheckSuffix(recheckOf(wr)), " (recheck unanswered)"));
  wr.recheck.attempts = 1; wr.recheck.answeredAttempt = 1;
  CHECK(recheckOf(wr) == Recheck::Answered && !strcmp(recheckSuffix(recheckOf(wr)), " after recheck"));
  wr = WearResult{}; wr.recheckSkipped = true;
  CHECK(recheckOf(wr) == Recheck::Skipped && !strcmp(recheckSuffix(recheckOf(wr)), " (no time left to recheck)"));
  // The read-back waits stay well inside the job backstop.
  CHECK(kReadbackDelayMs == 500 && kReadbackRecheckMs == 1500 && kReadbackSliceMs <= 100);
  CHECK(kJobStartDeadlineMs + kReadbackRecheckBudgetMs + kReadbackRecheckMs + 6750 < kJobBackstopMs);
  // The two-write Brightness job (auto off + level): 24.55 s worst case, with
  // at least 3 s to the backstop (30 s since the Brightness row).
  CHECK(kBrightnessWriteWorstMs == 3000 + 6750 + 50 + 3250 + 500 + 3250 + 500 + 500 + 6750);
  CHECK(kBrightnessWriteWorstMs + 3000 <= kJobBackstopMs && kJobBackstopMs == 30000);
  CHECK(kSwitchWriteWorstMs + 3000 <= kJobBackstopMs && kRecheckWriteWorstMs + 3000 <= kJobBackstopMs);
}

// The re-read after "?" itself (fix 2026-11-21): a failed one changes and
// pushes nothing, a successful one pushes only rows whose title changes, it
// never turns a known row into "?", and it does not use up the tick limit.
// One FULL_UPDATE of all five rows when one is pending, then its ACK.
static void pushAll(Policy& p, uint8_t& magic) {
  if (!anyPending(p)) return;
  CHECK(pushStep(p, 0) == PushStep::Send);
  pushSent(p, ++magic, 0);
  CHECK(noteAck(p, Ack{magic, true, kRowCount}));
  CHECK(!anyPending(p) && pushStep(p, 0) == PushStep::Idle);
}

static void testRecoveryRead() {
  const RecoveryGate open = openGate();
  uint8_t magic = 0;
  // Wear and auto brightness known and on the lens; a silent write timed out ("?").
  Policy p{};
  CHECK(afterRead(p, rd(true, true, true, true, true, false, true, true), true) == After::Push);
  pushAll(p, magic);
  markUnknown(p, Control::Silent);
  recoveryArmIfUnknown(p, 1000);
  pushAll(p, magic);
  CHECK(!anyPending(p) && p.recovery.armed && p.recovery.delayMs == 2000);
  CHECK(decideRecovery(p, open, 3000) == RecoveryAction::Read);
  noteBackgroundJobStarted(p);
  recoveryTaken(p.recovery);
  CHECK(p.jobInFlight && !p.everJob);
  // The re-read goes unanswered (or its job started too late): no row changes,
  // nothing is pushed, the next step is scheduled.
  CHECK(afterRecoveryRead(p, rd(false, false, false, false, false, false), true) == After::None);
  CHECK(!p.jobInFlight && !anyPending(p));
  CHECK(p.row[rowSlot(Control::Wear)].next == Shown::On && p.row[rowSlot(Control::Wear)].shown == Shown::On);
  CHECK(p.row[rowSlot(Control::AutoBright)].next == Shown::On);
  CHECK(p.row[rowSlot(Control::Silent)].next == Shown::Unknown);
  recoveryAfterRead(p, 6000);
  CHECK(p.recovery.armed && p.recovery.delayMs == 5000);
  // A tick on a known row right after the re-read is applied, not rate-limited.
  Decision d = decideItem(p, true, 7, item(kWearUid, true), 6100);
  CHECK(d.action == TickAction::Apply && d.control == Control::Wear && !d.targetOn);
  // ...and that tick's job does use up the 5 s budget.
  noteJobStarted(p, 6100);
  p.jobInFlight = false;
  CHECK(decideItem(p, true, 8, item(kAutoBrightUid, true), 7000).action == TickAction::RateLimited);
  pushAll(p, magic);

  // A partial answer: the known rows take their state, an unknown one keeps its
  // own (a known row never turns into "?"); only changed titles are pushed.
  Policy q{};
  CHECK(afterRead(q, rd(true, true, true, true, true, true, true, false), true) == After::Push);
  pushAll(q, magic);
  markUnknown(q, Control::Silent);
  pushAll(q, magic);
  noteBackgroundJobStarted(q);
  CHECK(afterRecoveryRead(q, rd(true, false, true, false, false, false, true, false), true) == After::Push);
  CHECK(q.row[rowSlot(Control::Wear)].next == Shown::Off && q.row[rowSlot(Control::Wear)].pending);
  CHECK(q.row[rowSlot(Control::AutoBright)].next == Shown::Off && !q.row[rowSlot(Control::AutoBright)].pending);
  CHECK(q.row[rowSlot(Control::Silent)].next == Shown::Unknown && !q.row[rowSlot(Control::Silent)].pending);
  recoveryAfterRead(q, 10000);
  CHECK(q.recovery.armed);   // silent still "?"
  pushAll(q, magic);
  // The full answer: the "?" row gets its state and is the only one pushed.
  noteBackgroundJobStarted(q);
  CHECK(afterRecoveryRead(q, rd(true, true, true, false, true, true, true, false), true) == After::Push);
  CHECK(!q.row[rowSlot(Control::Wear)].pending && !q.row[rowSlot(Control::AutoBright)].pending);
  CHECK(q.row[rowSlot(Control::Silent)].next == Shown::On && q.row[rowSlot(Control::Silent)].pending);
  recoveryAfterRead(q, 20000);
  CHECK(!q.recovery.armed && q.recovery.step == 0);
  // Every row already right and ACKed: nothing to push.
  pushAll(q, magic);
  noteBackgroundJobStarted(q);
  CHECK(afterRecoveryRead(q, rd(true, true, true, false, true, true, true, false), true) == After::None);
  CHECK(!anyPending(q));
  // A FULL_UPDATE that was not ACKed is sent again even when nothing changed.
  rowOf(q, Control::Wear).pending = true;
  pushSent(q, ++magic, 0);                 // the same titles, no ACK this time
  q.push.awaitingAck = false;              // (its wait ran out)
  CHECK(q.row[rowSlot(Control::Wear)].shown == Shown::Off && !q.row[rowSlot(Control::Wear)].acked);
  noteBackgroundJobStarted(q);
  CHECK(afterRecoveryRead(q, rd(false, false, false, false, false, false), true) == After::None);
  CHECK(!anyPending(q));
  CHECK(afterRecoveryRead(q, rd(true, true, true, false, true, true, true, false), true) == After::Push);
  CHECK(q.row[rowSlot(Control::Wear)].pending && q.row[rowSlot(Control::Wear)].next == Shown::Off);
  // Another firmware: Unsupported, like afterRead.
  CHECK(afterRecoveryRead(q, rd(true, false, true, true, true, true, true, true), false) == After::Unsupported);

  // A due re-read held by the gate is Blocked even while other work waits on
  // the same gate (so the owner does not lap fast for it).
  Policy b{};
  markUnknown(b, Control::Wear);
  recoveryArmIfUnknown(b, 0);
  RecoveryGate g = open;
  g.otherWork = true;
  g.busy = true;
  CHECK(decideRecovery(b, g, 2000) == RecoveryAction::Blocked);
  g.busy = false;
  CHECK(decideRecovery(b, g, 2000) == RecoveryAction::Wait);
  CHECK(decideRecovery(b, open, 1999) == RecoveryAction::Wait);
  g = open;
  g.busy = true;
  CHECK(decideRecovery(b, g, 1999) == RecoveryAction::Wait);   // not due: Wait, not Blocked
}

// ---- 10. the Brightness row: step, write whitelist, row state ---------------
static void testBrightness() {
  // The read-back match rule (device test 2026-11-21: the glasses keep only
  // even levels, 25 -> 24 and 75 -> 74; 50 and 100 exact): equal, or the
  // written level is odd and the read one is one less. Nothing wider.
  struct M { uint8_t written, read; bool match; };
  const M matches[] = {{25, 24, true},  {24, 25, false}, {50, 49, false}, {75, 74, true},  {100, 100, true},
                       {2, 2, true},    {3, 2, true},    {25, 25, true},  {25, 23, false}, {50, 48, false},
                       {100, 99, false}, {75, 76, false}, {1, 0, true},   {0, 0, true},    {0, 255, false}};
  for (const M& t : matches) CHECK(brightnessLevelMatches(t.written, t.read) == t.match);
  // The step a level matches, and the level the row shows.
  CHECK(brightnessStepIndex(24) == 0 && brightnessStepIndex(25) == 0 && brightnessStepIndex(26) == -1);
  CHECK(brightnessStepIndex(49) == -1 && brightnessStepIndex(50) == 1 && brightnessStepIndex(74) == 2);
  CHECK(brightnessStepIndex(75) == 2 && brightnessStepIndex(99) == -1 && brightnessStepIndex(100) == 3);
  CHECK(brightnessShownLevel(24) == 25 && brightnessShownLevel(74) == 75 && brightnessShownLevel(60) == 60);
  CHECK(brightnessShownLevel(49) == 49 && brightnessShownLevel(0) == 0 && brightnessShownLevel(100) == 100);
  // The step after a level (auto brightness off): when it matches a step, the
  // following step (24 counts as 25, 74 as 75; 100 wraps to 25); otherwise the
  // smallest of 25/50/75/100 strictly above it, and 100 and above wrap to 25.
  struct S { uint8_t level, next; };
  const S steps[] = {{0, 25},  {2, 25},  {20, 25}, {24, 50},  {25, 50},  {26, 50}, {49, 50},  {50, 75},
                     {60, 75}, {74, 100}, {75, 100}, {99, 100}, {100, 25}, {101, 25}, {255, 25}};
  for (const S& t : steps) CHECK(brightnessStep(t.level) == t.next);
  // The cycle as the glasses store it (25 -> 24, 75 -> 74): never stuck.
  CHECK(brightnessStep(uint8_t(brightnessStep(100) - 1)) == 50 && brightnessStep(uint8_t(brightnessStep(50) - 1)) == 100);
  // Auto brightness on: always 25 (after the auto-off write), whatever level it chose.
  for (uint8_t level : {0, 15, 25, 64, 100}) CHECK(brightnessTarget(true, level) == 25);
  CHECK(brightnessTarget(false, 60) == 75 && brightnessTarget(false, 100) == 25 && brightnessTarget(false, 0) == 25);
  // A full cycle from Auto: 25, 50, 75, 100, 25.
  uint8_t level = brightnessTarget(true, 64);
  const uint8_t cycle[] = {25, 50, 75, 100, 25};
  for (uint8_t want : cycle) {
    CHECK(level == want);
    level = brightnessStep(level);
  }
  CHECK(kBrightnessStepCount == 4);

  // The settings writes a tick may send: the four switches (0/1) and the
  // level at a step only. The angle, position and the other fields never.
  for (uint8_t inner : {kSetwWear, kSetwSilent, kSetwBrightness, kSetwHeadUp}) {
    CHECK(rowWriteAllowed(inner, kSetwLeafSwitch, 0) && rowWriteAllowed(inner, kSetwLeafSwitch, 1));
    CHECK(!rowWriteAllowed(inner, kSetwLeafSwitch, 2));
  }
  for (uint8_t v : kBrightnessSteps) CHECK(rowWriteAllowed(kSetwBrightness, kSetwLeafLevel, v));
  for (uint32_t v : {0u, 1u, 2u, 24u, 26u, 60u, 99u, 101u, 255u, 256u + 25u})
    CHECK(!rowWriteAllowed(kSetwBrightness, kSetwLeafLevel, v));
  CHECK(!rowWriteAllowed(kSetwHeadUp, 2, 60));   // the head-up angle
  for (uint8_t inner : {0, 2, 3, 7, 8, 9, 10, 11, 12})
    CHECK(!rowWriteAllowed(inner, kSetwLeafSwitch, 1));
  CHECK(!rowWriteAllowed(kSetwWear, kSetwLeafLevel, 25) && !rowWriteAllowed(kSetwBrightness, 3, 1));
  CHECK(kSetwBrightness == 1 && kSetwHeadUp == 4 && kSetwWear == 5 && kSetwSilent == 6 && kSetwLeafLevel == 2);

  // The Brightness row from auto brightness (f18) and the level (f2).
  SwitchState s = brightnessRowState(SwitchState{true, true, 0}, true, 64);
  CHECK(s.known && s.on);                                   // "Auto" whatever the level
  s = brightnessRowState(SwitchState{true, true, 0}, false, 0);
  CHECK(s.known && s.on);                                   // the level is not needed
  s = brightnessRowState(SwitchState{true, false, 0}, true, 60);
  CHECK(s.known && !s.on && s.level == 60);
  s = brightnessRowState(SwitchState{true, false, 0}, false, 0);
  CHECK(!s.known);                                          // off, level unknown: "?"
  s = brightnessRowState(SwitchState{false, false, 0}, true, 60);
  CHECK(!s.known);                                          // auto brightness unknown: "?"

  // Row titles compare as (state, level): a level change is a title change.
  Row r{};
  r.shown = r.next = Shown::Off;
  r.shownLevel = 50;
  r.nextLevel = 75;
  CHECK(rowChanged(r));
  r.nextLevel = 50;
  CHECK(!rowChanged(r));
  // ... compared as shown: 24 and 25 both show "25%", 74 and 75 "75%".
  r.shownLevel = 25;
  r.nextLevel = 24;
  CHECK(!rowChanged(r));
  r.shownLevel = 74;
  r.nextLevel = 75;
  CHECK(!rowChanged(r));
  r.nextLevel = 76;
  CHECK(rowChanged(r));
  r.shownLevel = r.nextLevel = 50;
  // setNext keeps the level only for the Brightness row at Off.
  setNext(r, Control::Brightness, SwitchState{true, false, 75});
  CHECK(r.next == Shown::Off && r.nextLevel == 75);
  setNext(r, Control::Brightness, SwitchState{true, true, 64});
  CHECK(r.next == Shown::On && r.nextLevel == 0);
  setNext(r, Control::Brightness, SwitchState{false, false, 75});
  CHECK(r.next == Shown::Unknown && r.nextLevel == 0);
  setNext(r, Control::HeadUp, SwitchState{true, false, 75});
  CHECK(r.next == Shown::Off && r.nextLevel == 0);
  // notePushed / noteRepush / markUnknown carry the level.
  Policy p{};
  setNext(rowOf(p, Control::Brightness), Control::Brightness, SwitchState{true, false, 75});
  rowOf(p, Control::Brightness).pending = true;
  notePushed(p, Control::Brightness, 7);
  CHECK(rowOf(p, Control::Brightness).shown == Shown::Off && rowOf(p, Control::Brightness).shownLevel == 75);
  noteRepush(p, Control::Brightness);
  CHECK(rowOf(p, Control::Brightness).next == Shown::Off && rowOf(p, Control::Brightness).nextLevel == 75);
  markUnknown(p, Control::Brightness);
  CHECK(rowOf(p, Control::Brightness).next == Shown::Unknown && rowOf(p, Control::Brightness).nextLevel == 0);

  // A read: Auto, a level, "?"; afterRead sets the level with the state.
  Policy q{};
  CHECK(afterRead(q, rd(true, true, true, true, true, false, true, true, true, true, 64), true) == After::Push);
  CHECK(rowOf(q, Control::Brightness).next == Shown::On && rowOf(q, Control::HeadUp).next == Shown::On);
  CHECK(afterRead(q, rd(true, true, true, true, true, false, true, false, true, false, 60), true) == After::Push);
  CHECK(rowOf(q, Control::Brightness).next == Shown::Off && rowOf(q, Control::Brightness).nextLevel == 60);
  CHECK(rowOf(q, Control::HeadUp).next == Shown::Off);
  CHECK(std::strcmp(rowTitle(Control::Brightness, rowOf(q, Control::Brightness).next,
                             rowOf(q, Control::Brightness).nextLevel).text, "Brightness: 60%") == 0);
  CHECK(afterRead(q, rd(true, false, true, true, true, false, true, false, false, false, -1), true) == After::Push);
  CHECK(rowOf(q, Control::Brightness).next == Shown::Unknown && rowOf(q, Control::HeadUp).next == Shown::Unknown);
  CHECK(afterRead(q, rd(false, false, true, true, true, false, true, false, true, true, 60), true) == After::Push);
  CHECK(rowOf(q, Control::Brightness).next == Shown::Unknown && rowOf(q, Control::Brightness).nextLevel == 0);
  // A re-read after "?" pushes the Brightness row when only its level changed.
  uint8_t magic = 0;
  Policy t{};
  CHECK(afterRead(t, rd(true, true, true, true, true, false, true, false, true, true, 50), true) == After::Push);
  pushAll(t, magic);
  markUnknown(t, Control::Wear);
  pushAll(t, magic);
  noteBackgroundJobStarted(t);
  CHECK(afterRecoveryRead(t, rd(true, true, true, true, true, false, true, false, true, true, 75), true) ==
        After::Push);
  CHECK(rowOf(t, Control::Brightness).pending && rowOf(t, Control::Brightness).nextLevel == 75);
  CHECK(rowOf(t, Control::Wear).pending && !rowOf(t, Control::HeadUp).pending &&
        !rowOf(t, Control::AutoBright).pending);
  pushAll(t, magic);
  noteBackgroundJobStarted(t);
  CHECK(afterRecoveryRead(t, rd(true, true, true, true, true, false, true, false, true, true, 75), true) ==
        After::None);

  // A Brightness write's read-back confirms the target (and auto off when it
  // was turned off); takeReadBack copies only the read-back fields.
  WearResult w{};
  w.readOk = w.levelKnown = true;
  w.level = w.target = 75;
  CHECK(brightnessConfirmed(w));
  w.level = 60;
  CHECK(!brightnessConfirmed(w));
  w.level = 74;                            // 75 stored as 74 (even levels): confirmed
  CHECK(brightnessConfirmed(w));
  w.level = w.target = 50;
  CHECK(brightnessConfirmed(w));
  w.level = 49;                            // 50 is even: 49 is not it
  CHECK(!brightnessConfirmed(w));
  w.target = 25;
  w.level = 24;                            // 25 stored as 24: confirmed on the first read-back
  CHECK(brightnessConfirmed(w));
  w.level = 25;
  w.target = 25;
  w.autoWritten = true;
  w.autoKnown = true;
  w.autoOn = true;
  CHECK(!brightnessConfirmed(w));          // auto brightness still on
  w.autoOn = false;
  CHECK(brightnessConfirmed(w));
  w.autoKnown = false;
  CHECK(!brightnessConfirmed(w));
  w.readOk = false;
  CHECK(!brightnessConfirmed(w));
  WearResult a{}, b{};
  a.target = 50;
  a.autoWritten = a.acked = true;
  b.readOk = b.known = b.levelKnown = b.autoKnown = true;
  b.level = 50;
  b.target = 99;
  takeReadBack(a, b);
  CHECK(a.readOk && a.known && a.levelKnown && a.level == 50 && a.autoKnown && !a.autoOn);
  CHECK(a.target == 50 && a.autoWritten && a.acked);   // the write's own fields stay
}

// ---- 11. a Brightness / Auto Brightness / Head-Up tick's result -------------------
// The rows as a connect read leaves them, pushed and ACKed.
static Policy pushedRows(bool autoOn, int level, bool headOn, uint8_t& magic) {
  Policy p{};
  CHECK(afterRead(p, rd(true, true, true, true, true, false, true, autoOn, true, headOn, level), true) == After::Push);
  pushAll(p, magic);
  CHECK(!anyPending(p));
  return p;
}
static WearResult brightResult(WearStage stage, bool autoWritten, uint8_t target, bool readOk, bool autoOn,
                               int level) {
  WearResult r{};
  r.stage = stage;
  r.target = target;
  r.autoWritten = autoWritten;
  r.readOk = readOk;
  r.autoKnown = readOk;
  r.autoOn = autoOn;
  r.levelKnown = readOk && level >= 0;
  r.level = (uint8_t)(level < 0 ? 0 : level);
  const SwitchState s = brightnessRowState(SwitchState{r.autoKnown, r.autoOn, 0}, r.levelKnown, r.level);
  r.known = s.known;
  r.on = s.on;
  return r;
}

static void testBrightnessApply() {
  uint8_t magic = 0;
  Control c = Control::Wear;
  // Auto brightness on: the tick turned it off and wrote 25; the read-back
  // confirms both. Both rows are pushed: Auto Brightness (index 2) first.
  Policy p = pushedRows(true, 64, true, magic);
  CHECK(rowOf(p, Control::Brightness).shown == Shown::On && rowOf(p, Control::AutoBright).shown == Shown::On);
  noteJobStarted(p, 0);
  WearResult r = brightResult(WearStage::Done, true, 25, true, false, 25);
  CHECK(brightnessConfirmed(r));
  CHECK(afterApply(p, Control::Brightness, r) == After::Push && !p.jobInFlight);
  CHECK(nextPendingRow(p, &c) && c == Control::AutoBright && rowOf(p, c).next == Shown::Off);
  notePushed(p, c, ++magic);
  CHECK(nextPendingRow(p, &c) && c == Control::Brightness);
  CHECK(std::strcmp(rowTitle(c, rowOf(p, c).next, rowOf(p, c).nextLevel).text, "Brightness: 25%") == 0);
  notePushed(p, c, ++magic);
  CHECK(!anyPending(p));   // wear, silent and head-up were not touched

  // Auto off at 60: 75; the Auto Brightness row is unchanged (still Off, ACKed) and not pushed.
  Policy q = pushedRows(false, 60, false, magic);
  CHECK(rowOf(q, Control::Brightness).shownLevel == 60);
  noteJobStarted(q, 0);
  CHECK(afterApply(q, Control::Brightness, brightResult(WearStage::Done, false, 75, true, false, 75)) == After::Push);
  CHECK(nextPendingRow(q, &c) && c == Control::Brightness && rowOf(q, c).nextLevel == 75);
  CHECK(!rowOf(q, Control::AutoBright).pending);
  pushAll(q, magic);
  // 100 -> 25.
  Policy w = pushedRows(false, 100, false, magic);
  noteJobStarted(w, 0);
  CHECK(afterApply(w, Control::Brightness, brightResult(WearStage::Done, false, 25, true, false, 25)) == After::Push);
  CHECK(rowOf(w, Control::Brightness).next == Shown::Off && rowOf(w, Control::Brightness).nextLevel == 25);
  CHECK(!rowOf(w, Control::AutoBright).pending);

  // The auto-off write was not ACKed: no level write; the read-back still says
  // auto on, so both rows stay as they are (Brightness re-pushed as "Auto").
  Policy u = pushedRows(true, 64, false, magic);
  noteJobStarted(u, 0);
  r = brightResult(WearStage::AutoOffUnacked, true, 25, true, true, 15);
  CHECK(afterApply(u, Control::Brightness, r) == After::Push);
  CHECK(rowOf(u, Control::Brightness).next == Shown::On && rowOf(u, Control::Brightness).pending);
  CHECK(rowOf(u, Control::AutoBright).next == Shown::On && !rowOf(u, Control::AutoBright).pending);
  // ... or it went off without an ACK: the rows show that.
  Policy u2 = pushedRows(true, 64, false, magic);
  noteJobStarted(u2, 0);
  r = brightResult(WearStage::AutoOffUnacked, true, 25, true, false, 15);
  CHECK(afterApply(u2, Control::Brightness, r) == After::Push);
  CHECK(rowOf(u2, Control::Brightness).next == Shown::Off && rowOf(u2, Control::Brightness).nextLevel == 15);
  CHECK(rowOf(u2, Control::AutoBright).next == Shown::Off && rowOf(u2, Control::AutoBright).pending);

  // Read-back mismatch after the recheck (level 60 still): the row shows the read value.
  Policy m = pushedRows(false, 50, false, magic);
  noteJobStarted(m, 0);
  r = brightResult(WearStage::Done, false, 75, true, false, 60);
  r.rechecked = true;
  CHECK(!brightnessConfirmed(r));
  CHECK(afterApply(m, Control::Brightness, r) == After::Push && rowOf(m, Control::Brightness).nextLevel == 60);
  // Read-back unanswered: read every row again.
  Policy n = pushedRows(true, 64, false, magic);
  noteJobStarted(n, 0);
  r = brightResult(WearStage::Done, true, 25, false, false, -1);
  CHECK(afterApply(n, Control::Brightness, r) == After::ReadThenPush && !anyPending(n));
  // Auto turned off but the read-back has no level: Brightness "?", Auto Brightness Off.
  Policy k = pushedRows(true, 64, false, magic);
  noteJobStarted(k, 0);
  r = brightResult(WearStage::Done, true, 25, true, false, -1);
  CHECK(!r.known);
  CHECK(afterApply(k, Control::Brightness, r) == After::ReadThenPush);

  // An Auto Brightness tick refreshes the Brightness row: off -> the level
  // auto brightness last chose (owner, 2026-11-21: "Brightness: 15%"); on -> "Auto".
  Policy ab = pushedRows(true, 15, false, magic);
  noteJobStarted(ab, 0);
  WearResult x{};
  x.stage = WearStage::Done;
  x.readOk = x.known = true;
  x.on = false;
  x.levelKnown = true;
  x.level = 15;
  CHECK(afterApply(ab, Control::AutoBright, x) == After::Push);
  CHECK(rowOf(ab, Control::AutoBright).next == Shown::Off && rowOf(ab, Control::AutoBright).pending);
  CHECK(rowOf(ab, Control::Brightness).next == Shown::Off && rowOf(ab, Control::Brightness).nextLevel == 15 &&
        rowOf(ab, Control::Brightness).pending);
  CHECK(std::strcmp(rowTitle(Control::Brightness, Shown::Off, 15).text, "Brightness: 15%") == 0);
  pushAll(ab, magic);
  noteJobStarted(ab, 0);
  x.on = true;
  x.level = 64;
  CHECK(afterApply(ab, Control::AutoBright, x) == After::Push);
  CHECK(rowOf(ab, Control::Brightness).next == Shown::On && rowOf(ab, Control::Brightness).pending);
  // Auto off with the level unknown: Brightness "?", never a stale "Auto".
  pushAll(ab, magic);
  noteJobStarted(ab, 0);
  x.on = false;
  x.levelKnown = false;
  CHECK(afterApply(ab, Control::AutoBright, x) == After::Push);
  CHECK(rowOf(ab, Control::Brightness).next == Shown::Unknown && rowOf(ab, Control::Brightness).pending);
  // Already set (no write): the pre-read's level refreshes it too; unchanged and ACKed = no push.
  pushAll(ab, magic);
  Policy as = pushedRows(false, 40, false, magic);
  noteJobStarted(as, 0);
  x = WearResult{};
  x.stage = WearStage::AlreadySet;
  x.readOk = x.known = x.levelKnown = true;
  x.level = 40;
  CHECK(afterApply(as, Control::AutoBright, x) == After::Push);
  CHECK(!rowOf(as, Control::Brightness).pending && rowOf(as, Control::AutoBright).pending);

  // Head-up off / on: only its own row.
  Policy h = pushedRows(true, 64, true, magic);
  noteJobStarted(h, 0);
  x = WearResult{};
  x.stage = WearStage::Done;
  x.readOk = x.known = true;
  x.on = false;
  CHECK(afterApply(h, Control::HeadUp, x) == After::Push);
  CHECK(nextPendingRow(h, &c) && c == Control::HeadUp && rowOf(h, c).next == Shown::Off);
  notePushed(h, c, ++magic);
  CHECK(!anyPending(h));
  CHECK(std::strcmp(rowTitle(Control::HeadUp, rowOf(h, Control::HeadUp).shown).text, "Head-Up Display: Off") == 0);
}

// ---- the push (2026-11-21): one FULL_UPDATE of all five rows ---------------
static const std::vector<std::string> kWanted = {"Wear Detection: On", "Silent Mode: Off", "Auto Brightness: On",
                                                 "Head-Up Display: On", "Brightness: Auto"};

// Drives the push the way G2_QuickList.cpp's owner does: one pushStep per
// 50 ms lap, the 1 s retry gate after a failure, ACKs delivered as wire bytes
// (08 02 10 <magic> 22 04 08 01 10 05) through parseAck at the next lap. The
// lens shows the titles of the last FULL_UPDATE it took, in item order (it
// replaces the whole list, the Even app's items included).
struct PushSim {
  Policy p{};
  uint32_t now = 0xFFFFF000u;          // runs across the uint32 wrap
  uint8_t magic = 0;
  std::vector<std::string> lens = {"line c", "extranotetest"};   // the Even app's items
  std::vector<Bytes> sent;
  unsigned failSends = 0;              // the next N sends fail
  unsigned dropAcks = 0;               // the next N sends get no ACK
  unsigned failures = 0, timeouts = 0, acks = 0;
  bool gaveUp = false;
  uint32_t ackDelayMs = 0;             // each ACK arrives this long after its send
  struct QueuedAck { uint8_t magic; uint32_t dueMs; };
  std::vector<QueuedAck> ackQueue;
  void deliver() {
    std::vector<QueuedAck> later;
    for (const QueuedAck& q : ackQueue) {
      if ((int32_t)(now - q.dueMs) < 0) { later.push_back(q); continue; }
      const uint8_t m = q.magic;
      Bytes a = {0x08, 0x02, 0x10};
      putVarint(a, m);
      const Bytes tail = {0x22, 0x04, 0x08, 0x01, 0x10, 0x05};
      a.insert(a.end(), tail.begin(), tail.end());
      Ack ack{};
      CHECK(parseAck(a.data(), a.size(), ack));
      if (noteAck(p, ack)) acks++;
    }
    ackQueue = later;
  }
  bool fail() {
    failures++;
    retryAt = now;
    retryArmed = true;
    if (pushFailed(p, now)) gaveUp = true;
    return gaveUp;
  }
  uint32_t retryAt = 0;
  bool retryArmed = false;
  // Laps until Idle or a give-up.
  bool run(unsigned maxLaps = 400) {
    for (unsigned lap = 0; lap < maxLaps; ++lap) {
      now += 50;
      deliver();
      if (retryArmed && (uint32_t)(now - retryAt) < 1000) continue;
      retryArmed = false;
      switch (pushStep(p, now)) {
        case PushStep::Idle: return true;
        case PushStep::WaitAck: continue;
        case PushStep::AckTimeout:
          timeouts++;
          if (fail()) return true;
          continue;
        case PushStep::Send: break;
      }
      magic = nextMagic(magic);
      const Bytes b = rowsOf(magic, p);
      CHECK(validateFullUpdatePb(b.data(), b.size()));
      if (failSends) {
        failSends--;
        if (fail()) return true;
        continue;
      }
      pushSent(p, magic, now);
      sent.push_back(b);
      lens = decodeTitles(b);
      if (dropAcks) dropAcks--;
      else ackQueue.push_back(QueuedAck{magic, now + ackDelayMs});
    }
    CHECK(!"the push did not end");
    return false;
  }
};
// Connected: the read gave every row, one FULL_UPDATE went out and was ACKed.
static PushSim steadySim() {
  PushSim s;
  CHECK(afterRead(s.p, rd(true, true, true, true, true, false, true, true, true, true, -1), true) == After::Push);
  pushWant(s.p, PushReason::Connect);
  CHECK(s.run() && s.sent.size() == 1 && s.acks == 1 && s.lens == kWanted);
  CHECK(s.p.push.reason == PushReason::Connect && s.p.push.want == PushReason::None);
  s.sent.clear();
  s.acks = 0;
  return s;
}

static void testPush() {
  CHECK(std::strcmp(pushReasonName(PushReason::Tick), "tick") == 0);
  CHECK(std::strcmp(pushReasonName(PushReason::Resend), "re-read after a give-up") == 0);
  CHECK(std::strcmp(pushStepName(PushStep::AckTimeout), "ACK timeout") == 0);

  // A zeroed policy: nothing to do.
  {
    Policy p{};
    CHECK(pushStep(p, 0) == PushStep::Idle && pushAckWaitMs(p, 0) == UINT32_MAX);
  }

  // Connect: ONE FULL_UPDATE, the five rows at index 0..4; the Even app's items
  // are gone from the lens; one ACK covers every row.
  {
    PushSim s;
    CHECK(afterRead(s.p, rd(true, true, true, true, true, false, true, true, true, true, -1), true) == After::Push);
    pushWant(s.p, PushReason::Connect);
    CHECK(s.run());
    CHECK(s.sent.size() == 1 && s.acks == 1 && s.failures == 0 && s.lens == kWanted);
    const Bytes want = rows(1, "Wear Detection: On", "Silent Mode: Off", "Auto Brightness: On",
                            "Head-Up Display: On", "Brightness: Auto");
    CHECK(s.sent[0] == want && want.size() == 194);
    for (uint8_t i = 0; i < kRowCount; ++i) CHECK(s.p.row[i].acked && s.p.row[i].magic == 1 && !s.p.row[i].pending);
    CHECK(!s.p.push.awaitingAck && !s.p.staleLens);
  }

  // Every row "?" (an unanswered read): still the five rows, in order.
  {
    PushSim s;
    CHECK(afterRead(s.p, rd(false, false, false, false, false, false), true) == After::Push);
    CHECK(s.run() && s.sent.size() == 1);
    const std::vector<std::string> q = {"Wear Detection: ?", "Silent Mode: ?", "Auto Brightness: ?",
                                        "Head-Up Display: ?", "Brightness: ?"};
    CHECK(s.lens == q);
  }

  // A tick: exactly one FULL_UPDATE, the new title in its own place, the
  // others unchanged.
  {
    PushSim s = steadySim();
    noteJobStarted(s.p, 0);
    CHECK(afterApply(s.p, Control::HeadUp, res(WearStage::Done, true, false)) == After::Push);
    pushWant(s.p, PushReason::Tick);
    CHECK(s.run() && s.sent.size() == 1 && s.acks == 1);
    std::vector<std::string> want = kWanted;
    want[3] = "Head-Up Display: Off";
    CHECK(s.lens == want);
    CHECK(s.sent[0] == rows(2, kWanted[0].c_str(), kWanted[1].c_str(), kWanted[2].c_str(), "Head-Up Display: Off",
                            kWanted[4].c_str()));
    CHECK(s.p.push.reason == PushReason::Tick);
  }

  // A Brightness tick changes two rows (Auto Brightness off, Brightness 25%):
  // still one FULL_UPDATE.
  {
    PushSim s = steadySim();
    noteJobStarted(s.p, 0);
    CHECK(afterApply(s.p, Control::Brightness, brightResult(WearStage::Done, true, 25, true, false, 24)) ==
          After::Push);
    CHECK(s.run() && s.sent.size() == 1);
    std::vector<std::string> want = kWanted;
    want[2] = "Auto Brightness: Off";
    want[4] = "Brightness: 25%";
    CHECK(s.lens == want);
  }

  // A tick re-push (busy, rate-limited): the list again, unchanged.
  {
    PushSim s = steadySim();
    noteRepush(s.p, Control::HeadUp);
    pushWant(s.p, PushReason::Repush);
    CHECK(s.run() && s.sent.size() == 1 && s.lens == kWanted && s.p.push.reason == PushReason::Repush);
  }

  // A row that changes while the ACK is awaited goes out in the next
  // FULL_UPDATE, after that ACK.
  {
    PushSim s = steadySim();
    rowOf(s.p, Control::Silent).next = Shown::On;
    rowOf(s.p, Control::Silent).pending = true;
    CHECK(pushStep(s.p, s.now) == PushStep::Send);
    pushSent(s.p, 50, s.now);
    CHECK(pushAckWaitMs(s.p, s.now) == kPushAckWaitMs && pushAckWaitMs(s.p, s.now + 400) == 600);
    CHECK(noteChangePush(s.p, Control::Silent, false, true) == ChangeAction::Push);
    CHECK(pushStep(s.p, s.now + 100) == PushStep::WaitAck);   // one update in flight at a time
    CHECK(noteAck(s.p, Ack{50, true, 5}));
    CHECK(pushStep(s.p, s.now + 150) == PushStep::Send);
    s.magic = 50;
    CHECK(s.run() && s.sent.size() == 1 && s.lens == kWanted);   // "Silent Mode: Off" again
  }

  // A failed send: the list again after the retry gate.
  {
    PushSim s = steadySim();
    noteRepush(s.p, Control::Wear);
    s.failSends = 1;
    CHECK(s.run() && !s.gaveUp && s.failures == 1 && s.sent.size() == 1 && s.acks == 1);
    CHECK(s.p.push.failRun == 0 && s.lens == kWanted);
  }

  // An ACK that never comes: WaitAck until kPushAckWaitMs, then AckTimeout and
  // the list again with a new magic; the old magic's late ACK is then refused.
  {
    Policy p{};
    const uint32_t t0 = 0xFFFFFF00u;
    markUnknown(p, Control::Wear);
    pushWant(p, PushReason::Change);
    CHECK(pushStep(p, t0) == PushStep::Send);
    pushSent(p, 7, t0);
    CHECK(pushStep(p, t0 + 999) == PushStep::WaitAck);   // past the uint32 wrap
    CHECK(pushAckWaitMs(p, t0 + kPushAckWaitMs) == 0);
    CHECK(pushStep(p, t0 + kPushAckWaitMs) == PushStep::AckTimeout && !p.push.awaitingAck);
    CHECK(!pushFailed(p, t0 + kPushAckWaitMs) && p.push.failRun == 1);
    CHECK(p.push.resendDue && !anyPending(p) && pushWanted(p));        // the whole list goes again
    CHECK(!p.staleLens && !p.recovery.armed);                          // one failure: no resend schedule
    CHECK(pushStep(p, t0 + 2000) == PushStep::Send);
    pushSent(p, 8, t0 + 2000);
    CHECK(p.push.reason == PushReason::Change);                         // the resend keeps its reason
    CHECK(!noteAck(p, Ack{7, true, 5}));                                // the old update's late ACK
    CHECK(noteAck(p, Ack{8, true, 5}) && p.push.failRun == 0);
    CHECK(pushStep(p, t0 + 2100) == PushStep::Idle);
  }

  // An ACK that comes after its timeout but before the resend counts: the
  // resend is cancelled, so late ACKs never resend without end (review
  // 2026-11-21: they reset the failure run and every row stayed pending).
  {
    PushSim s = steadySim();
    noteRepush(s.p, Control::Wear);
    s.ackDelayMs = 1200;                 // after the 1 s timeout, inside the 1 s retry gate
    CHECK(s.run() && !s.gaveUp && s.sent.size() == 1 && s.timeouts == 1 && s.acks == 1);
    CHECK(!s.p.push.resendDue && !anyPending(s.p) && s.p.push.failRun == 0 && !s.p.staleLens);
    for (uint8_t i = 0; i < kRowCount; ++i) CHECK(s.p.row[i].acked);
    // Policy level: timeout, pushFailed, the late ACK, then nothing to send.
    Policy p{};
    markUnknown(p, Control::Wear);
    pushSent(p, 7, 0);
    CHECK(pushStep(p, kPushAckWaitMs) == PushStep::AckTimeout && !pushFailed(p, kPushAckWaitMs));
    CHECK(noteAck(p, Ack{7, true, 5}) && !p.push.resendDue && p.push.failRun == 0);
    CHECK(pushStep(p, kPushAckWaitMs + 1000) == PushStep::Idle);
    // A row that changed meanwhile still goes out (once).
    pushSent(p, 8, 0);
    CHECK(pushStep(p, kPushAckWaitMs) == PushStep::AckTimeout && !pushFailed(p, kPushAckWaitMs));
    CHECK(noteChangePush(p, Control::Silent, true, true) == ChangeAction::Push);
    CHECK(noteAck(p, Ack{8, true, 5}) && !p.push.resendDue);
    CHECK(pushStep(p, kPushAckWaitMs + 1000) == PushStep::Send && rowOf(p, Control::Silent).pending &&
          !rowOf(p, Control::Wear).pending);
  }
  // ACKs later than the resend: each is refused (an older magic), so the
  // failures add up and the list is given up after kPushTriesMax sends; the
  // last list's ACK, when it does come, clears the stale lens.
  {
    PushSim s = steadySim();
    noteRepush(s.p, Control::Wear);
    s.ackDelayMs = 2500;
    CHECK(s.run() && s.gaveUp && s.sent.size() == kPushTriesMax && s.timeouts == kPushTriesMax && s.acks == 0);
    CHECK(s.p.staleLens && !pushWanted(s.p) && s.p.recovery.armed);
    s.now += 2000;
    s.deliver();
    CHECK(s.acks == 1 && !s.p.staleLens && !s.p.recovery.armed && !pushWanted(s.p));
  }
  // The ACK wait starts when the send returned: a lap clock read before that
  // (the send blocked on the TX gate) is still a wait, not a timeout.
  {
    Policy p{};
    markUnknown(p, Control::Wear);
    pushSent(p, 9, 5000);
    CHECK(pushStep(p, 2000) == PushStep::WaitAck && pushAckWaitMs(p, 2000) == kPushAckWaitMs);
    CHECK(pushStep(p, 5999) == PushStep::WaitAck && pushStep(p, 6000) == PushStep::AckTimeout);
  }

  // kPushTriesMax failures in a row: given up, nothing pending, the lens
  // marked stale and a re-read scheduled 2 s later.
  {
    PushSim s = steadySim();
    noteRepush(s.p, Control::Wear);
    pushWant(s.p, PushReason::Repush);
    s.failSends = kPushTriesMax;
    CHECK(s.run() && s.gaveUp && s.failures == kPushTriesMax && s.sent.empty());
    CHECK(!anyPending(s.p) && s.p.push.failRun == 0 && s.p.push.want == PushReason::None);
    CHECK(pushStep(s.p, s.now) == PushStep::Idle);
    CHECK(s.p.staleLens && s.p.recovery.armed && recoveryWaitMs(s.p.recovery, s.now) == 2000);
    // Missing ACKs count the same.
    PushSim u = steadySim();
    noteRepush(u.p, Control::Wear);
    u.dropAcks = kPushTriesMax;
    CHECK(u.run() && u.gaveUp && u.timeouts == kPushTriesMax && u.sent.size() == kPushTriesMax);
    CHECK(u.p.staleLens && !rowOf(u.p, Control::Wear).acked);
    // An ACK resets the run.
    PushSim r = steadySim();
    noteRepush(r.p, Control::Wear);
    r.failSends = kPushTriesMax - 1;
    CHECK(r.run() && !r.gaveUp && r.sent.size() == 1 && r.p.push.failRun == 0);
  }

  // A give-up is retried by the re-read schedule: an unanswered re-read sends
  // nothing and backs off; an answered one sends the list again even when
  // nothing changed; once ACKed, a re-read that changes nothing sends nothing.
  {
    const RecoveryGate open = openGate();
    PushSim s = steadySim();
    CHECK(noteChangePush(s.p, Control::Silent, true, true) == ChangeAction::Push);
    pushWant(s.p, PushReason::Change);
    s.failSends = kPushTriesMax;
    CHECK(s.run() && s.gaveUp && s.lens == kWanted);   // the lens still shows Silent Mode: Off
    CHECK(s.p.staleLens && !anyPending(s.p) && !anyUnknown(s.p));
    // The resend is held during a voice session and waits behind other work.
    RecoveryGate g = open;
    g.busy = true;
    CHECK(decideRecovery(s.p, g, s.now + 2000) == RecoveryAction::Blocked);
    g = open;
    g.otherWork = true;
    CHECK(decideRecovery(s.p, g, s.now + 2000) == RecoveryAction::Wait);
    CHECK(decideRecovery(s.p, open, s.now + 1999) == RecoveryAction::Wait);
    // Unanswered: nothing pushed, the next step 5 s later.
    s.now += 2000;
    CHECK(decideRecovery(s.p, open, s.now) == RecoveryAction::Read);
    noteBackgroundJobStarted(s.p);
    recoveryTaken(s.p.recovery);
    CHECK(afterRecoveryRead(s.p, rd(false, false, false, false, false, false), true) == After::None);
    recoveryAfterRead(s.p, s.now);
    CHECK(!anyPending(s.p) && s.p.staleLens && s.p.recovery.armed && s.p.recovery.delayMs == 5000);
    // Answered (silent on, nothing else changed): the whole list goes out.
    s.now += 5000;
    CHECK(decideRecovery(s.p, open, s.now) == RecoveryAction::Read);
    noteBackgroundJobStarted(s.p);
    recoveryTaken(s.p.recovery);
    CHECK(afterRecoveryRead(s.p, rd(true, true, true, true, true, true, true, true, true, true, -1), true) ==
          After::Push);
    for (uint8_t i = 0; i < kRowCount; ++i) CHECK(s.p.row[i].pending);
    pushWant(s.p, PushReason::Resend);
    recoveryAfterRead(s.p, s.now);
    CHECK(!s.p.recovery.armed && s.p.recovery.step == 2);   // not reset until the ACK
    CHECK(s.run() && s.sent.size() == 1 && s.acks == 1);
    std::vector<std::string> want = kWanted;
    want[1] = "Silent Mode: On";
    CHECK(s.lens == want && s.p.push.reason == PushReason::Resend);
    CHECK(!s.p.staleLens && !s.p.recovery.armed && s.p.recovery.step == 0 && !anyPending(s.p));
    // A re-read that changes nothing now sends nothing.
    noteBackgroundJobStarted(s.p);
    CHECK(afterRecoveryRead(s.p, rd(true, true, true, true, true, true, true, true, true, true, -1), true) ==
          After::None);
    recoveryAfterRead(s.p, s.now);
    CHECK(!anyPending(s.p) && !s.p.recovery.armed);
    s.sent.clear();
    CHECK(s.run() && s.sent.empty());
  }

  // Give-ups that keep failing back off 2, 5, 15, 30, 60, 60 s.
  {
    PushSim f = steadySim();
    const uint32_t want[] = {2000, 5000, 15000, 30000, 60000, 60000};
    for (uint32_t w : want) {
      noteRepush(f.p, Control::Wear);
      f.failSends = kPushTriesMax;
      f.gaveUp = false;
      CHECK(f.run() && f.gaveUp && f.p.staleLens);
      CHECK(f.p.recovery.armed && f.p.recovery.delayMs == w);
      recoveryTaken(f.p.recovery);   // the re-read answers, its push fails again
    }
  }

  // Not the exact pair / firmware rejected: pushAbort drops everything; linkReset too.
  {
    PushSim a = steadySim();
    noteRepush(a.p, Control::Wear);
    a.failSends = kPushTriesMax;
    CHECK(a.run() && a.gaveUp && a.p.staleLens);
    pushAbort(a.p);
    CHECK(!a.p.staleLens && !anyPending(a.p) && !a.p.push.awaitingAck);
    Policy p{};
    markUnknown(p, Control::Silent);
    pushWant(p, PushReason::Tick);
    CHECK(pushStep(p, 0) == PushStep::Send);
    pushSent(p, 3, 0);
    pushAbort(p);
    CHECK(!p.push.awaitingAck && pushStep(p, 0) == PushStep::Idle && p.push.want == PushReason::None);
    markUnknown(p, Control::Silent);
    pushSent(p, 4, 0);
    linkReset(p);
    CHECK(!p.push.awaitingAck && !anyPending(p) && p.row[0].magic == 0 && pushStep(p, 0) == PushStep::Idle);
  }

  // The reason: the latest request, taken by the next update.
  {
    Policy p{};
    pushWant(p, PushReason::Change);
    pushWant(p, PushReason::Tick);
    markUnknown(p, Control::Wear);
    pushSent(p, 1, 0);
    CHECK(p.push.reason == PushReason::Tick && p.push.want == PushReason::None);
  }
}

int main() {
  testGoldens();
  testVarints();
  testEncoderRejects();
  testValidatorRejects();
  testAck();
  testTickReport();
  testDedup();
  testDecide();
  testAfter();
  testChangePush();
  testHelpers();
  testRecovery();
  testRecoveryRead();
  testBrightness();
  testBrightnessApply();
  testPush();
  if (gFailures) {
    std::fprintf(stderr, "g2_quicklist_core_tests: %d failure(s)\n", gFailures);
    return 1;
  }
  std::printf("g2_quicklist_core_tests: all passed\n");
  return 0;
}
