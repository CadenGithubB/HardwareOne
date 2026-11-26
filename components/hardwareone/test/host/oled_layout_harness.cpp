#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>
#include "OLED_Layout.h"
#include "HAL_Display_P4EyePixels.h"
#include "glcdfont.c"
using std::min;
using std::max;
static int canvasWidth = 128;
static int canvasHeight = 64;
// Portrait (oledrotation 90 and 270) is the 64 x 128 grid. The landscape loop
// leaves gHarnessPortrait false, so those frames stay 128 wide.
static bool gHarnessPortrait = false;
#define DISPLAY_WIDTH canvasWidth
#define DISPLAY_HEIGHT canvasHeight
#define SCREEN_WIDTH canvasWidth
#define SCREEN_HEIGHT canvasHeight
#define OLED_HEADER_HEIGHT (gHarnessPortrait ? oled_layout::portrait::kHeaderHeight : 10)
#define OLED_FOOTER_HEIGHT (gHarnessPortrait ? oled_layout::portrait::kFooterHeight : 10)
#define OLED_CONTENT_START_Y (gHarnessPortrait ? oled_layout::portrait::kContentY : 11)
#define OLED_CONTENT_HEIGHT (gHarnessPortrait \
  ? (canvasHeight - oled_layout::portrait::kHeaderHeight - oled_layout::portrait::kFooterHeight - oled_layout::kContentGap) \
  : (canvasHeight - 21))
