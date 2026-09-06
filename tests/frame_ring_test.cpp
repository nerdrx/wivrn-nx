// The hand-over between the decode threads and the render thread: a frame reaches the
// display complete or not at all, and the reader never waits for the writer.
//
// The class under test is the real one the headset runs -- wivrn::latest_complete_ring
// (client/utils/frame_ring.h) -- with a payload whose contents check themselves, which
// is the point: Vulkan handles cannot say whether they were sampled mid-write, and a
// half-written frame is exactly the failure this exists to prevent. One eye of frame N
// beside the other eye of frame N+1 is a visible judder that no fence would catch.
//
// Part A: the empty ring. Nothing published reads as nothing, not as a stale slot.
// Part B: the uncontended case, which has to be exact before the racy one means anything.
// Part C: the race. A writer publishing as fast as it can against a reader sampling as
//         fast as it can, for millions of hand-overs, asserting every frame read is
//         internally consistent. This is the tearing test.
// Part D: the reader never goes backwards past what the ring still holds, and never
//         blocks -- measured as progress, not as an absence of deadlock.
// Part E: the step-back rule. With the newest slot deliberately held mid-write, the
//         reader must return the previous frame rather than nothing and rather than
//         waiting.

#include "utils/frame_ring.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

namespace {

int checks = 0;
int failures = 0;

void check(bool ok, const char * what)
{
	++checks;
	if (ok)
	{
		std::printf("  ok   %s\n", what);
	}
	else
	{
		++failures;
		std::printf("  FAIL %s\n", what);
	}
}

// A frame whose contents prove they were written together. Every word carries the same
// id, so a copy that mixes two publications is detectable by inspection alone -- which
// is what a pair of eye handles from different frames would be, without the luxury of
// being able to look at them.
//
// Deliberately several words wide: a payload small enough to be written atomically by
// the compiler would pass this test without the sequence lock doing anything.
struct frame
{
	static constexpr size_t words = 32;
	uint64_t id = 0;
	uint64_t w[words]{};

	bool consistent() const
	{
		for (size_t i = 0; i < words; ++i)
			if (w[i] != id * 0x9e3779b97f4a7c15ull + i)
				return false;
		return true;
	}
	static frame make(uint64_t id)
	{
		frame f;
		f.id = id;
		for (size_t i = 0; i < words; ++i)
			f.w[i] = id * 0x9e3779b97f4a7c15ull + i;
		return f;
	}
};

void part_a()
{
	std::printf("\nPart A: an empty ring reports empty\n");
	wivrn::latest_complete_ring<frame, 4> ring;
	frame out = frame::make(999);
	check(not ring.load_latest(out), "nothing published reads as nothing");
	check(ring.published() == 0, "and the publication count is zero");
	// The out parameter must be left alone on failure: a consumer that presented it
	// anyway would be showing whatever its own stack held.
	check(out.id == 999, "a failed load does not touch the caller's frame");
}

void part_b()
{
	std::printf("\nPart B: uncontended publish and load\n");
	wivrn::latest_complete_ring<frame, 4> ring;
	frame out;

	ring.publish(frame::make(1));
	check(ring.load_latest(out) and out.id == 1, "the first frame reads back");
	check(out.consistent(), "and is internally consistent");

	for (uint64_t i = 2; i <= 100; ++i)
		ring.publish(frame::make(i));
	check(ring.load_latest(out) and out.id == 100, "the newest frame is the one returned");
	check(out.consistent(), "and it is consistent after wrapping the ring many times");
	check(ring.published() == 100, "every publication is counted");

	// Reading twice with no publication in between must give the same frame: the
	// display loop reads once per refresh and may well outrun the decoder.
	frame again;
	check(ring.load_latest(again) and again.id == out.id,
	      "reading again with no new frame returns the same one");
}

void part_c()
{
	std::printf("\nPart C: a writer and a reader racing, for millions of hand-overs\n");

	wivrn::latest_complete_ring<frame, 4> ring;
	std::atomic<bool> stop{false};
	std::atomic<uint64_t> reads{0};
	std::atomic<uint64_t> torn{0};
	std::atomic<uint64_t> misses{0};
	std::atomic<uint64_t> published{0};

	std::thread writer([&] {
		uint64_t id = 1;
		while (not stop.load(std::memory_order_relaxed))
		{
			ring.publish(frame::make(id));
			published.store(id, std::memory_order_relaxed);
			++id;
		}
	});

	std::thread reader([&] {
		frame out;
		while (not stop.load(std::memory_order_relaxed))
		{
			if (ring.load_latest(out))
			{
				reads.fetch_add(1, std::memory_order_relaxed);
				// THE assertion: whatever came back is one frame, not two.
				if (not out.consistent())
					torn.fetch_add(1, std::memory_order_relaxed);
			}
			else
			{
				misses.fetch_add(1, std::memory_order_relaxed);
			}
		}
	});

	std::this_thread::sleep_for(std::chrono::seconds(3));
	stop.store(true, std::memory_order_relaxed);
	writer.join();
	reader.join();

	std::printf("  %llu publications, %llu reads, %llu torn, %llu empty\n",
	            (unsigned long long)published.load(),
	            (unsigned long long)reads.load(),
	            (unsigned long long)torn.load(),
	            (unsigned long long)misses.load());

	check(published.load() > 100000, "the writer really did contend (>100k publications)");
	check(reads.load() > 100000, "and the reader really did read (>100k reads)");
	check(torn.load() == 0, "NO torn frame, over every read");
	// A miss is legal -- it means every slot the reader looked at was mid-write -- but
	// with four slots and one writer it should be vanishingly rare. Anything else means
	// the step-back rule is not working and the display would be starved.
	check(misses.load() * 1000 < reads.load(), "and the reader almost never comes back empty");
}

void part_d()
{
	std::printf("\nPart D: the reader makes progress and never blocks\n");

	wivrn::latest_complete_ring<frame, 4> ring;
	std::atomic<bool> stop{false};
	std::atomic<uint64_t> last_seen{0};
	std::atomic<uint64_t> backwards{0};

	std::thread writer([&] {
		uint64_t id = 1;
		while (not stop.load(std::memory_order_relaxed))
			ring.publish(frame::make(id++));
	});

	std::thread reader([&] {
		frame out;
		uint64_t prev = 0;
		while (not stop.load(std::memory_order_relaxed))
		{
			if (ring.load_latest(out))
			{
				// The id may repeat (the reader outruns the writer) and may skip
				// (the writer outruns the reader). It must not go BACKWARDS by
				// more than the ring depth, which is the only staleness the
				// step-back rule can introduce.
				if (out.id + 4 < prev)
					backwards.fetch_add(1, std::memory_order_relaxed);
				prev = out.id > prev ? out.id : prev;
				last_seen.store(out.id, std::memory_order_relaxed);
			}
		}
	});

	std::this_thread::sleep_for(std::chrono::seconds(1));
	const uint64_t mid = last_seen.load();
	std::this_thread::sleep_for(std::chrono::seconds(1));
	const uint64_t end = last_seen.load();
	stop.store(true, std::memory_order_relaxed);
	writer.join();
	reader.join();

	check(mid > 0 and end > mid, "the reader kept advancing rather than stalling");
	check(backwards.load() == 0, "and never went further back than the ring holds");
}

// A deliberately fat frame, so that one publication takes long enough for a reader to
// arrive in the middle of it. The ring stores its payload word by word, so the write
// window scales with the payload; at 128 kB it is wide enough to hit reliably without
// any hook into the class under test.
struct big_frame
{
	static constexpr size_t words = 16384;
	uint64_t id = 0;
	uint64_t w[words]{};

