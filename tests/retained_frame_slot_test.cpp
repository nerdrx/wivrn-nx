#include "utils/retained_frame_slot.h"
#include <array>
#include <set>
#include <iostream>
#include <cstdlib>
void require(bool ok) { if (!ok) std::abort(); }
int main() {
 for (size_t count : {3,4}) for (uint64_t stride : {1, 2, 3, 4, 8}) {
  std::array<std::optional<uint64_t>,4> ids{};
  auto active=std::span(ids).first(count);
  for (uint64_t n=1;n<=20;++n) {
   auto slot=wivrn::retained_frame_slot(active,n*stride); require(slot.has_value()); ids[*slot]=n*stride;
   std::set<uint64_t> got; for(auto id:ids) if(id) got.insert(*id);
   std::set<uint64_t> want; for(uint64_t j=n>=count?n-count+1:1;j<=n;++j) want.insert(j*stride);
   require(got==want);
  }
  auto before=ids; auto duplicate=wivrn::retained_frame_slot(active,20*stride); require(duplicate && ids[*duplicate]==20*stride);
  require(!wivrn::retained_frame_slot(active,1)); require(ids==before);
 }
 // Sparse IDs must retain frame 9 for a slower eye's matching stereo image.
 std::array<std::optional<uint64_t>,3> sparse{};
 for(uint64_t id : {0,1,2,9,12}) {
  auto slot=wivrn::retained_frame_slot(sparse,id); require(slot.has_value()); sparse[*slot]=id;
 }
 std::set<uint64_t> got; for(auto id:sparse) if(id) got.insert(*id);
 require(got==std::set<uint64_t>({2,9,12}));
 std::array<std::optional<uint64_t>,4> ids{100,104,108,112};
 auto late=wivrn::retained_frame_slot(ids,106);require(late && ids[*late]==100);
 require(!wivrn::retained_frame_slot({},42));
 std::cout<<"PASS: sequential and skipped IDs, duplicates, stale and out-of-order arrivals, empty capacity\n";
}
