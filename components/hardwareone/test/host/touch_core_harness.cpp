#include "Input_TouchCore.h"
#include "OLED_TouchLayout.h"
#include <assert.h>

static TouchAction drain(TouchQueue& queue, TouchTap& tap) {
  TouchAction action = TouchAction::None;
  TouchEvent e;
  while (queue.pop(e)) { auto a = tap.accept(e); if (a != TouchAction::None) action = a; }
  return action;
}
int main() {
  TouchQueue q; TouchTap tap;
  // First held contact is suppressed until released.
  q.sample(1, 0, 500, 880, 1); q.sample(0, 0, 0, 0, 2);
  assert(drain(q,tap)==TouchAction::None);
  // A completed short tap survives between UI frames and consumes once.
  q.sample(1, 0, 500, 880, 3); q.sample(0, 0, 0, 0, 4);
  assert(drain(q,tap)==TouchAction::Select);
  assert(drain(q,tap)==TouchAction::None);
  // Small moves retain the Down/Up pair.
  q.sample(1, 0, 500, 880, 5);
  for(int i=0;i<10;i++) q.sample(1,0,500+i%50,880,6+i);
  q.sample(0,0,0,0,110);
  assert(drain(q,tap)==TouchAction::Select);
  // Dragging outside cancels, including when the finger returns.
  q.sample(1,0,500,880,111); q.sample(1,0,1,1,112);
  q.sample(1,0,500,880,113); q.sample(0,0,0,0,114);
  assert(drain(q,tap)==TouchAction::None);
  // Extra fingers cancel; removing one cannot turn the other into a tap.
  q.sample(1,0,500,880,115); q.sample(2,0,500,880,116);
  q.sample(1,0,500,880,117); q.sample(0,0,0,0,118);
  assert(drain(q,tap)==TouchAction::None);
  // UI ownership change while held and transport loss cancel gestures.
  q.sample(1,0,500,880,119); drain(q,tap); q.discard(120);
  q.sample(0,0,0,0,121); assert(drain(q,tap)==TouchAction::None);
  q.sample(1,0,500,880,122); q.cancel(123);
  q.sample(0,0,0,0,124); assert(drain(q,tap)==TouchAction::None);
  // Changing context while idle allows the very next fresh tap.
  q.discard(125); drain(q,tap);
  q.sample(1,0,500,880,126); q.sample(0,0,0,0,127);
  assert(drain(q,tap)==TouchAction::Select);
  // Overflow drops pending actions and requires a release before resuming.
  for(int i=0;i<9;i++) { q.sample(1,0,500,880,130+i*2); q.sample(0,0,0,0,131+i*2); }
  assert(drain(q,tap)==TouchAction::None);
  // A changed tracking ID never completes another contact's gesture.
  q.sample(1,0,500,880,160); q.sample(1,1,500,880,161); q.sample(0,0,0,0,162);
  assert(drain(q,tap)==TouchAction::None);
  assert(touchHitTest(0,0)==TouchAction::None);
  assert(touchHitTest(680,880)==TouchAction::None);
  for(const auto& c:touchControls) assert(touchHitTest(c.x+c.w/2,c.y+c.h/2)==c.action);
}
