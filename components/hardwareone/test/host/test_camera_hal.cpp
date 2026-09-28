#include "HAL_Camera.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int main() {
  // Persistence compatibility: every pre-existing ID keeps its dimensions.
  static constexpr unsigned expected[][2] = {
    {320,240},{640,480},{800,600},{1024,768},{1280,1024},{1600,1200},
    {96,96},{160,120},{176,144},{240,176},{240,240},{1280,720},{400,296}
  };
  assert(unsigned(CameraFrameSize::Count)==sizeof(expected)/sizeof(expected[0]));
  for(unsigned i=0;i<unsigned(CameraFrameSize::Count);++i) {
    const auto* info=cameraFrameSizeInfo(CameraFrameSize(i));
    assert(info && unsigned(info->id)==i && info->width==expected[i][0] && info->height==expected[i][1]);
    assert(info->name && *info->name);
    assert(cameraFrameSizeFromSetting(i)==CameraFrameSize(i));
  }
  assert(!cameraFrameSizeInfo(CameraFrameSize::Count));
  assert(!cameraFrameSizeInfo(CameraFrameSize(255)));
  assert(cameraFrameSizeFromSetting(-1)==CameraFrameSize::VGA);
  assert(cameraFrameSizeFromSetting(256)==CameraFrameSize::VGA);
  for(unsigned i=0;i<unsigned(CameraControl::Count);++i) {
    assert(strcmp(cameraControlName(CameraControl(i)),"unknown"));
    for(unsigned j=0;j<i;++j) assert(strcmp(cameraControlName(CameraControl(i)),cameraControlName(CameraControl(j))));
  }
  assert(!strcmp(cameraControlName(CameraControl::Count),"unknown"));

  // This validates the ownership/budget boundary, not a JPEG decoder: a
  // four-byte SOI/EOI shell is accepted here and still fails real decoding.
  CameraFrame frame;
  assert(!cameraFrameIsValid(frame,kCameraMaxFrameBytes));
  frame.data=static_cast<uint8_t*>(malloc(kCameraMaxFrameBytes));
  assert(frame.data);
  frame.width=320;frame.height=240;frame.length=kCameraMaxFrameBytes;
  memset(frame.data,0,kCameraMaxFrameBytes);
  frame.data[0]=0xff;frame.data[1]=0xd8;
  frame.data[frame.length-2]=0xff;frame.data[frame.length-1]=0xd9;
  assert(cameraFrameIsValid(frame,kCameraMaxFrameBytes));
  assert(!cameraFrameIsValid(frame,kCameraMaxFrameBytes-1));
  assert(!cameraFrameIsValid(frame,0));
  frame.width=0;assert(!cameraFrameIsValid(frame,kCameraMaxFrameBytes));frame.width=320;
  frame.height=0;assert(!cameraFrameIsValid(frame,kCameraMaxFrameBytes));frame.height=240;
  frame.data[0]=0;assert(!cameraFrameIsValid(frame,kCameraMaxFrameBytes));frame.data[0]=0xff;
  frame.data[frame.length-1]=0;assert(!cameraFrameIsValid(frame,kCameraMaxFrameBytes));
  frame.length=3;assert(!cameraFrameIsValid(frame,kCameraMaxFrameBytes));
  frame.length=4;frame.data[2]=0xff;frame.data[3]=0xd9;
  assert(cameraFrameIsValid(frame,4));
  cameraFrameRelease(frame);
  assert(!frame.data && !frame.length && !frame.width && !frame.height);
  cameraFrameRelease(frame);
  puts("CAMERA_HAL PASS");
}
