#include "OLED_MessageText.h"
#include "OLED_Layout.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#define SCREEN_WIDTH 128
#define OLED_CONTENT_HEIGHT (SCREEN_HEIGHT-21)
constexpr int DISPLAY_FG=1, DISPLAY_BG=0;
static bool oledPortrait(){return false;}  // the landscape toast; portrait is drawn on the bench
uint32_t now=0;
uint32_t millis(){return now;}
bool locked=false;
int sToastMux=0, dirty=0;
#define portENTER_CRITICAL(m) do { (void)(m);assert(!locked);locked=true; } while(0)
#define portEXIT_CRITICAL(m) do { (void)(m);assert(locked);locked=false; } while(0)
void oledMarkDirty(){assert(!locked);++dirty;}
// INSERT_TYPES
void oledToastShow(const char*,uint32_t=3000,OledUiIcon=OledUiIcon::NONE);
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
void oledDrawIcon(int,int,OledUiIcon,int color){display.iconColor=color;}
bool oledBootModeActive=false, confirmation=false;
bool oledConfirmIsActive(){return confirmation;}
// INSERT_METHODS
int main(){
 assert(oled_message::isError(" [error] timed out"));
 assert(oled_message::isError("usage: openwifi"));
 assert(!oled_message::isError("ErrorCount = 0"));
 auto wrapped=oled_message::wrap("one two three four",7,2);
 assert(wrapped.count==2 && std::string(wrapped.text[0])=="one two" && std::string(wrapped.text[1])=="th...");
 oledToastShow("No saved Wi-Fi networks. Add one in Wi-Fi settings.",2500,OledUiIcon::ERROR);
 assert(gOledToast.expireMs==8000 && gOledToast.icon==OledUiIcon::ERROR);
 oledToastRender();assert(display.iconColor==DISPLAY_BG);
 assert(display.lines.size()>=3 && display.lines.size()<=6);
 std::string message;for(const auto& line:display.lines)message+=line+" ";
 assert(message.find("settings.")!=std::string::npos);
 now=7999;assert(oledToastActive());int before=dirty;
 now=8000;assert(!oledToastActive() && dirty==before+1);
 assert(!oledToastActive() && dirty==before+1);
 now=UINT32_MAX-100;oledToastShow("ERROR: busy",1);
 assert(gOledToast.icon==OledUiIcon::ERROR);
 assert(oledToastActive());now+=7999;assert(oledToastActive());++now;assert(!oledToastActive());
 oledToastShow("Saved",2500,OledUiIcon::SUCCESS);assert(gOledToast.expireMs==now+2500);
 oledToastClear();assert(!oledToastActive());
 oledToastClear();
 std::string longText(300,'x');oledToastShow(longText.c_str());
 assert(strlen(gOledToast.message)==oled_message::kMessageBytes-1);
 display.lines.clear();oledToastRender();
 assert(display.lines.back().find("...")!=std::string::npos);
 // Bursts cannot overwrite an error. Pending items start their own lifetime.
 oledToastClear();now=0;
 oledToastShow("Error: denied",1);oledToastShow("Saved",2000,OledUiIcon::SUCCESS);
 assert(std::string(gOledToast.message)=="denied" && sToastQueueCount==1);
 oledToastShow("Saved",2000,OledUiIcon::SUCCESS);assert(sToastQueueCount==1);
 now=7999;assert(oledToastActive() && std::string(gOledToast.message)=="denied");
 now=8000;assert(oledToastActive() && std::string(gOledToast.message)=="Saved" && gOledToast.expireMs==10000);
 now=10000;assert(!oledToastActive());
 // Confirmation hides feedback and preserves remaining lifetime, including
 // messages submitted while it is open and pauses across millis rollover.
 oledToastClear();now=UINT32_MAX-100;
 oledToastShow("Error: wait",1);now+=50;
 confirmation=true;display.lines.clear();oledUiRender();assert(display.lines.empty());
 now+=20000;assert(oledToastActive());
 confirmation=false;oledUiRender();assert(gOledToast.expireMs==now+7950);
 now+=7950;assert(!oledToastActive());
 oledToastClear();now=0;oledFeedbackSetPaused(true);now=10000;
 oledToastShow("queued while confirming",2000);now=30000;oledFeedbackSetPaused(false);
 assert(gOledToast.expireMs==32000);
 // Overflow retains pending errors against routine feedback.
 oledToastClear();oledToastShow("active",1);
 for(unsigned i=0;i<kToastQueueCapacity;++i){char text[24];snprintf(text,sizeof(text),"error %u",i);oledToastShow(text,1,OledUiIcon::ERROR);}
 oledToastShow("routine",1);assert(sToastQueueCount==kToastQueueCapacity && sToastQueue[0].icon==OledUiIcon::ERROR);
 oledToastShow("new error",1,OledUiIcon::ERROR);assert(std::string(sToastQueue[kToastQueueCapacity-1].message)=="new error");
 oledToastClear();assert(!oledToastActive() && !sToastQueueCount && !sToastQueue[0].message[0]);
 // Event severity goes through the same renderer and minimum error duration.
 now=0;oledNotificationBannerShow("event fault",OledUiIcon::ERROR,2000);
 assert(gOledToast.icon==OledUiIcon::ERROR && gOledToast.expireMs==8000);
 oledToastClear();oledBootModeActive=true;
 oledNotificationBannerShow("boot",OledUiIcon::INFO,2000);assert(!oledToastActive());

}
