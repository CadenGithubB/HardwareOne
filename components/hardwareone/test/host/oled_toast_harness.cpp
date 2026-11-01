#include "OLED_MessageText.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#define SCREEN_WIDTH 128
#define OLED_CONTENT_HEIGHT (SCREEN_HEIGHT-21)
constexpr int DISPLAY_FG=1, DISPLAY_BG=0;
uint32_t now=0;
uint32_t millis(){return now;}
bool locked=false;
int sToastMux=0, dirty=0;
#define portENTER_CRITICAL(m) do { (void)(m);assert(!locked);locked=true; } while(0)
#define portEXIT_CRITICAL(m) do { (void)(m);assert(locked);locked=false; } while(0)
void oledMarkDirty(){assert(!locked);++dirty;}
// INSERT_TYPES
void oledToastShow(const char*,uint32_t=3000,OledUIIcon=OledUIIcon::NONE);
OledToast gOledToast{};
struct Screen {
 int x=0,y=0,iconColor=-1;std::vector<std::string> lines;
 void fillRect(int a,int b,int w,int h,int){assert(!locked);assert(a>=0 && b>=11 && a+w<=128 && b+h<=SCREEN_HEIGHT-10);}
 void drawRect(int a,int b,int w,int h,int c){fillRect(a,b,w,h,c);}
 void setTextSize(int size){assert(size==1);}
 void setTextColor(int){}
 void setCursor(int a,int b){x=a;y=b;}
 void print(const char* text){assert(!locked);assert(x+int(strlen(text))*6<120 && y+8<=SCREEN_HEIGHT-10);lines.emplace_back(text);}
};
Screen display;Screen* gDisplay=&display;
void oledDrawIcon(int,int,OledUIIcon,int color){display.iconColor=color;}
// INSERT_METHODS
int main(){
 assert(oled_message::isError(" [error] timed out"));
 assert(oled_message::isError("usage: openwifi"));
 assert(!oled_message::isError("ErrorCount = 0"));
 auto wrapped=oled_message::wrap("one two three four",7,2);
 assert(wrapped.count==2 && std::string(wrapped.text[0])=="one two" && std::string(wrapped.text[1])=="th...");
 oledToastShow("No saved Wi-Fi networks. Add one in Wi-Fi settings.",2500,OledUIIcon::ERROR);
 assert(gOledToast.expireMs==8000 && gOledToast.icon==OledUIIcon::ERROR);
 oledToastRender();assert(display.iconColor==DISPLAY_BG);
 assert(display.lines.size()>=3 && display.lines.size()<=6);
 std::string message;for(const auto& line:display.lines)message+=line+" ";
 assert(message.find("settings.")!=std::string::npos);
 now=7999;assert(oledToastActive());int before=dirty;
 now=8000;assert(!oledToastActive() && dirty==before+1);
 assert(!oledToastActive() && dirty==before+1);
 now=UINT32_MAX-100;oledToastShow("ERROR: busy",1);
 assert(gOledToast.icon==OledUIIcon::ERROR);
 assert(oledToastActive());now+=7999;assert(oledToastActive());++now;assert(!oledToastActive());
 oledToastShow("Saved",2500,OledUIIcon::SUCCESS);assert(gOledToast.expireMs==now+2500);
 oledToastClear();assert(!oledToastActive());
 std::string longText(300,'x');oledToastShow(longText.c_str());
 assert(strlen(gOledToast.message)==oled_message::kMessageBytes-1);
 display.lines.clear();oledToastRender();
 assert(display.lines.back().find("...")!=std::string::npos);
}
