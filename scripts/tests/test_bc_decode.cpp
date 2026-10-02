#include "util/util_bc.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
using namespace dxvk;

static void solid(uint8_t* p, unsigned kind, uint8_t alpha = 255) {
  std::memset(p, 0, kind == 1 || kind == 4 ? 8 : 16);
  if (kind == 1) { p[0]=0; p[1]=0xf8; } // RGB565 red, index 0.
  if (kind == 2) { std::memset(p, alpha, 8); p[8]=0; p[9]=0xf8; }
  if (kind == 3) { p[0]=alpha; p[8]=0; p[9]=0xf8; }
  if (kind == 4 || kind == 5) { p[0]=73; if (kind == 5) p[8]=191; }
}
int main() {
  const VkFormat formats[] = {VK_FORMAT_UNDEFINED, VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
    VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC4_UNORM_BLOCK,
    VK_FORMAT_BC5_UNORM_BLOCK};
  const VkExtent3D extents[] = {{1,1,1},{2,2,1},{4,4,1},{5,3,1},{7,9,1},{9,7,3}};
  for (unsigned kind=1; kind<=5; kind++) for (auto extent : extents) {
    unsigned bytes = kind==1 || kind==4 ? 8 : 16;
    unsigned pixels = kind==4 ? 1 : kind==5 ? 2 : 4;
    unsigned bx=(extent.width+3)/4, by=(extent.height+3)/4;
    unsigned row=bx*bytes+16, slice=row*by+32;
    std::vector<uint8_t> input(slice*extent.depth, 0xCD);
    for (unsigned z=0;z<extent.depth;z++) for(unsigned y=0;y<by;y++) for(unsigned x=0;x<bx;x++)
      solid(input.data()+z*slice+y*row+x*bytes,kind,0xaa);
    CpuImage out;
    VkFormat target = kind==4 ? VK_FORMAT_R8_UNORM : kind==5 ? VK_FORMAT_R8G8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    assert(DecodeBcImage(formats[kind], target, extent, input.data(), row, slice, out));
    assert(out.rowPitch==extent.width*pixels && out.slicePitch==out.rowPitch*extent.height);
    assert(out.data.size()==out.slicePitch*extent.depth);
    for (size_t i=0;i<out.data.size();i+=pixels) {
      assert(out.data[i]==(kind>=4?73:255));
      if (kind==5) assert(out.data[i+1]==191);
      if (kind<=3) {
        assert(out.data[i+1]==0 && out.data[i+2]==0);
        assert(out.data[i+3]==(kind==1?255:170));
      }
    }
    assert(!DecodeBcImage(formats[kind],target,extent,input.data(),bx*bytes-1,slice,out));
    assert(!DecodeBcImage(formats[kind],target,extent,input.data(),row,row*by-1,out));
  }
  std::array<uint8_t,8> transparent{};
  std::fill(transparent.begin()+4,transparent.end(),255); // BC1 c0<=c1, index 3.
  CpuImage out;
  assert(DecodeBcImage(VK_FORMAT_BC1_RGBA_UNORM_BLOCK,VK_FORMAT_R8G8B8A8_UNORM,{1,1,1},transparent.data(),8,8,out));
  assert(out.data==std::vector<uint8_t>({0,0,0,0}));
  transparent.fill(0);
  assert(DecodeBcImage(VK_FORMAT_BC1_RGBA_SRGB_BLOCK,VK_FORMAT_R8G8B8A8_SRGB,{1,1,1},transparent.data(),8,8,out));
  assert(out.data==std::vector<uint8_t>({0,0,0,255})); // zero BC differs from zero RGBA.
  assert(!DecodeBcImage(VK_FORMAT_UNDEFINED,VK_FORMAT_R8_UNORM,{1,1,1},transparent.data(),8,8,out));
  std::cout << "PASS: BC1-5 decode, alpha, padded pitches, edge mips, volume slices, invalid pitches and sRGB zero-blocks\n";
}
