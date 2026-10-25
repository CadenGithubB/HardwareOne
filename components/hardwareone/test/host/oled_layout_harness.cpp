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
struct OLEDFooterHints {};
void oledRenderFooter(Adafruit_SSD1306*, const OLEDFooterHints*){}
void oledDrawIcon(Adafruit_SSD1306*, const char*, int, int, int){}
// INSERT_TYPES
// INSERT_FUNCTIONS
static OLEDScrollState sMainScroll;
static bool sMainScrollInit=false;
// INSERT_MENU_INIT
// INSERT_MAP_PACK
static Adafruit_SSD1306* oledDisplay=nullptr;
static bool oledConnected=true;
static struct {bool up=false,down=false;} gNavEvents;
#define INPUT_BUTTON_A 1
#define INPUT_CHECK(buttons,mask) ((buttons)&(mask))
uint32_t millis(){return 1000;}
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
    OLEDScrollState simple;
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

    // Exercise the shipping map packer over both sizes, including the last row.
    std::vector<uint8_t> shades(128*height), packed(128*((height+7)/8),0xaa);
    shades.back()=15; shades[64*128-1]=1;
    packShadeBufTo1Bit(shades.data(),packed.data());
    assert(packed.back()==0x80);
    for(int yy=0;yy<height;++yy) for(int xx=0;xx<128;++xx)
      assert(((packed[(yy/8)*128+xx]>>(yy&7))&1)==(shades[yy*128+xx]!=0));
  }
  std::puts("OLED/P4 layout, performance-monitor rows/scrolling, map packing and render bounds passed");
}