#define DISPLAY_COLOR_WHITE 1
#define DISPLAY_COLOR_BLACK 0
#define OLED_SCROLL_MAX_ITEMS 32
bool oledPortrait() { return gHarnessPortrait; }
int oledContentBottom() {
  // geometryFor(): landscape ends two rows above the footer; portrait ends
  // three rows above it (the blank row over the footer rule).
  return gHarnessPortrait ? canvasHeight - oled_layout::portrait::kFooterHeight - 3
                          : OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT - 2;
}
int oledCols() { return SCREEN_WIDTH / 6; }
int oledVisibleRows(int rowHeight, int reservedHeight=0, int baselineRows=0) {
  return oled_layout::visibleRows(OLED_CONTENT_HEIGHT,rowHeight,reservedHeight,baselineRows);
}
struct Adafruit_SSD1306 {
  int widthPx, heightPx;
  std::vector<uint8_t> buffer;
  int x=0,y=0,color=1,background=-1,textSize=1;
  std::vector<int> textRows;
  Adafruit_SSD1306():widthPx(canvasWidth),heightPx(canvasHeight),buffer(canvasWidth*((canvasHeight+7)/8)){}
  void pixel(int px,int py,int value) {
    if(px<0 || px>=widthPx || py<0 || py>=heightPx) return;
    auto& byte=buffer[(py/8)*widthPx+px];
    if(value) byte|=1u<<(py&7); else byte&=~(1u<<(py&7));
  }
  void fillRect(int px,int py,int w,int h,int value) {
    for(int yy=py;yy<py+h;++yy) for(int xx=px;xx<px+w;++xx) pixel(xx,yy,value);
  }
  void drawRect(int px,int py,int w,int h,int value) {
    fillRect(px,py,w,1,value);fillRect(px,py+h-1,w,1,value);
    fillRect(px,py,1,h,value);fillRect(px+w-1,py,1,h,value);
  }
  void printf(const char* format,...) {
    char text[128];va_list args;va_start(args,format);
    vsnprintf(text,sizeof(text),format,args);va_end(args);print(text);
  }
  void drawFastVLine(int px,int py,int h,int value){fillRect(px,py,1,h,value);}
  void drawFastHLine(int px,int py,int w,int value){fillRect(px,py,w,1,value);}
  void setTextSize(int size){assert(size==1 || size==2);textSize=size;}
  void setTextColor(int value,int bg=-1){color=value;background=bg;}
  void setCursor(int px,int py){x=px;y=py;}
  void fillCircle(int cx,int cy,int r,int value){
    for(int yy=-r;yy<=r;++yy) for(int xx=-r;xx<=r;++xx)
      if(xx*xx+yy*yy<=r*r) pixel(cx+xx,cy+yy,value);
  }
  void drawCircle(int cx,int cy,int r,int value){
    for(int yy=-r;yy<=r;++yy) for(int xx=-r;xx<=r;++xx) {
      const int d=xx*xx+yy*yy;
      if(d<=r*r && d>=(r-1)*(r-1)) pixel(cx+xx,cy+yy,value);
    }
  }
  void print(const char* text) {
    textRows.push_back(y);
    const int step=6*textSize;
    const int pitch=8*textSize;
    for(;*text;++text) {
      auto ch=static_cast<unsigned char>(*text);
      if(ch=='\n'){x=0;y+=pitch;continue;}
      if(x+step>widthPx){x=0;y+=pitch;}
      for(int xx=0;xx<6;++xx) for(int yy=0;yy<8;++yy) {
        const bool on=xx<5 && ((font[ch*5+xx]>>yy)&1);
        if(!(on || background>=0)) continue;
        for(int sy=0;sy<textSize;++sy) for(int sx=0;sx<textSize;++sx)
          pixel(x+xx*textSize+sx,y+yy*textSize+sy,on?color:background);
      }
      x+=step;
    }
  }
  void println(const char* text=""){print(text);x=0;y+=8;}
};
struct OledFooterHints {};
void oledRenderFooter(Adafruit_SSD1306*, const OledFooterHints*){}
void oledDrawIcon(Adafruit_SSD1306*, const char*, int, int, int){}
// INSERT_TYPES
// The shared renderers draw every label through the production text field:
// its implementation is spliced here over the GFX double, and the canvas
// bridges drawPixel to its framebuffer.
#include "oled_text_field_gfx_double.h"
static int currentOledMode=0;
uint32_t millis();
// INSERT_TEXT_FIELD
struct CanvasGfx : Adafruit_GFX {
  Adafruit_SSD1306& c;
  explicit CanvasGfx(Adafruit_SSD1306& d):Adafruit_GFX(d.widthPx,d.heightPx),c(d){}
  void drawPixel(int16_t px,int16_t py,uint16_t v) override { c.pixel(px,py,v); }
};
void oledTextFieldDraw(Adafruit_SSD1306* d,int x,int y,int w,const char* t,uint16_t fg,uint16_t bg,bool a,const void* o) {
  if(!d) return;
  if(t && *t) d->textRows.push_back(y);
  CanvasGfx g(*d);
  oledTextFieldDraw(static_cast<Adafruit_GFX*>(&g),x,y,w,t,fg,bg,a,o);
}
// The shipping wrapper (OLED_Utils.cpp). Baseline trees are compiled without it.
#ifndef BASELINE_LAYOUT
#include "OLED_MessageText.h"
#endif
// INSERT_WRAP
// INSERT_HELPERS
// INSERT_BOOT
// INSERT_SETTING
// INSERT_CARDS
// INSERT_FUNCTIONS
static OledScrollState sMainScroll;
static bool sMainScrollInit=false;
// INSERT_MENU_INIT
// INSERT_MAP_PACK
// INSERT_ESPNOW_DEVICE_ROWS
static Adafruit_SSD1306* oledDisplay=nullptr;
static bool oledConnected=true;
static struct {bool up=false,down=false;} gNavEvents;
#define INPUT_BUTTON_A 1
#define INPUT_CHECK(buttons,mask) ((buttons)&(mask))
static uint32_t gNow=1000;
uint32_t millis(){return gNow;}
void oledMarkDirtyUntil(uint32_t){}
void oledMarkDirty(){}
// INSERT_PERF
static void chrome(Adafruit_SSD1306& d) {
  d.setCursor(0,0); d.print("Hardware One");
  d.fillRect(0,9,128,1,1);
  d.fillRect(0,canvasHeight-11,128,1,1);
  d.setCursor(0,canvasHeight-8); d.print("A:Select B:Back");
}
// Portrait chrome from the layout constants the firmware uses at 90 and 270:
// 10-cell title, power on the status row, footer chips instead of the
// landscape hint string. The list and the wrapped body are the shipping
// functions; this only paints the bars around them.
static void portraitChrome(Adafruit_SSD1306& d, const char* title, const char* hints = "A:Select B:Back") {
  namespace P = oled_layout::portrait;
  d.setTextColor(DISPLAY_COLOR_WHITE);
  d.setCursor(d.widthPx - 6 * 3, P::kStatusTextY);
  d.print("USB");
  char fitted[P::kTitleCells + 1];
  int ellipsisAt = -1;
  const int n = oled_layout::fitPortraitTitle("", 0, title, std::strlen(title), P::kTitleCells,
                                              false, fitted, ellipsisAt);
  for (int i = 0; i < n; ++i) {
    const int x = P::kTitleX + i * 6;
    if (i == ellipsisAt) {
      for (int dot = 0; dot < 3; ++dot) d.pixel(x + 2 * dot, P::kTitleY + 6, DISPLAY_COLOR_WHITE);
      continue;
    }
    char glyph[2] = {fitted[i], 0};
    d.setCursor(x, P::kTitleY);
    d.print(glyph);
  }
  d.drawFastHLine(0, P::kHeaderRuleY, d.widthPx, DISPLAY_COLOR_WHITE);
  d.drawFastHLine(0, P::kFooterRuleY, d.widthPx, DISPLAY_COLOR_WHITE);
  oled_layout::FooterChips chips;
  oled_layout::parseFooterHints(hints, chips);
  static const char letters[] = {'A', 'B', 'X', 'Y'};
  for (int slot = 0; slot < oled_layout::kSlotCount; ++slot) {
    if (!chips.verb[slot][0]) continue;
    const int cx = oled_layout::chipX(slot), cy = oled_layout::chipY(slot);
    d.fillRect(cx, cy, P::kChipW, P::kChipH, DISPLAY_COLOR_WHITE);
    d.setTextColor(DISPLAY_COLOR_BLACK);
    d.setCursor(cx + 1, cy + 1);
    char letter[2] = {letters[slot], 0};
    d.print(letter);
    d.setTextColor(DISPLAY_COLOR_WHITE);
    d.setCursor(cx + P::kChipW + 1, cy + 1);
    d.print(chips.verb[slot]);
  }
}
static void savePages(const uint8_t* pages,int width,int height,const std::string& path,bool panel) {
  std::ofstream out(path,std::ios::binary);
  assert(out);
  if(panel) {
    assert(width==128 && height==128);
    out<<"P5\n240 240\n255\n";
    std::vector<uint16_t> physical(240*240);
    p4eye_display::rasterize(pages,physical.data(),0,240,false);
    for(uint16_t px:physical) {
      assert(px==0 || px==0xffff);
      out.put(px?char(255):char(0));
    }
    return;
  }
  out<<"P5\n"<<width<<" "<<height<<"\n255\n";
  for(int yy=0;yy<height;++yy) for(int xx=0;xx<width;++xx) {
    const bool lit=((pages[(yy/8)*width+xx]>>(yy&7))&1)!=0;
    out.put(lit?char(255):char(0));
  }
}
static void save(const Adafruit_SSD1306& d,const std::string& path,bool panel=false) {
  savePages(d.buffer.data(),d.widthPx,panel?128:d.heightPx,path,panel);
}
// oleddump text: '#' lit, '.' dark, one pixel row per line. Blank lines are
// ignored. A "oleddump WxH rows A-B" header places the following rows at A,
// so the 31-row replies concatenate into one canvas. The 240 panel is the
// shipping rasterizer, and only a 128x128 canvas is a P4 source.
struct DumpCanvas { int w=0,h=0; std::vector<uint8_t> pages; };
static bool parseDumpHeader(const std::string& line,int& w,int& h,int& first,int& last) {
  const auto at=line.find("oleddump");
  if(at==std::string::npos) return false;
  return std::sscanf(line.c_str()+at,"oleddump %dx%d rows %d-%d",&w,&h,&first,&last)==4
    && w>0 && h>0 && first>=0 && last>=first;
}
static DumpCanvas parseOledDump(const std::string& text) {
  int declaredW=0,declaredH=0,cursor=0;
  std::vector<std::string> rows;
  std::string line;
  auto take=[&](std::string raw) {
    if(!raw.empty() && raw.back()=='\r') raw.pop_back();
    if(raw.empty()) return;
    int hw=0,hh=0,first=0,last=0;
    if(parseDumpHeader(raw,hw,hh,first,last)) {
      assert(!declaredW || (declaredW==hw && declaredH==hh));
      declaredW=hw; declaredH=hh; cursor=first;
      return;
    }
    for(char c:raw) if(c!='#' && c!='.') return;
    if(declaredW) assert(static_cast<int>(raw.size())==declaredW);
    if(static_cast<int>(rows.size())<=cursor) rows.resize(cursor+1);
    assert(rows[cursor].empty());
    rows[cursor]=raw;
    ++cursor;
  };
  for(char c:text) {
    if(c=='\n') { take(line); line.clear(); }
    else line.push_back(c);
  }
  if(!line.empty()) take(line);
  int width=declaredW;
  if(!width) for(const auto& row:rows) if(!row.empty()) { width=static_cast<int>(row.size()); break; }
  assert(width>0);
  for(const auto& row:rows) if(!row.empty()) assert(static_cast<int>(row.size())==width);
  const int height=declaredH?declaredH:static_cast<int>(rows.size());
  if(declaredH) assert(static_cast<int>(rows.size())<=height);
  DumpCanvas out;
  out.w=width; out.h=height;
  out.pages.assign(((height+7)/8)*width,0);
  for(int y=0;y<static_cast<int>(rows.size()) && y<height;++y) {
    if(rows[y].empty()) continue;
    for(int x=0;x<width;++x)
      if(rows[y][x]=='#') out.pages[(y/8)*width+x]|=static_cast<uint8_t>(1u<<(y&7));
  }
  return out;
}
static std::string emitOledDump(const uint8_t* pages,int w,int h) {
  std::string out;
  for(int first=0;first<h;) {
    const int rows=std::min(31,h-first);
    const int last=first+rows-1;
    char hdr[96];
    if(last+1<h) std::snprintf(hdr,sizeof hdr,"oleddump %dx%d rows %d-%d (next: oleddump %d)\n",w,h,first,last,last+1);
    else std::snprintf(hdr,sizeof hdr,"oleddump %dx%d rows %d-%d\n",w,h,first,last);
    out+=hdr;
    for(int y=first;y<=last;++y) {
      for(int x=0;x<w;++x) out+=((pages[(y/8)*w+x]>>(y&7))&1)?'#':'.';
      out+='\n';
    }
    first=last+1;
  }
  return out;
}
static const char* gSurveyDir = nullptr;
static std::string readAll(const std::string& path) {
  std::ifstream in(path,std::ios::binary);
  if(!in) return {};
  std::string data; char buf[4096];
  while(true) {
    in.read(buf,sizeof buf);
    const auto n=in.gcount();
    if(n>0) data.append(buf,static_cast<size_t>(n));
    if(!in) break;
  }
  return data;
}
#ifndef BASELINE_LAYOUT
// 90-degree survey. Each frame calls a production draw the screen actually
// uses (scroll, wrap, text field, hero, key/value, perf). A few screens
// replay that function's cursor and print sequence because the function
// itself pulls Arduino String, the console ring, or a device driver.
static void renderNinetySurvey() {
  if (!gSurveyDir) return;
  const std::string dir(gSurveyDir);
  auto saveAs = [&](const Adafruit_SSD1306& d, const char* name) {
    save(d, dir + "/" + name);
  };
  auto simple = [&](Adafruit_SSD1306& d, int startY, int selected, std::initializer_list<const char*> labels) {
    OledScrollState list;
    oledScrollInit(&list, nullptr, 4);
    for (const char* label : labels) oledScrollAddItem(&list, label, nullptr, true, nullptr);
    list.selectedIndex = selected;
    oledScrollRenderSimple(&d, &list, true, startY);
  };
  const char* body =
      "The glass preview wraps this sentence onto a second line of the body "
      "and then keeps going past the sixth stored line into rows the content area still has free.";

  canvasWidth = 128;
  canvasHeight = 64;
  gHarnessPortrait = false;
  {
    Adafruit_SSD1306 d;
    chrome(d);
    oledPrintWrapped(&d, 0, OLED_CONTENT_START_Y, body, 12, 8);
    saveAs(d, "00-body-long.pgm");
  }
  {
    Adafruit_SSD1306 d;
    oledDrawBootProgress(&d, "1.2.3", "Initializing...", 40);
    saveAs(d, "00-boot.pgm");
  }
  {
    Adafruit_SSD1306 d;
    chrome(d);
    int y = OLED_CONTENT_START_Y + 2;
    y = oledDrawLoginField(&d, y, ">User: _____", true);
    y = oledDrawLoginField(&d, y, " Pass: _____", false);
    oledDrawLoginField(&d, y, " [Login]", false);
    saveAs(d, "00-login.pgm");
  }
  {
    Adafruit_SSD1306 d;
    chrome(d);
    oledDrawSettingValue(&d, "Brightness", 40, 0, 100, false);
    saveAs(d, "00-settings.pgm");
  }

  canvasWidth = 64;
  canvasHeight = 128;
  gHarnessPortrait = true;
  gNow = 1000;

  auto framed = [&](const char* title, const char* name, auto&& draw) {
    Adafruit_SSD1306 d;
    portraitChrome(d, title);
    oledTextFieldReset();
    oledTextFieldFrameBegin();
    draw(d);
    oledTextFieldFrameEnd();
    saveAs(d, name);
  };

  framed("Menu", "90-menu-split.pgm", [&](Adafruit_SSD1306& d) {
    OledScrollState menu;
    oledScrollInit(&menu, nullptr, 4);
    oledScrollSetSplitPane(&menu, 68, 74, 32);
    const char* labels[] = {"System status >", "Wireless network >", "Bluetooth devices >",
                            "Automations >", "Transcription >", "File browser >"};
    for (const char* label : labels) oledScrollAddItem(&menu, label, nullptr, true, nullptr);
    menu.selectedIndex = 1;
    oledScrollRender(&d, &menu, true, true, nullptr);
  });
  framed("Sensors", "90-sensor-menu.pgm", [&](Adafruit_SSD1306& d) {
    OledScrollState menu;
    oledScrollInit(&menu, nullptr, 4);
    oledScrollSetSplitPane(&menu, 68, 74, 32);
    const char* labels[] = {"Distance sensor", "Thermal camera", "Microphone level", "Realtime clock"};
    for (const char* label : labels) oledScrollAddItem(&menu, label, nullptr, true, nullptr);
    menu.selectedIndex = 0;
    oledScrollRender(&d, &menu, true, true, nullptr);
  });
  framed("Power", "90-power.pgm", [&](Adafruit_SSD1306& d) {
    const int y = oledPrintStatus(&d, OLED_CONTENT_START_Y, "Performance: 240MHz");
    simple(d, y, 0, {"Sleep now", "CPU frequency", "Display timeout", "Wireless radio", "Deep sleep"});
  });
  framed("CPU", "90-power-cpu.pgm", [&](Adafruit_SSD1306& d) {
    simple(d, OLED_CONTENT_START_Y, 2,
           {"80 MHz", "160 MHz", "240 MHz performance", "Dynamic frequency"});
  });
  framed("WiFi", "90-network.pgm", [&](Adafruit_SSD1306& d) {
    const char* ssid = "HardwareOne-Guest";
    const char* rssi = "-70dBm";
    const int room = oledCols() - 1 - (int)std::strlen(rssi);
    char line[24];
    std::snprintf(line, sizeof line, "%.*s %s", room > 0 ? room : 0, ssid, rssi);
    d.setCursor(0, OLED_CONTENT_START_Y);
    d.print(line);
    simple(d, OLED_CONTENT_START_Y + 8, 1,
           {"Status", "Saved networks", "Scan for networks", "Add Network", "Remove Network", "Auto Start: ON"});
  });
  framed("WiFi status", "90-network-status.pgm", [&](Adafruit_SSD1306& d) {
    simple(d, OLED_CONTENT_START_Y, 0,
           {"SSID HardwareOne-Guest", "BSSID aa:bb:cc:dd:ee:ff", "RSSI -70 dBm", "IP 192.168.1.40", "Gateway 192.168.1.1"});
  });
  framed("Saved WiFi", "90-saved-networks.pgm", [&](Adafruit_SSD1306& d) {
    simple(d, OLED_CONTENT_START_Y, 0,
           {"HardwareOne-Guest", "Office-5GHz-Private", "Phone hotspot", "Cafe public wifi"});
  });
  framed("WiFi scan", "90-wifi-scan.pgm", [&](Adafruit_SSD1306& d) {
    simple(d, OLED_CONTENT_START_Y, 1,
           {"HardwareOne-Guest -42", "NeighborMesh-2G -81", "xfinitywifi -90", "Scanning again"});
  });
  framed("Web", "90-web.pgm", [&](Adafruit_SSD1306& d) {
    const int y = oledPrintStatus(&d, OLED_CONTENT_START_Y, "https://hardware.local");
    simple(d, y, 0, {"Server: ON", "Auto start: ON", "HTTPS certificate", "Clients: 3"});
  });
  framed("Speech", "90-speech-menu.pgm", [&](Adafruit_SSD1306& d) {
    const int y = oledPrintStatus(&d, OLED_CONTENT_START_Y, "SPEECH (wake!)");
    simple(d, y, 0, {"View Status", "Stop SR", "Models", "Settings"});
  });
  framed("Speech", "90-speech-status.pgm", [&](Adafruit_SSD1306& d) {
    int y = OLED_CONTENT_START_Y;
    auto line = [&](const char* text) {
      const auto g = oled_layout::geometryFor(SCREEN_WIDTH, SCREEN_HEIGHT);
      const int room = oled_layout::rowsInBand(g, y, 8);
      if (room < 1) return;
      y = oledPrintWrapped(&d, 0, y, text, room, 8);
    };
    line("SR: (await cat)");
    line(">Wireless network>living room north");
    line("Last: open the g.. 86%");
    line("W:12 C:340");
  });
  framed("Transcript", "90-transcription-home.pgm", [&](Adafruit_SSD1306& d) {
    const char* message = "Listening on the room microphone for a long utterance";
    const auto wrapped = oled_message::wrap(message, oledCols(), 3);
    for (int i = 0; i < wrapped.count; ++i) {
      d.setCursor(0, OLED_CONTENT_START_Y + i * 8);
      d.print(wrapped.text[i]);
    }
    const int reserved = wrapped.count * 8 + 2;
    simple(d, OLED_CONTENT_START_Y + reserved, 0,
           {"Stop session", "Recent live text", "Save next: ON", "Saved: internal", "Saved: SD card", "Mic: room array"});
  });
  framed("Files", "90-transcription-files.pgm", [&](Adafruit_SSD1306& d) {
    simple(d, OLED_CONTENT_START_Y, 1,
           {"2026-11-25-standup.txt", "weekly-planning-notes.txt", "voice-memo-kitchen.txt", "Next files >"});
  });
  framed("ESP-NOW", "90-espnow-devices.pgm", [&](Adafruit_SSD1306& d) {
    OledScrollState list;
    oledScrollInit(&list, "ESP-NOW Devices", 4);
    oledScrollAddItem(&list, "living-room-sensor", "Room E  -42 dBm", true, nullptr);
    oledScrollAddItem(&list, "garage-door-node", "Unpaired, last seen Tuesday", true, nullptr);
    oledScrollAddItem(&list, "ABCDEFGHIJ", "ten glyphs of ink", true, nullptr);
    oledScrollAddItem(&list, "porch-light", "asleep", true, nullptr);
    list.selectedIndex = 2;
    oledScrollRender(&d, &list, true, true, nullptr);
  });
  framed("G2", "90-bluetooth-g2.pgm", [&](Adafruit_SSD1306& d) {
    const int y = oledPrintStatus(&d, OLED_CONTENT_START_Y, "G2 connected");
    simple(d, y, 0, {"Status detail", "Disconnect client", "Advertise again", "Pair phone", "Forget bond"});
  });
  framed("R1", "90-bluetooth-r1.pgm", [&](Adafruit_SSD1306& d) {
    const int y = oledPrintStatus(&d, OLED_CONTENT_START_Y, "Ring battery 80%");
    simple(d, y, 0, {"Buttons", "Sleep the ring", "Firmware", "Unpair"});
  });
  framed("LED", "90-led.pgm", [&](Adafruit_SSD1306& d) {
    simple(d, OLED_CONTENT_START_Y, 1,
           {"Off", "Warm white steady", "Slow breathe blue", "Status rainbow", "Night dim red"});
  });
  framed("Logging", "90-logging.pgm", [&](Adafruit_SSD1306& d) {
    simple(d, OLED_CONTENT_START_Y, 0,
           {"Serial log: verbose", "File log: warnings", "ESP-NOW log: debug", "Clear the log file"});
  });
  framed("Models", "90-llm-home.pgm", [&](Adafruit_SSD1306& d) {
    simple(d, OLED_CONTENT_START_Y, 0,
           {"Ask the loaded model", "Switch model", "Prompt templates", "Conversation entities"});
  });
  framed("Models", "90-llm-models.pgm", [&](Adafruit_SSD1306& d) {
    simple(d, OLED_CONTENT_START_Y, 1,
           {"qwen2.5-0.5b-instruct", "llama-3.2-1b-q4", "Remote OpenAI", "Load from SD card"});
  });
  framed("Chat", "90-llm-chat.pgm", [&](Adafruit_SSD1306& d) {
    const int lines = std::max(1, (oledContentBottom() + 1 - OLED_CONTENT_START_Y) / 10);
    const int cols = std::min(21, oledCols()) - 1;
    const char* reply =
        "The porch light is already on and the garage door stayed shut overnight.";
    const auto wrapped = oled_message::wrap(reply, cols, lines);
    for (int i = 0; i < wrapped.count; ++i) {
      d.setCursor(0, OLED_CONTENT_START_Y + i * 10);
      d.print(wrapped.text[i]);
    }
    if (wrapped.count) {
      d.setCursor(cols * 6 + 2, OLED_CONTENT_START_Y + (wrapped.count - 1) * 10);
      d.print("v");
    }
  });
  framed("Files", "90-file-actions.pgm", [&](Adafruit_SSD1306& d) {
    simple(d, OLED_CONTENT_START_Y, 0,
           {"Open", "Send over ESP-NOW", "Delete permanently", "Rename this file", "Copy to SD card"});
  });
  framed("Health", "90-health-actions.pgm", [&](Adafruit_SSD1306& d) {
    simple(d, OLED_CONTENT_START_Y, 0,
           {"Heap graph", "CPU graph", "Restart the radio", "Capture a diagnostic"});
  });
  framed("Bond", "90-bond-picker.pgm", [&](Adafruit_SSD1306& d) {
    OledScrollState list;
    oledScrollInit(&list, "Paired remotes", 4);
    oledScrollAddItem(&list, "Kitchen remote", "last seen 2m ago", true, nullptr);
    oledScrollAddItem(&list, "Workshop clicker", "battery low, unpaired soon", true, nullptr);
    oledScrollAddItem(&list, "ABCDEFGHIJ", "field is 59 px", true, nullptr);
    list.selectedIndex = 1;
    oledScrollRender(&d, &list, true, true, nullptr);
  });
  framed("Message", "90-body-wrap.pgm", [&](Adafruit_SSD1306& d) {
    oledPrintWrapped(&d, 0, OLED_CONTENT_START_Y, body, 12, 8);
  });
  framed("Unavailable", "90-unavailable.pgm", [&](Adafruit_SSD1306& d) {
    const char* reason = "FM radio was left out of this build. Enable the tuner and flash again.";
    oledPrintWrapped(&d, 0, OLED_CONTENT_START_Y, reason, 4, 9);
  });
  {
    Adafruit_SSD1306 d;
    portraitChrome(d, "CPU");
    perfRenderData.cpuCount = perfRenderData.stkCount = 8;
    const char* names[] = {"wifiRadio", "mainLoop", "oledRender", "bluetooth",
                           "sensorHub", "fileWrite", "espnowRx", "logger"};
    for (int i = 0; i < 8; ++i) {
      std::snprintf(perfRenderData.cpuName[i], 11, "%s", names[i]);
      std::snprintf(perfRenderData.stkName[i], 11, "%s", names[i]);
      perfRenderData.cpuPct[i] = (uint8_t)(80 - i * 7);
      perfRenderData.stkFree[i] = i == 3 ? 128u : 4096u;
    }
    perfRenderData.lapsPerSec = 1200;
    perfRenderData.stalls5s = 3;
    perfRenderData.loopValid = true;
    perfRenderData.valid = true;
    oledDisplay = &d;
    sPerfPage = 0;
    sCpuScroll = 0;
    displayPerfStatsRendered();
    oledDisplay = nullptr;
    saveAs(d, "90-perf-cpu.pgm");
    Adafruit_SSD1306 stack;
    portraitChrome(stack, "Stack");
    oledDisplay = &stack;
    sPerfPage = 1;
    sStkScroll = 0;
    displayPerfStatsRendered();
    oledDisplay = nullptr;
    saveAs(stack, "90-perf-stack.pgm");
  }
  framed("System", "90-system-status.pgm", [&](Adafruit_SSD1306& d) {
    int y = OLED_CONTENT_START_Y;
    y = oledKvRow(&d, y, "Batt", "3.97V");
    y = oledKvRow(&d, y, "Level", "82%");
    y = oledKvRow(&d, y, "Power", "USB");
    y = oledKvRow(&d, y, "Radio", "ON");
    y = oledKvRow(&d, y, "WiFi", "HardwareOne-Guest");
    y = oledKvRow(&d, y, "Heap", "148K");
    oledKvRow(&d, y, "Up", "3h12m");
  });
  framed("Distance", "90-hero-sensor.pgm", [&](Adafruit_SSD1306& d) {
    oledHeroCard(&d, "Distance to the wall", "12345", "mm", false);
    oledMeter(&d, oled_layout::portrait::kHeroBandY, 12, 70, 100);
  });
  framed("Config", "90-settings.pgm", [&](Adafruit_SSD1306& d) {
    const int labelRight = SCREEN_WIDTH - 10;
    const char* names[] = {"Wireless radios", "Bluetooth devices", "Display and sleep",
                           "Speech models", "User accounts", "Logging targets"};
    const int lineHeight = 10;
    const int maxVisible = OLED_CONTENT_HEIGHT / lineHeight;
    int y = OLED_CONTENT_START_Y;
    for (int i = 0; i < maxVisible && i < 6; ++i) {
      const bool selected = i == 1;
      if (selected) d.fillRect(0, y, SCREEN_WIDTH - 3, 10, DISPLAY_COLOR_WHITE);
      oledTextFieldDraw(&d, 2, y + 1, labelRight - 2, names[i],
                        selected ? DISPLAY_COLOR_BLACK : DISPLAY_COLOR_WHITE, DISPLAY_COLOR_WHITE,
                        selected, names);
      y += lineHeight;
    }
  });
  framed("Brightness", "90-settings-edit.pgm", [&](Adafruit_SSD1306& d) {
    oledDrawSettingValue(&d, "Display and sleep timeout", 30, 0, 600, false);
  });
  {
    Adafruit_SSD1306 d;
    oledDrawBootProgress(&d, "1.2.3", "Initializing...", 40);
    saveAs(d, "90-boot.pgm");
  }
  framed("Automations", "90-automations.pgm", [&](Adafruit_SSD1306& d) {
    const int listWidth = SCREEN_WIDTH - 3;
    const int detailX = 1;
    const int itemHeight = 10;
    const int startY = oled_layout::portrait::kPaneListY;
    d.drawFastHLine(0, oled_layout::portrait::kPaneRuleY, SCREEN_WIDTH, DISPLAY_COLOR_WHITE);
    const char* names[] = {"Morning lights", "Garage door left open", "Porch light at dusk"};
    const char* triggers[] = {"daily 07:30", "on garage_door_open", "every 15m after dusk"};
    const int shown = (oledContentBottom() - startY + 2) / itemHeight;
    for (int i = 0; i < shown && i < 3; ++i) {
      const int y = startY + i * itemHeight;
      const bool selected = i == 1;
      if (selected) d.fillRect(0, y, listWidth, itemHeight - 1, DISPLAY_COLOR_WHITE);
      const int ink = selected ? DISPLAY_COLOR_BLACK : DISPLAY_COLOR_WHITE;
      if (selected) d.fillCircle(3, y + 4, 2, ink);
      else d.drawCircle(3, y + 4, 2, ink);
      oledTextFieldDraw(&d, 8, y + 1, listWidth - 9, names[i], ink, ink, selected, names + i);
    }
    const char* rest = triggers[1];
    int dy = OLED_CONTENT_START_Y + 2;
    for (int line = 0; line < 2 && rest && *rest; ++line) {
      char seg[11];
      while (*rest == ' ') ++rest;
      size_t take = std::strlen(rest);
      if (take > 10) {
        size_t space = 10;
        while (space > 0 && rest[space] != ' ') --space;
        take = space > 0 ? space : 10;
      }
      std::memcpy(seg, rest, take);
      seg[take] = '\0';
      rest += take;
      d.setTextColor(DISPLAY_COLOR_WHITE);
      d.setCursor(detailX, dy);
      d.print(seg);
      dy += 10;
    }
    d.setCursor(detailX, dy);
    d.print("ON");
    d.setCursor(detailX + 24, dy);
    d.print("12 cmds");
  });
  framed("Confirm", "90-confirm.pgm", [&](Adafruit_SSD1306& d) {
    namespace P = oled_layout::portrait;
    const int w = SCREEN_WIDTH;
    d.fillRect(0, P::kContentY, w, P::kContentBottom - P::kContentY + 1, DISPLAY_COLOR_BLACK);
    d.drawRect(0, P::kContentY, w, P::kContentBottom - P::kContentY + 1, DISPLAY_COLOR_WHITE);
    const int cols = oled_layout::cellsIn(w - 4);
    int y = P::kContentY + 4;
    auto printLines = [&](const char* text) {
      const auto lines = oled_message::wrap(text, cols, 3);
      for (int i = 0; i < lines.count; ++i) {
        d.setCursor(2, y);
        d.print(lines.text[i]);
        y += 9;
      }
    };
    printLines("Delete the wireless network and forget its password?");
    y += 4;
    printLines("This removes HardwareOne-Guest from the saved list on this device.");
    auto bar = [&](int by, const char* label, bool selected) {
      const int barX = 3, barW = w - 6, barH = 13;
      if (selected) {
        d.fillRect(barX, by, barW, barH, DISPLAY_COLOR_WHITE);
        d.setTextColor(DISPLAY_COLOR_BLACK);
      } else {
        d.drawRect(barX, by, barW, barH, DISPLAY_COLOR_WHITE);
        d.setTextColor(DISPLAY_COLOR_WHITE);
      }
      const int n = (int)std::strlen(label);
      d.setCursor(barX + (barW - oled_layout::textWidth(n)) / 2, by + 3);
      d.print(label);
    };
    bar(72, "Yes", true);
    bar(88, "No", false);
    d.setTextColor(DISPLAY_COLOR_WHITE);
  });
  framed("Toast", "90-toast.pgm", [&](Adafruit_SSD1306& d) {
    namespace P = oled_layout::portrait;
    const char* message =
        "This recording belongs to alice. Only alice or an administrator can stop it from this screen.";
    const int boxX = 1, boxW = SCREEN_WIDTH - 4;
    const int maxLines = (P::kContentBottom - P::kContentY - 8) / 8;
    const auto lines = oled_message::wrap(message, oled_layout::cellsIn(boxW - 6), maxLines);
    const int count = lines.count ? lines.count : 1;
    const int boxH = 8 + 8 * count;
    const int boxY = P::kContentBottom - boxH;
    d.fillRect(boxX, boxY, boxW, boxH, DISPLAY_COLOR_WHITE);
    d.setTextColor(DISPLAY_COLOR_BLACK);
    for (int row = 0; row < lines.count; ++row) {
      const int n = (int)std::strlen(lines.text[row]);
      d.setCursor(boxX + (boxW - oled_layout::textWidth(n)) / 2, boxY + 4 + row * 8);
      d.print(lines.text[row]);
    }
    d.setTextColor(DISPLAY_COLOR_WHITE);
  });
  framed("Login", "90-login.pgm", [&](Adafruit_SSD1306& d) {
    int y = OLED_CONTENT_START_Y + 2;
    y = oledDrawLoginField(&d, y, ">User: _____", true);
    y = oledDrawLoginField(&d, y, " Pass: _____", false);
    oledDrawLoginField(&d, y, " [Login]", false);
  });
  framed("CLI", "90-cli-result.pgm", [&](Adafruit_SSD1306& d) {
    int y = OLED_CONTENT_START_Y;
    const int cols = oledCols();
    const char* command = "> wifi join HardwareOne-Guest";
    char cmd[32];
    std::snprintf(cmd, sizeof cmd, "%.*s", cols, command);
    d.setCursor(0, y);
    d.print(cmd);
    y += 11;
    const char* status = "FAILED";
    const int badgeW = (int)std::strlen(status) * 6 + 3;
    d.fillRect(0, y - 1, badgeW, 10, DISPLAY_COLOR_WHITE);
    d.setTextColor(DISPLAY_COLOR_BLACK);
    d.setCursor(2, y);
    d.print(status);
    d.setTextColor(DISPLAY_COLOR_WHITE);
    y += 11;
    const char* result =
        "Association rejected. The passphrase does not match the saved network HardwareOne-Guest.";
    const int len = (int)std::strlen(result);
    int pos = 0, line = 0;
    const int kMaxLines = 6;
    while (pos < len && line < kMaxLines) {
      char slice[32];
      std::snprintf(slice, sizeof slice, "%.*s", cols, result + pos);
      d.setCursor(0, y);
      d.print(slice);
      y += 10;
      pos += cols;
      ++line;
    }
    if (pos < len) {
      d.setCursor(SCREEN_WIDTH - 6, y - 10);
      d.print("~");
    }
  });
  {
    Adafruit_SSD1306 d;
    const char* title = "Wireless setup wizard";
    const auto place = oled_layout::setupInfoPlace(oled_layout::geometryFor(SCREEN_WIDTH, SCREEN_HEIGHT));
    const int rowCells = oledCols();
    const char* ind = "2/6";
    const int indChars = (int)std::strlen(ind);
    d.setCursor(SCREEN_WIDTH - indChars * 6, place.titleY);
    d.print(ind);
    const int titleMax = rowCells - indChars - 1;
    char shown[32];
    std::snprintf(shown, sizeof shown, "%.*s.", titleMax - 1, title);
    d.setCursor(0, place.titleY);
    d.print(shown);
    d.drawFastHLine(0, place.ruleY, SCREEN_WIDTH, DISPLAY_COLOR_WHITE);
    const char* info =
        "Join the network you want this device to use. A long passphrase can be typed on the next page.";
    const int bodyRows = oled_layout::rowsInBand(oled_layout::geometryFor(SCREEN_WIDTH, SCREEN_HEIGHT), place.bodyY, 8);
    oledPrintWrapped(&d, 0, place.bodyY, info, bodyRows > 0 ? bodyRows : 1, 8);
    d.drawFastHLine(0, place.footerY, SCREEN_WIDTH, DISPLAY_COLOR_WHITE);
    d.setCursor(0, place.footerY + 2);
    d.print("A:Next B:Back");
    saveAs(d, "90-setup-info.pgm");
  }
  framed("Notices", "90-notifications.pgm", [&](Adafruit_SSD1306& d) {
    const char* messages[] = {
        "Association rejected by the access point",
        "Recording stopped by alice",
        "Garage door left open overnight",
        "Bluetooth client disconnected",
        "Heap dropped under the watermark",
        "Firmware check found no update",
        "ESP-NOW peer garage-door-node asleep",
        "Microphone capture overrun",
        "One more notice below the window"};
    const int startY = OLED_CONTENT_START_Y;
    const int lineHeight = 10;
    const int maxVisible = (oledContentBottom() + 1 - startY) / lineHeight;
    const int room = oledCols() - 3;
    for (int i = 0; i < maxVisible && i < 9; ++i) {
      const int y = startY + i * lineHeight;
      const bool selected = i == 0;
      if (selected) {
        d.fillRect(0, y, SCREEN_WIDTH - 10, lineHeight - 1, DISPLAY_COLOR_WHITE);
        d.setTextColor(DISPLAY_COLOR_BLACK);
      } else {
        d.setTextColor(DISPLAY_COLOR_WHITE);
      }
      char msg[20];
      std::snprintf(msg, sizeof msg, "%.*s", room, messages[i]);
      d.setCursor(6, y + 1);
      d.print("! ");
      d.print(msg);
    }
    d.setTextColor(DISPLAY_COLOR_WHITE);
    d.setCursor(SCREEN_WIDTH - 8, startY + (maxVisible - 1) * lineHeight);
    d.print("\x19");
  });
  {
    Adafruit_SSD1306 d;
    portraitChrome(d, "Bluetooth link", "A:Disconnect B:Advertise X:Settings Y:Unpair");
    saveAs(d, "90-footer-hints.pgm");
  }
  {
    Adafruit_SSD1306 d;
    portraitChrome(d, "Configuration and radios");
    saveAs(d, "90-title-fit.pgm");
  }
}
#endif

