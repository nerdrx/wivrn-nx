// Replace the central 128x128 per-eye patch with independent RGB888 pixels.
#pragma once
#include "nxwarp_direct.h"
#include <algorithm>
#include <span>
#include <vector>
namespace wivrn::nxwarp_direct {
inline std::span<const uint8_t> native_center_frame(layout l, std::span<const uint8_t> raw,
                                                   std::span<const uint32_t> rgb, std::vector<uint8_t>& out)
{
 constexpr uint32_t side=128, native_flag=1u<<29;
 const auto f=parse_frame(l,raw);
 if (!l.native_center || !f || read32(raw,4)!=1 || l.eyes!=2 || l.width<256 || l.height<256 || rgb.size()!=2*side*side)
  return raw;
 const uint32_t ox=((l.width-side)/2)&~31u, oy=((l.height-side)/2)&~31u;
 const uint32_t cols=l.width/32, rows=l.height/32;
 out.clear();out.reserve(raw.size()+2*side*side*4+128);
 for(uint32_t v:{frame_magic,2u,l.tile_count(),0u}) append32(out,v);
 out.resize(16+l.tile_count()*4);
 uint32_t words=0;
 for(uint32_t ty=0;ty<rows;++ty) for(uint32_t tx=0;tx<cols*l.eyes;++tx) {
  const uint32_t tile=ty*cols*l.eyes+tx, eye=tx/cols, x=(tx%cols)*32, y=ty*32;
  const uint32_t old=read32(f->descriptors,tile*4), mode=old>>30;
  uint32_t d=old;
  if(x>=ox && x<ox+side && y>=oy && y<oy+side) {
   d=native_flag|words;
   for(uint32_t dy=0;dy<32;++dy) for(uint32_t dx=0;dx<32;++dx)
    append32(out,rgb[eye*side*side+(y-oy+dy)*side+x-ox+dx]&0xffffffu);
   append32(out,0);words+=1025;
  } else if(mode!=3) {
   const uint32_t n=80u>>(2*mode), off=old&0x3fffffffu;
   d=(mode<<30)|words;
   out.insert(out.end(),f->blocks.begin()+off*4,f->blocks.begin()+(off+n)*4);words+=n;
  }
  for(unsigned b=0;b<4;++b) out[16+tile*4+b]=uint8_t(d>>(b*8));
 }
 for(unsigned b=0;b<4;++b) out[12+b]=uint8_t(words>>(b*8));
 return out;
}
}
