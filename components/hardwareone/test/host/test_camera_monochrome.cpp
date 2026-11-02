#include "CameraMonochrome.h"
#include <cassert>
#include <cstdio>
#include <vector>

int main() {
  std::vector<uint8_t> rgb(32*32*3,128), shades(16*16), bits(32);
  camera_mono::Tone tone;
  camera_mono::Image image;
  auto render=[&](bool dither) {
    return camera_mono::render(rgb.data(),32,32,96,rgb.size(),shades.data(),shades.size(),
                               bits.data(),bits.size(),16,16,tone,0,dither,image);
  };
  assert(render(true)&&image.whitePixels==128);
  const auto stable=bits;assert(render(true)&&bits==stable);
  assert(render(false)&&image.whitePixels==256);
  std::fill(rgb.begin(),rgb.end(),0);tone={};assert(render(true)&&image.whitePixels==0);
  std::fill(rgb.begin(),rgb.end(),255);tone={};assert(render(true)&&image.whitePixels==256);
  std::fill(rgb.begin(),rgb.end(),8);tone={};assert(render(true)&&image.whitePixels<=16);
  // Fine source texture is averaged before one-bit conversion.
  for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x)
    for(unsigned c=0;c<3;++c)rgb[(y*32+x)*3+c]=((x+y)&1)?255:0;
  tone={};assert(render(true));for(auto g:shades)assert(g==127);
  unsigned counts[3]{};
  for(unsigned channel=0;channel<3;++channel) {
    std::fill(rgb.begin(),rgb.end(),0);
    for(unsigned p=0;p<32*32;++p)rgb[p*3+channel]=255;
    tone={};assert(render(true));counts[channel]=image.whitePixels;
  }
  assert(counts[1]>counts[0]&&counts[0]>counts[2]);
  // Actual OLED, Waveshare and EYE viewport heights, with non-byte widths too.
  for(unsigned height:{34u,58u,98u})for(unsigned width:{127u,128u}) {
    const size_t n=(width+7)/8*height;
    std::vector<uint8_t> out(n+2,0x5a), gray(width*height+2,0x6b);
    std::fill(rgb.begin(),rgb.end(),255);tone={};
    assert(camera_mono::render(rgb.data(),32,32,96,rgb.size(),gray.data()+1,gray.size()-2,
                              out.data()+1,n,width,height,tone,0,true,image));
    assert(image.width==height&&image.height==height&&image.whitePixels==height*height);
    assert(out.front()==0x5a&&out.back()==0x5a&&gray.front()==0x6b&&gray.back()==0x6b);
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
      const bool on=out[1+y*((width+7)/8)+x/8]&(0x80u>>(x&7));
      assert(on==(x>=image.x&&x<image.x+image.width));
    }
  }
  assert(!camera_mono::render(rgb.data(),32,32,SIZE_MAX,SIZE_MAX,shades.data(),shades.size(),
                              bits.data(),bits.size(),16,16,tone,0,true,image));
  assert(!camera_mono::render(rgb.data(),32,32,96,rgb.size()-1,shades.data(),shades.size(),
                              bits.data(),bits.size(),16,16,tone,0,true,image));
  assert(!camera_mono::render(rgb.data(),32,32,96,rgb.size(),shades.data(),1,
                              bits.data(),bits.size(),16,16,tone,0,true,image));
  puts("Camera monochrome: area averaging, luminance, stable dithering, aspect fit and bounded writes passed");
}