int main(int argc,char** argv) {
  if (argc > 1) gSurveyDir = argv[1];
  static_assert(oled_layout::visibleRows(43,10,1,4)==4);
  static_assert(oled_layout::visibleRows(43,8,10,3)==3);
  static_assert(oled_layout::visibleRows(43,8,0,4)==4);
  static_assert(oled_layout::visibleRows(107,10,1,4)==10);
  static_assert(oled_layout::visibleRows(107,8,10,3)==12);
  static_assert(oled_layout::visibleRows(107,8,0,4)==13);
  static_assert(oled_layout::visibleRows(107,16,10,3)==6);
  static_assert(oled_layout::visibleRows(1,10)==1);
  for(int height:{64,128}) {
    canvasHeight=height;
    sMainScrollInit=false;
    mainMenuEnsureInit();
    const int rows=sMainScroll.visibleLines;
#ifndef BASELINE_LAYOUT
    assert(rows==(height==64?4:10));
#endif
    for(int i=0;i<24;++i) oledScrollAddItem(&sMainScroll,"Menu item",nullptr,true,nullptr);
    for(int selected=0;selected<24;++selected) {
      sMainScroll.selectedIndex=selected;
      oledScrollClampSelection(&sMainScroll);
      assert(selected>=sMainScroll.scrollOffset && selected<sMainScroll.scrollOffset+rows);
      Adafruit_SSD1306 d;
      oledScrollRender(&d,&sMainScroll,true,true,nullptr);
      assert(d.textRows.size()>=static_cast<size_t>(rows));
      for(int row=0;row<rows;++row) assert(d.textRows[row]==13+row*10);
      for(int y:d.textRows) assert(y>=11 && y+8<=canvasHeight-11);
    }
    oledScrollPageUp(&sMainScroll);
    assert(sMainScroll.selectedIndex==23-rows);
    oledScrollPageDown(&sMainScroll);
    assert(sMainScroll.selectedIndex==23);
    oledScrollUp(&sMainScroll); assert(sMainScroll.selectedIndex==22);
    oledScrollDown(&sMainScroll); assert(sMainScroll.selectedIndex==23);

    sMainScroll.selectedIndex=0;sMainScroll.scrollOffset=0;
    Adafruit_SSD1306 menu;
    chrome(menu);oledScrollRender(&menu,&sMainScroll,true,true,nullptr);
    if(argc>1) {
      save(menu,std::string(argv[1])+"/menu-"+std::to_string(height)+".pgm");
      if(height==128) save(menu,std::string(argv[1])+"/p4-panel.pgm",true);
    }
    OledScrollState simple;
    oledScrollInit(&simple,nullptr,oledVisibleRows(8,10,3));
    for(int i=0;i<24;++i) oledScrollAddItem(&simple,"Transcript",nullptr,true,nullptr);
    for(int selected=0;selected<24;++selected) {
      simple.selectedIndex=selected;
      Adafruit_SSD1306 d;
      oledScrollRenderSimple(&d,&simple,true,21);
      for(int y:d.textRows) assert(y>=21 && y+8<=canvasHeight-11);
    }
    simple.selectedIndex=0;simple.scrollOffset=0;
    Adafruit_SSD1306 text;chrome(text);
    oledScrollRenderSimple(&text,&simple,true,21);
    if(argc>1) save(text,std::string(argv[1])+"/simple-"+std::to_string(height)+".pgm");

    // The performance monitor has a summary plus a task list on BOTH pages.
    perfRenderData={};perfRenderData.valid=true;perfRenderData.loopValid=true;
    perfRenderData.cpuCount=perfRenderData.stkCount=24;
    for(int i=0;i<24;++i) {
      snprintf(perfRenderData.cpuName[i],11,"task%d",i);
      snprintf(perfRenderData.stkName[i],11,"task%d",i);
      perfRenderData.cpuPct[i]=10;perfRenderData.stkFree[i]=1024;
    }
    [[maybe_unused]] const int perfRows=height==64?4:10;
#ifndef BASELINE_LAYOUT
    assert(PERF_VISIBLE_ROWS==perfRows);
#endif
    for(int page=0;page<2;++page) {
      sPerfPage=page;sCpuScroll=sStkScroll=0;
      Adafruit_SSD1306 d;oledDisplay=&d;
      displayPerfStatsRendered();
      #ifdef BASELINE_LAYOUT
      const int drawn=PERF_VISIBLE_ROWS;
#else
      const int drawn=perfRows;
#endif
      // CPU rows each print a name and percentage; stack rows print once.
      assert(d.textRows.size()==static_cast<size_t>(1+drawn*(page==0?2:1)+1));
      for(int row=0;row<drawn;++row) {
        const int index=1+row*(page==0?2:1);
        assert(d.textRows[index]==20+row*9);
        if(height==128) assert(d.textRows[index]+8<=height-11);
      }
      if(argc>1) {
        chrome(d);
        save(d,std::string(argv[1])+"/perf-"+(page==0?"cpu-":"stack-")+std::to_string(height)+".pgm");
      }
      gNavEvents.down=true;
      for(int i=0;i<24-drawn;++i) assert(perfStatsInputHandler(0,0,0));
      assert(!perfStatsInputHandler(0,0,0));
      int& offset=page==0?sCpuScroll:sStkScroll;
      assert(offset==24-drawn);
      // A shrinking sample must clamp an old scroll position before drawing.
      perfRenderData.cpuCount=perfRenderData.stkCount=2;
      displayPerfStatsRendered();assert(offset==0);
      perfRenderData.cpuCount=perfRenderData.stkCount=24;
      gNavEvents={};
    }
    oledDisplay=nullptr;

    // Overflowing labels through the shipping renderers, checked on pixels: a
    // lit pixel never leaves its field (the pane, the cursor column, the
    // scrollbar), unselected rows end in "...", and only the selected row
    // moves, at the shared marquee's pace.
    {
      auto litIn=[&](const Adafruit_SSD1306& d,int x0,int y0,int x1,int y1){
        int n=0;for(int yy=y0;yy<y1;++yy)for(int xx=x0;xx<x1;++xx)n+=((d.buffer[(yy/8)*128+xx]>>(yy&7))&1)!=0;return n;};
      const char* longLabel="A label far too long for any OLED row to hold";
      // Compact list with a scrollbar: label field is [12, 126).
      OledScrollState compact;oledScrollInit(&compact,nullptr,3);
      for(int i=0;i<6;++i) oledScrollAddItem(&compact,longLabel,nullptr,true,nullptr);
      compact.selectedIndex=1;
      gNow=5000;Adafruit_SSD1306 c0;oledTextFieldFrameBegin();oledScrollRenderSimple(&c0,&compact,true,11);oledTextFieldFrameEnd();
      // The same list with empty labels draws only the cursor and the scrollbar:
      // the long labels must leave both column ranges exactly as they are.
      OledScrollState bare;oledScrollInit(&bare,nullptr,3);
      for(int i=0;i<6;++i) oledScrollAddItem(&bare,"",nullptr,true,nullptr);
      bare.selectedIndex=1;
      Adafruit_SSD1306 cb;oledTextFieldFrameBegin();oledScrollRenderSimple(&cb,&bare,true,11);oledTextFieldFrameEnd();
      for(int yy=11;yy<35;++yy){
        for(int xx=0;xx<12;++xx) assert(((c0.buffer[(yy/8)*128+xx]>>(yy&7))&1)==((cb.buffer[(yy/8)*128+xx]>>(yy&7))&1));
        for(int xx=126;xx<128;++xx) assert(((c0.buffer[(yy/8)*128+xx]>>(yy&7))&1)==((cb.buffer[(yy/8)*128+xx]>>(yy&7))&1));
      }
      assert(litIn(c0,12,11,126,35)>litIn(cb,12,11,126,35));                 // the labels are drawn in between
      oledTextFieldReset();gNow=5000;c0=Adafruit_SSD1306();oledTextFieldFrameBegin();oledScrollRenderSimple(&c0,&compact,true,11);oledTextFieldFrameEnd();
      // Unselected rows end in the ellipsis: the last three cells are dots.
      Adafruit_SSD1306 dots;dots.setCursor(108,11);dots.print("...");
      for(int xx=108;xx<126;++xx)for(int yy=11;yy<19;++yy) assert((((c0.buffer[(yy/8)*128+xx]>>(yy&7))&1)!=0)==(((dots.buffer[(yy/8)*128+xx]>>(yy&7))&1)!=0));
      // The selected row has not moved before the initial pause, then moves.
      gNow=5999;assert(!oledTextFieldNeedsFrame());
      gNow=6000+50*4;assert(oledTextFieldNeedsFrame());
      Adafruit_SSD1306 c1;oledTextFieldFrameBegin();oledScrollRenderSimple(&c1,&compact,true,11);oledTextFieldFrameEnd();
      auto rowSame=[&](const Adafruit_SSD1306& a,const Adafruit_SSD1306& b,int y){
        for(int yy=y;yy<y+8;++yy)for(int xx=12;xx<126;++xx)
          if((((a.buffer[(yy/8)*128+xx]>>(yy&7))&1))!=(((b.buffer[(yy/8)*128+xx]>>(yy&7))&1)))return false;
        return true;};
      for(int row=0;row<3;++row) assert(rowSame(c0,c1,11+row*8)==(row!=1));   // only the selected row changed
      for(int yy=11;yy<35;++yy){                                             // cursor and scrollbar stayed put
        for(int xx=0;xx<12;++xx) assert(((c1.buffer[(yy/8)*128+xx]>>(yy&7))&1)==((cb.buffer[(yy/8)*128+xx]>>(yy&7))&1));
        for(int xx=126;xx<128;++xx) assert(((c1.buffer[(yy/8)*128+xx]>>(yy&7))&1)==((cb.buffer[(yy/8)*128+xx]>>(yy&7))&1));
      }
      // Split pane (the main menu's 68 px list): nothing crosses the separator.
      OledScrollState split;oledScrollInit(&split,nullptr,4);oledScrollSetSplitPane(&split,68,74,32);
      for(int i=0;i<3;++i) oledScrollAddItem(&split,longLabel,nullptr,true,nullptr);
      split.selectedIndex=0;
      Adafruit_SSD1306 s0;oledTextFieldFrameBegin();oledScrollRender(&s0,&split,true,true,nullptr);oledTextFieldFrameEnd();
      assert(litIn(s0,67,11,74,11+OLED_CONTENT_HEIGHT)==0);                   // between the bar and the separator
      assert(litIn(s0,75,11,128,11+OLED_CONTENT_HEIGHT)==0);                   // the icon pane (no icon here)
      assert(litIn(s0,2,13,67,21)>0);                                         // the selected row's glyphs are there
      // Two-line full-width rows (items start at the content top, y=11):
      // each declared line is bounded, the selected item's two long lines
      // both draw, and nothing spills into a third line below the window.
      OledScrollState two;oledScrollInit(&two,nullptr,2);
      oledScrollAddItem(&two,"short","and short",true,nullptr);
      oledScrollAddItem(&two,longLabel,longLabel,true,nullptr);
      oledScrollAddItem(&two,"third","row",true,nullptr);
      two.selectedIndex=1;
      Adafruit_SSD1306 t0;oledTextFieldFrameBegin();oledScrollRender(&t0,&two,true,true,nullptr);oledTextFieldFrameEnd();
      assert(litIn(t0,0,0,128,11)==0);                                         // nothing under the header
      assert(litIn(t0,0,27,3,43)>0);                                           // selection bar on the second item
      assert(litIn(t0,5,27,126,35)>0 && litIn(t0,5,35,126,43)>0);              // both lines drawn
      assert(litIn(t0,0,43,126,59)==0);                                        // no third line, nothing below the window
#ifndef BASELINE_LAYOUT
      // The titled full-width list (the ESP-NOW device list, its production
      // row count): the title on the content's first line, every item's two
      // lines between it and the footer, at both panel heights; the selected
      // item is always one of them.
      {
        const int rows=espnowDeviceListRows();
        assert(rows==(height==64?2:6));
        OledScrollState list;oledScrollInit(&list,"ESP-NOW Devices",rows);
        for(int i=0;i<9;++i) oledScrollAddItem(&list,"+ peer","Room E",true,nullptr);
        for(int selected=0;selected<9;++selected) {
          list.selectedIndex=selected;oledScrollClampSelection(&list);
          assert(selected>=list.scrollOffset && selected<list.scrollOffset+rows);
          Adafruit_SSD1306 d;oledTextFieldFrameBegin();oledScrollRender(&d,&list,true,true,nullptr);oledTextFieldFrameEnd();
          assert(litIn(d,0,0,128,11)==0);                                      // the title is not under the header
          assert(litIn(d,0,11,100,19)>0);                                      // the title line
          assert(litIn(d,0,canvasHeight-11,128,canvasHeight)==0);              // nothing in the footer band
          assert(litIn(d,0,21+(selected-list.scrollOffset)*16,3,37+(selected-list.scrollOffset)*16)>0);  // its bar
        }
      }
#endif
      if(argc>1){
        Adafruit_SSD1306 pc;chrome(pc);oledScrollRenderSimple(&pc,&compact,true,11);
        save(pc,std::string(argv[1])+"/overflow-compact-"+std::to_string(height)+".pgm");
        gNow+=1000+50*30;Adafruit_SSD1306 pm;chrome(pm);oledTextFieldFrameBegin();oledScrollRenderSimple(&pm,&compact,true,11);oledTextFieldFrameEnd();
        save(pm,std::string(argv[1])+"/overflow-compact-moved-"+std::to_string(height)+".pgm");
        Adafruit_SSD1306 ps;chrome(ps);oledScrollRender(&ps,&split,true,true,nullptr);
        save(ps,std::string(argv[1])+"/overflow-split-"+std::to_string(height)+".pgm");
      }
      oledTextFieldReset();gNow=1000;
    }

#ifndef BASELINE_LAYOUT
    // Host screen mirror: one wrapped body at both canvas heights. The 128
    // frame is also the 240 panel. Those pixels are written as an oleddump
    // capture and read back, so the capture path does not need a board.
    {
      static const char kBody[] =
        "The glass preview wraps this sentence onto a second line of the body.";
      const auto wrapped = oled_message::wrap(kBody, oled_layout::cellsIn(SCREEN_WIDTH), 6);
      assert(wrapped.count == 4);
      assert(std::strcmp(wrapped.text[0], "The glass preview") == 0);
      assert(std::strcmp(wrapped.text[1], "wraps this sentence") == 0);
      assert(std::strcmp(wrapped.text[2], "onto a second line of") == 0);
      assert(std::strcmp(wrapped.text[3], "the body.") == 0);
      Adafruit_SSD1306 body;
      chrome(body);
      const int yEnd = oledPrintWrapped(&body, 0, OLED_CONTENT_START_Y, kBody, 6, 8);
      assert(yEnd == OLED_CONTENT_START_Y + wrapped.count * 8);
      assert(OLED_CONTENT_START_Y + (wrapped.count - 1) * 8 + 8 <= canvasHeight - 11);
      auto litAt = [](const Adafruit_SSD1306& d, int x, int y) {
        return ((d.buffer[(y / 8) * 128 + x] >> (y & 7)) & 1) != 0;
      };
      // A 21-cell clip would keep drawing into "wra". Word wrap stops after
      // "preview", so cells 17-20 of the first body row stay dark.
      for (int y = 11; y < 19; ++y)
        for (int x = 17 * 6; x < 21 * 6; ++x) assert(!litAt(body, x, y));
      Adafruit_SSD1306 ref;
      ref.setCursor(0, 19);
      ref.print(wrapped.text[1]);
      for (int y = 19; y < 27; ++y)
        for (int x = 0; x < 128; ++x) assert(litAt(body, x, y) == litAt(ref, x, y));
      const std::string dumped = emitOledDump(body.buffer.data(), 128, canvasHeight);
      const DumpCanvas back = parseOledDump(dumped);
      assert(back.w == 128 && back.h == canvasHeight);
      assert(std::memcmp(back.pages.data(), body.buffer.data(), body.buffer.size()) == 0);
      if (argc > 1) {
        const std::string dir(argv[1]);
        save(body, dir + "/body-" + std::to_string(height) + ".pgm");
        if (height == 128) {
          save(body, dir + "/body-p4-panel.pgm", true);
          savePages(back.pages.data(), 128, 128, dir + "/capture-p4-panel.pgm", true);
          std::ofstream dump(dir + "/body-128.oleddump.txt");
          assert(dump);
          dump << dumped;
        }
      }
    }
#endif

    // Exercise the shipping map packer over both sizes, including the last row.
    std::vector<uint8_t> shades(128*height), packed(128*((height+7)/8),0xaa);
    shades.back()=15; shades[64*128-1]=1;
#ifdef BASELINE_LAYOUT
    packShadeBufTo1Bit(shades.data(),packed.data());
#else
    packShadeBufTo1Bit(shades.data(),packed.data(),128,height);
#endif
    assert(packed.back()==0x80);
    for(int yy=0;yy<height;++yy) for(int xx=0;xx<128;++xx)
      assert(((packed[(yy/8)*128+xx]>>(yy&7))&1)==(shades[yy*128+xx]!=0));
  }
#ifndef BASELINE_LAYOUT
  if (canvasHeight == 64) {
    // A list set up while the screen was portrait (oledVisibleRows gave a
    // 64 x 128 count) and then drawn in landscape gets the rows that fit, with
    // the selection kept in view (booting at 90, then oledrotation 0).
    static const char* names[10]={"a","b","c","d","e","f","g","h","i","j"};
    gHarnessPortrait=true;
    OledScrollState simple;oledScrollInit(&simple,nullptr,10);
    OledScrollState split;oledScrollInit(&split,nullptr,8);
    gHarnessPortrait=false;
    oledScrollSetSplitPane(&split,68,74,32);
    for(auto* n:names){oledScrollAddItem(&simple,n,nullptr,true,nullptr);oledScrollAddItem(&split,n,nullptr,true,nullptr);}
    simple.selectedIndex=7;split.selectedIndex=7;
    Adafruit_SSD1306 a;oledScrollRenderSimple(&a,&simple,true,OLED_CONTENT_START_Y);
    assert(simple.visibleLines==5 && simple.scrollOffset==3);
    Adafruit_SSD1306 b;oledScrollRender(&b,&split,true,true,nullptr);
    assert(split.visibleLines==4 && split.scrollOffset==4);
    // A list set up in landscape keeps its own count.
    OledScrollState kept;oledScrollInit(&kept,nullptr,3);
    for(auto* n:names) oledScrollAddItem(&kept,n,nullptr,true,nullptr);
    Adafruit_SSD1306 c;oledScrollRenderSimple(&c,&kept,true,OLED_CONTENT_START_Y);
    assert(kept.visibleLines==3);
    oledTextFieldReset();
  }
  {
    // A portrait raster (64 x 128 logical, oledrotation 90/270) packs into the
    // same 1024 bytes in its own pages: 16 pages of 64 columns.
    std::vector<uint8_t> shades(64*128), packed(1024,0xaa);
    shades[0]=3; shades[127*64+63]=1; shades[70*64+5]=9;
    packShadeBufTo1Bit(shades.data(),packed.data(),64,128);
    for(int yy=0;yy<128;++yy) for(int xx=0;xx<64;++xx)
      assert(((packed[(yy/8)*64+xx]>>(yy&7))&1)==(shades[yy*64+xx]!=0));
    assert(packed[1023]==0x80 && packed[0]==0x01);
  }
  {
    // Slim canvas shared by oledrotation 90 and 270: GFX swaps the 128 x 64
    // panel to 64 x 128, and geometryFor builds one portrait grid either way.
    const auto geo = oled_layout::geometryFor(64, 128);
    assert(geo.portrait && geo.width == 64 && geo.height == 128 && geo.cols == 10);
    assert(geo.headerHeight == 20 && geo.footerHeight == 20);
    assert(geo.contentY == 21 && geo.contentBottom == 105 && geo.contentHeight == 87);
    canvasWidth = 64;
    canvasHeight = 128;
    gHarnessPortrait = true;
    assert(SCREEN_WIDTH == 64 && oledCols() == 10);
    assert(OLED_CONTENT_START_Y == 21 && oledContentBottom() == 105);
    static const char kBody[] =
      "The glass preview wraps this sentence onto a second line of the body.";
    const int cols = oled_layout::cellsIn(SCREEN_WIDTH);
    assert(cols == 10);
    const auto wrapped = oled_message::wrap(kBody, cols, 6);
    assert(wrapped.count == 6);
    assert(std::strcmp(wrapped.text[0], "The glass") == 0);
    assert(std::strcmp(wrapped.text[1], "preview") == 0);
    assert(std::strcmp(wrapped.text[2], "wraps this") == 0);
    assert(std::strcmp(wrapped.text[3], "sentence") == 0);
    assert(std::strcmp(wrapped.text[4], "onto a") == 0);
    assert(std::strcmp(wrapped.text[5], "sec...") == 0);
    // A 10-glyph label's ink is 59 px (the portrait field width) but the text
    // field counts 6 px per glyph, so glyphsThatFit(59) is 9 and the label
    // is ellipsized.
    assert(oled_layout::portrait::kListSelectW - oled_layout::portrait::kTextX - 1 == 59);
    assert(oled_layout::textWidth(10) == 59);
    assert(oled_text::textWidthPx(10) == 60);
    assert(oled_text::glyphsThatFit(59) == 9);
    char shown[32];
    assert(oled_text::ellipsize("ABCDEFGHIJ", 10, 9, shown, sizeof shown) == 9);
    assert(std::strcmp(shown, "ABCDEF...") == 0);
    Adafruit_SSD1306 body;
    portraitChrome(body, "Configuration");
    const int yEnd = oledPrintWrapped(&body, 0, OLED_CONTENT_START_Y, kBody, 6, 8);
    assert(yEnd == OLED_CONTENT_START_Y + 6 * 8);
    oledTextFieldReset();
    gNow = 1000;
    static const char* labels[] = {
      "Sensors", "ABCDEFGHI", "ABCDEFGHIJ", "Wireless network", "Bluetooth link", "GPS position"};
    OledScrollState menu;
    oledScrollInit(&menu, nullptr, 4);
    for (const char* label : labels) oledScrollAddItem(&menu, label, "detail", true, nullptr);
    menu.selectedIndex = 0;
    Adafruit_SSD1306 list;
    portraitChrome(list, "Configuration");
    oledTextFieldFrameBegin();
    oledScrollRender(&list, &menu, true, true, nullptr);
    oledTextFieldFrameEnd();
    assert(menu.visibleLines == 5);
    auto rowMatches = [](const Adafruit_SSD1306& d, int y, const char* text) {
      Adafruit_SSD1306 ref;
      ref.setCursor(oled_layout::portrait::kTextX, y);
      ref.print(text);
      for (int yy = y; yy < y + 8; ++yy)
        for (int xx = 0; xx < 60; ++xx)
          if (((d.buffer[(yy / 8) * d.widthPx + xx] >> (yy & 7)) & 1) !=
              ((ref.buffer[(yy / 8) * ref.widthPx + xx] >> (yy & 7)) & 1))
            return false;
      return true;
    };
    const int row1 = OLED_CONTENT_START_Y + 16;
    const int row2 = OLED_CONTENT_START_Y + 32;
    const int row3 = OLED_CONTENT_START_Y + 48;
    assert(rowMatches(list, row1, "ABCDEFGHI"));
    assert(rowMatches(list, row2, "ABCDEF..."));
    assert(rowMatches(list, row3, "Wirele..."));
    if (argc > 1) {
      const std::string dir(argv[1]);
      save(body, dir + "/body-portrait-64x128.pgm");
      save(list, dir + "/menu-portrait-64x128.pgm");
    }
    renderNinetySurvey();
    canvasWidth = 128;
    canvasHeight = 64;
    gHarnessPortrait = false;
  }
#endif
  if (argc > 2) {
    std::ifstream in(argv[2], std::ios::binary);
    if (!in) {
      std::fprintf(stderr, "oleddump file missing: %s\n", argv[2]);
      return 1;
    }
    in.close();
    const DumpCanvas dump = parseOledDump(readAll(argv[2]));
    assert(dump.w > 0 && dump.h > 0);
    const std::string dir(argv[1]);
    savePages(dump.pages.data(), dump.w, dump.h, dir + "/oleddump-logical.pgm", false);
    if (dump.w == 128 && dump.h == 128)
      savePages(dump.pages.data(), 128, 128, dir + "/oleddump-p4-panel.pgm", true);
  }
  std::puts("OLED/P4 layout, performance-monitor rows/scrolling, map packing and render bounds passed");
}
