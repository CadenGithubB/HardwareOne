#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include "OLED_Layout.h"
#include "HAL_Display_P4EyePixels.h"
#include "glcdfont.c"
using std::min;
using std::max;
static int canvasHeight = 64;
#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT canvasHeight
#define SCREEN_WIDTH 128
#define OLED_CONTENT_START_Y 11
#define OLED_CONTENT_HEIGHT (canvasHeight - 21)
#define DISPLAY_COLOR_WHITE 1
#define DISPLAY_COLOR_BLACK 0
#define OLED_SCROLL_MAX_ITEMS 32
// Landscape here: the portrait branches of the shared renderers are not taken.
static bool gHarnessPortrait = false;  // set only while a list is set up "in portrait"
bool oledPortrait() { return gHarnessPortrait; }
int oledContentBottom() { return OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT - 2; }
int oledCols() { return SCREEN_WIDTH / 6; }
int oledVisibleRows(int rowHeight, int reservedHeight=0, int baselineRows=0) {
  return oled_layout::visibleRows(OLED_CONTENT_HEIGHT,rowHeight,reservedHeight,baselineRows);
}
struct Adafruit_SSD1306 {
  std::vector<uint8_t> buffer;
  int x=0,y=0,color=1,background=-1;
  std::vector<int> textRows;
  Adafruit_SSD1306():buffer(128*((canvasHeight+7)/8)){}
  void pixel(int px,int py,int value) {
    if(px<0 || px>=128 || py<0 || py>=canvasHeight) return;
    auto& byte=buffer[(py/8)*128+px];
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
  void setTextSize(int size){assert(size==1);}
  void setTextColor(int value,int bg=-1){color=value;background=bg;}
  void setCursor(int px,int py){x=px;y=py;}
  void print(const char* text) {
    textRows.push_back(y);
    for(;*text;++text) {
      auto ch=static_cast<unsigned char>(*text);
      if(ch=='\n'){x=0;y+=8;continue;}
      if(x+6>128){x=0;y+=8;}
      for(int xx=0;xx<6;++xx) for(int yy=0;yy<8;++yy) {
        const bool on=xx<5 && ((font[ch*5+xx]>>yy)&1);
        if(on || background>=0) pixel(x+xx,y+yy,on?color:background);
      }
      x+=6;
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
  explicit CanvasGfx(Adafruit_SSD1306& d):Adafruit_GFX(128,canvasHeight),c(d){}
  void drawPixel(int16_t px,int16_t py,uint16_t v) override { c.pixel(px,py,v); }
};
void oledTextFieldDraw(Adafruit_SSD1306* d,int x,int y,int w,const char* t,uint16_t fg,uint16_t bg,bool a,const void* o) {
  if(!d) return;
  if(t && *t) d->textRows.push_back(y);
  CanvasGfx g(*d);
  oledTextFieldDraw(static_cast<Adafruit_GFX*>(&g),x,y,w,t,fg,bg,a,o);
}
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
static void save(const Adafruit_SSD1306& d,const std::string& path,bool panel=false) {
  std::ofstream out(path,std::ios::binary);
  const int w=panel?240:128,h=panel?240:canvasHeight;
  out<<"P5\n"<<w<<" "<<h<<"\n255\n";
  std::vector<uint16_t> physical(240*240);
  if(panel) p4eye_display::rasterize(d.buffer.data(),physical.data(),0,240,false);
  for(int yy=0;yy<h;++yy) for(int xx=0;xx<w;++xx) {
    bool lit=panel?physical[yy*w+xx]!=0:((d.buffer[(yy/8)*128+xx]>>(yy&7))&1)!=0;
    out.put(lit?char(255):char(0));
  }
}
int main(int argc,char** argv) {
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
#endif
  std::puts("OLED/P4 layout, performance-monitor rows/scrolling, map packing and render bounds passed");
}
