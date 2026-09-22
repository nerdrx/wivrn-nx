#include "nxwarp_direct_flat.h"
#include "nxwarp_direct_lz4.h"
#include <chrono>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <cstdio>
#include <cassert>
using namespace wivrn::nxwarp_direct;
int main(int argc,char **argv) {
 for(int i=1;i<argc;++i) {
  std::ifstream f(argv[i],std::ios::binary);std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)),{}),out,packed,baseline;
  assert(parse_frame({2176,2176,2},raw));
  std::vector<double> times;
  for(int k=0;k<48;++k) {auto t=std::chrono::steady_clock::now();auto flat=compact_flat({2176,2176,2},raw,out);if(k>=8) times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count());assert(parse_frame({2176,2176,2},flat));}
  auto flat=compact_flat({2176,2176,2},raw,out);auto a=compress_lz4(raw,baseline);auto b=compress_lz4(flat,packed);std::sort(times.begin(),times.end());
  std::printf("%s,%zu,%zu,%zu,%zu,%.6f,%.6f\n",argv[i],raw.size(),flat.size(),a.size(),b.size(),times[20],times[38]);
  std::ofstream saved(std::string(argv[i])+".flat",std::ios::binary);saved.write((const char*)flat.data(),flat.size());
 }
}
