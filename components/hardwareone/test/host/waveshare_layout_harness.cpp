#include "HAL_Display_WavesharePixels.h"
#include "OLED_TouchLayout.h"
#include <cassert>
#include <vector>
using namespace waveshare_display;
int main() {
  static_assert(kCanvasBytes == 1408);
  static_assert(toPanel(0,0,true).x==719 && toPanel(0,0,true).y==0);
  static_assert(toPanel(1279,719,true).x==0 && toPanel(1279,719,true).y==1279);
  static_assert(fromPanel(0,1279,true).x==1279 && fromPanel(0,1279,true).y==719);
  std::vector<uint8_t> canvas(kCanvasBytes);
  for (int y=0;y<kCanvasHeight;++y)
    for(int x=0;x<kCanvasWidth;++x)
      if ((x+3*y)%11 < 5) canvas[(y/8)*kCanvasWidth+x] |= 1u<<(y&7);
  for (bool landscape : {false,true}) {
    const auto v=viewport(landscape);
    const int width=landscape?1280:720, height=landscape?720:1280;
    assert(v.x>=0 && v.y>=0 && v.x+kCanvasWidth*v.scale<=width);
    assert(v.y+kCanvasHeight*v.scale<=height);
    for(bool invert : {false,true}) {
      std::vector<uint16_t> storage(kPanelWidth*kPanelHeight+2,0x1234);
      auto* pixels=storage.data()+1;
      rasterize(canvas.data(),pixels,landscape,invert);
      assert(storage.front()==0x1234 && storage.back()==0x1234);
      for (int y=0;y<kPanelHeight;++y)
        for(int x=0;x<kPanelWidth;++x) {
          auto p=fromPanel(x,y,landscape);
          auto native=toPanel(p.x,p.y,landscape);
          assert(native.x==x && native.y==y);
          uint16_t expected=0x1234;
          if(p.x>=v.x && p.x<v.x+kCanvasWidth*v.scale &&
             p.y>=v.y && p.y<v.y+kCanvasHeight*v.scale) {
            int sx=(p.x-v.x)/v.scale, sy=(p.y-v.y)/v.scale;
            bool bit=(canvas[(sy/8)*kCanvasWidth+sx]>>(sy&7))&1;
            expected=bit!=invert?0xffff:0;
          }
          assert(pixels[y*kPanelWidth+x]==expected);
        }
    }
    for(size_t i=0;i<touchControlCount;++i) {
      const auto& c=touchControlsFor(landscape)[i];
      assert(c.x>=0 && c.y>=0 && c.x+c.w<=width && c.y+c.h<=height);
      assert(c.x>=v.x+kCanvasWidth*v.scale || c.y>=v.y+kCanvasHeight*v.scale);
      assert(touchHitTest(c.x+c.w/2,c.y+c.h/2,landscape)==c.action);
      for(size_t j=0;j<i;++j) {
        const auto& other=touchControlsFor(landscape)[j];
        assert(c.x+c.w<=other.x || other.x+other.w<=c.x || c.y+c.h<=other.y || other.y+other.h<=c.y);
      }
      const auto point=toPanel(c.x+c.w/2,c.y+c.h/2,landscape);
      TouchEvent event{}; event.x=point.x;event.y=point.y;event.id=1;
      TouchTap tap;event.phase=TouchPhase::Down;
      assert(tap.accept(event,landscape)==(touchActionIsDirection(c.action)?c.action:TouchAction::None));
      event.phase=TouchPhase::Up;
      assert(tap.accept(event,landscape)==(touchActionIsDirection(c.action)?TouchAction::None:c.action));
    }
  }
  // A held portrait action must not complete in the landscape layout.
  TouchTap tap; TouchEvent e{};e.x=500;e.y=880;e.id=1;e.phase=TouchPhase::Down;
  assert(tap.accept(e,false)==TouchAction::None);
  auto p=toPanel(1100,310,true);e.x=p.x;e.y=p.y;e.phase=TouchPhase::Up;
  assert(tap.accept(e,true)==TouchAction::None);
}