	bool consistent() const
	{
		for (size_t i = 0; i < words; ++i)
			if (w[i] != id * 0x9e3779b97f4a7c15ull + i)
				return false;
		return true;
	}
	static big_frame make(uint64_t id)
	{
		big_frame f;
		f.id = id;
		for (size_t i = 0; i < words; ++i)
			f.w[i] = id * 0x9e3779b97f4a7c15ull + i;
		return f;
	}
};

void part_e()
{
	std::printf("\nPart E: a slot caught mid-write yields the previous frame\n");

	// The instant the sequence lock exists for: a reader sampling a slot whose write is
	// half done. It must not see the mixture, must not wait for the writer, and must
	// not come back empty while an older complete frame is still in the ring -- it must
	// step back and return that one.
	//
	// Provoked with a payload big enough that the write window is wide, rather than by
	// poking the sequence numbers, so what is exercised is the class's own ordering.
	static auto ring = std::make_unique<wivrn::latest_complete_ring<big_frame, 4>>();
	for (uint64_t i = 1; i <= 3; ++i)
		ring->publish(big_frame::make(i));

	std::atomic<bool> go{false};
	std::atomic<bool> done{false};
	std::atomic<uint64_t> saw_previous{0};
	std::atomic<uint64_t> saw_new{0};
	std::atomic<uint64_t> torn{0};
	std::atomic<uint64_t> empty{0};

	std::thread writer([&] {
		while (not go.load())
			std::this_thread::yield();
		// Several publications of the SAME id, so the reader has many chances to
		// land inside a write while the previous complete frame is still id 3.
		for (int k = 0; k < 200; ++k)
			ring->publish(big_frame::make(4));
		done.store(true);
	});

	std::thread reader([&] {
		auto out = std::make_unique<big_frame>();
		go.store(true);
		while (not done.load())
		{
			if (ring->load_latest(*out))
			{
				if (not out->consistent())
					torn.fetch_add(1, std::memory_order_relaxed);
				else if (out->id == 3)
					saw_previous.fetch_add(1, std::memory_order_relaxed);
				else if (out->id == 4)
					saw_new.fetch_add(1, std::memory_order_relaxed);
			}
			else
			{
				empty.fetch_add(1, std::memory_order_relaxed);
			}
		}
	});

	writer.join();
	reader.join();

	std::printf("  saw previous %llu, saw new %llu, torn %llu, empty %llu\n",
	            (unsigned long long)saw_previous.load(),
	            (unsigned long long)saw_new.load(),
	            (unsigned long long)torn.load(),
	            (unsigned long long)empty.load());

	check(torn.load() == 0, "no torn frame while a fat write is in flight");
	check(saw_previous.load() > 0,
	      "a reader arriving mid-write stepped back to the previous complete frame");
	check(empty.load() == 0, "and never came back empty while one was still held");

	auto after = std::make_unique<big_frame>();
	check(ring->load_latest(*after) and after->id == 4,
	      "once the writes complete the new frame is the one returned");
	check(after->consistent(), "and it is consistent");
}

} // namespace

int main()
{
	std::printf("frame_ring: the decode-to-display hand-over\n");
	part_a();
	part_b();
	part_c();
	part_d();
	part_e();
	std::printf("\n%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
