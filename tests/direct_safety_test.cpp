#include "nxwarp_direct_safety.h"
#include <cassert>
#include <cstdio>
using namespace wivrn::nxwarp_direct;
using bytes = std::vector<uint8_t>;
int main() {
    const layout full{128,128,2}, low{32,32,2};
    bytes base = frame_header(low.tile_count(),0);
    for (unsigned i=0;i<low.tile_count();++i) append32(base,0xc0123456);
    bytes detail=frame_header(full.tile_count(),0);
    for (unsigned i=0;i<full.tile_count();++i) append32(detail,0xc0654321);
    bytes unit;
    for(uint32_t v:{safety_magic,1u,low.width,low.height,low.eyes,uint32_t(base.size()),uint32_t(detail.size()),0u}) append32(unit,v);
    unit.insert(unit.end(),base.begin(),base.end()); unit.insert(unit.end(),detail.begin(),detail.end());
    auto h=parse_safety_header(full,unit); assert(h && h->total_bytes()==unit.size());
    assert(parse_frame(low,std::span(unit).subspan(32,base.size())));
    assert(parse_frame(full,std::span(unit).subspan(h->prefix_bytes())));
    bytes wire; append32(wire,unit.size()); wire.insert(wire.end(),unit.begin(),unit.end());
    constexpr size_t chunk=40;
    std::vector<bytes> slots;
    for(size_t i=0;i<wire.size();i+=chunk) slots.emplace_back(wire.begin()+i,wire.begin()+std::min(wire.size(),i+chunk));
    const bytes expected(unit.begin(),unit.begin()+h->prefix_bytes());
    assert(recover_safety_prefix(full,slots,chunk)==expected);
    for(size_t lost=0;lost<slots.size();++lost) {
        auto bad=slots; bad[lost].clear();
        const auto recovered=recover_safety_prefix(full,bad,chunk);
        if(lost*chunk<4+h->prefix_bytes()) assert(recovered.empty());
        else assert(recovered==expected);
    }
    // Late/missing detail does not affect safety. Short safety and inconsistent
    // length, geometry, flags or allocation sizes must fail before allocation.
    auto bad=slots; bad[1].pop_back(); assert(recover_safety_prefix(full,bad,chunk).empty());
    for(size_t offset:{size_t(0),size_t(4),size_t(8),size_t(16),size_t(20),size_t(24),size_t(28),size_t(32)}) {
        bad=slots; bad[0][offset]^=0x80; assert(recover_safety_prefix(full,bad,chunk).empty());
    }
    for(size_t n=0;n<32;++n) assert(!parse_safety_header(full,std::span(unit).first(n)));
    assert(recover_safety_prefix(full,{},chunk).empty());
    assert(recover_safety_prefix(full,slots,0).empty());
    for(unsigned version=1;version<=6;++version) {
        auto sh=stream_header(full,version%2==0,version>=3,version>=5);
        assert(read32(sh,4)==version && parse_stream(sh));
    }
    puts("NX safety: prefix loss boundaries, malformed headers and versions passed");
}
