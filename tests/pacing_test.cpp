// Packet pacing and Wi-Fi QoS test harness.
//
// Part A drives wivrn::shard_pacer, the leaky-bucket schedule the video sender
// thread follows: given a frame size, a budget and the micro-burst granularity,
// the deadlines must be monotone, must start at the frame's start time, must
// stay inside the budget, and must degenerate correctly when the budget is tiny
// or the frame is small.
// Part B drives wivrn::pacing_slot, which shares one frame slot's window
// between the several video streams that drain through the same socket.
// Part C checks wivrn::set_socket_tos on real loopback sockets of both address
// families, reading the mark back with getsockopt.
//
// Build:
//   g++ -std=c++23 -I server/encoder -I common -o pacing_test tests/pacing_test.cpp && ./pacing_test

#include "shard_pacer.h"
#include "socket_tos.h"

#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace wivrn;

namespace
{
int failures = 0;
int checks = 0;

void check(bool ok, const std::string & what)
{
	++checks;
	if (ok)
		return;
	++failures;
	std::printf("  FAIL: %s\n", what.c_str());
}

constexpr int64_t ms = 1'000'000;

// 90 fps
constexpr int64_t period = 11'111'111;
// 150 Mbit/s spread over 90 frames, per stream
constexpr size_t frame_bytes = 208 * 1024;

// Replays a whole frame through the pacer, one 1400 byte shard at a time, on a
// virtual clock that only advances when the pacer asks for a sleep (i.e. the
// sends themselves are free). Returns every deadline it was told to sleep to.
std::vector<int64_t> replay(shard_pacer & pacer, size_t total, int64_t start, size_t shard = 1400)
{
	std::vector<int64_t> waits;
	int64_t now = start;

	for (size_t sent = 0; sent < total;)
	{
		sent = std::min(sent + shard, total);
		if (auto at = pacer.wait_until(sent, now))
		{
			waits.push_back(*at);
			now = *at;
		}
	}

	return waits;
}

void part_a()
{
	std::printf("Part A: shard_pacer schedule\n");

	// --- Nominal frame: 208 kB spread over 40% of a 90 fps frame period ----
	{
		const int64_t start = 1'000'000'000;
		const int64_t budget = int64_t(period * 0.4);
		shard_pacer p(start, budget, frame_bytes);

		check(p.active(), "a nominal frame is paced");
		check(p.deadline(0) == start, "the first byte goes out immediately");
		check(p.deadline(frame_bytes) == start + budget, "the last byte goes out at the end of the budget");

		// 208 kB / 12 kB = 17.33 -> 18 groups
		check(p.group_count() == 18, "208 kB is split into 18 micro-bursts, got " + std::to_string(p.group_count()));

		bool monotone = true;
		bool in_budget = true;
		for (size_t i = 0; i < p.group_count(); ++i)
		{
			const int64_t d = p.group_deadline(i);
			if (i and d < p.group_deadline(i - 1))
				monotone = false;
			if (d < start or d > start + budget)
				in_budget = false;
		}
		check(monotone, "group deadlines are monotone");
		check(in_budget, "every group deadline is inside [start, start + budget]");

		// Byte level monotonicity, at a finer grain than the groups
		bool byte_monotone = true;
		for (size_t off = 0; off + 1024 <= frame_bytes; off += 1024)
		{
			if (p.deadline(off + 1024) < p.deadline(off))
				byte_monotone = false;
		}
		check(byte_monotone, "byte deadlines are monotone");

		auto waits = replay(p, frame_bytes, start);
		check(waits.size() == p.group_count() - 1,
		      "one sleep per group but the first, got " + std::to_string(waits.size()));
		check(not waits.empty() and waits.back() <= start + budget, "the last sleep is still inside the budget");
		check(not waits.empty() and waits.front() > start, "the first sleep is after the start");

		// The interval between wakeups must be worth a syscall
		bool spaced = true;
		for (size_t i = 1; i < waits.size(); ++i)
		{
			if (waits[i] - waits[i - 1] < shard_pacer::min_sleep_ns)
				spaced = false;
		}
		check(spaced, "wakeups are at least min_sleep_ns apart");

		// ~4.4 ms budget over 18 groups is ~260 us per wakeup
		if (waits.size() > 1)
		{
			const int64_t step = waits[1] - waits[0];
			check(step > 200'000 and step < 320'000,
			      "the nominal wakeup interval is ~260 us, got " + std::to_string(step / 1000) + " us");
		}
	}

	// --- Completion never past the frame period ----------------------------
	{
		const int64_t start = 0;
		for (float w: {0.1f, 0.25f, 0.4f, 0.5f})
		{
			shard_pacer p(start, int64_t(period * w), frame_bytes);
			check(p.deadline(frame_bytes) < period,
			      "a window of " + std::to_string(w) + " completes inside a frame period");
		}
	}

	// --- Degenerate: no budget left (the encoder delivered late) -----------
	{
		shard_pacer p(500, 0, frame_bytes);
		check(not p.active(), "a frame with no budget is not paced");
		check(p.deadline(0) == 500 and p.deadline(frame_bytes) == 500, "every byte of it goes out at once");
		check(p.group_count() == 1, "it is a single burst");
		check(replay(p, frame_bytes, 500).empty(), "it never sleeps");
	}

	// --- Degenerate: frame smaller than one micro-burst --------------------
	{
		shard_pacer p(0, 4 * ms, shard_pacer::group_bytes);
		check(not p.active(), "a frame of exactly one group is not paced");
		check(replay(p, shard_pacer::group_bytes, 0).empty(), "and never sleeps");

		shard_pacer q(0, 4 * ms, shard_pacer::group_bytes + 1);
		check(q.active(), "one byte more and it is");
	}

	// --- Degenerate: enormous frame, per-group interval below the floor ----
	{
		// An 8 MB frame in 4.4 ms is 683 groups of 6.5 us each: sleeping once
		// per group would be pointless, so the min_sleep_ns floor coalesces
		// them and caps the wakeup rate whatever the frame size.
		const int64_t budget = int64_t(period * 0.4);
		const size_t huge = 8 * 1024 * 1024;
		shard_pacer p(0, budget, huge);
		check(p.active(), "a huge frame still has a schedule");
		check(p.deadline(huge) == budget, "which still ends within the budget");
		check(p.group_count() > 600, "and has hundreds of groups");

		auto waits = replay(p, huge, 0);
		check(waits.size() < p.group_count() / 10,
		      "yet sleeps far less often than once per group, got " + std::to_string(waits.size()));
		check(int64_t(waits.size()) <= budget / shard_pacer::min_sleep_ns + 1,
		      "the wakeup rate is capped by min_sleep_ns, got " + std::to_string(waits.size()));
		check(not waits.empty() and waits.back() <= budget, "and the last one is still inside the budget");
	}

	// --- Degenerate: budget far larger than any sane window ----------------
	{
		// Not something the server can produce (the window is clamped), but the
		// schedule must stay well formed for it.
		shard_pacer p(0, 10 * period, frame_bytes);
		check(p.active(), "an absurd budget is still a schedule");
		check(p.deadline(frame_bytes) == 10 * period, "that ends exactly at the budget");
		auto waits = replay(p, frame_bytes, 0);
		check(waits.size() == p.group_count() - 1, "with one sleep per group but the first");
		check(std::is_sorted(waits.begin(), waits.end()), "and monotone deadlines");
	}

	// --- The clock running late must not make the pacer sleep backwards ----
	{
		shard_pacer p(0, int64_t(period * 0.4), frame_bytes);
		// Every send takes far longer than its slot: nothing should be asked for
		int64_t now = 0;
		size_t sleeps = 0;
		for (size_t sent = 0; sent < frame_bytes;)
		{
			sent = std::min(sent + 1400, frame_bytes);
			now += 500'000; // 500 us per shard, way over budget
			if (auto at = p.wait_until(sent, now))
			{
				++sleeps;
				check(*at > now, "a sleep is never into the past");
			}
		}
		check(sleeps == 0, "a sender already behind its schedule never sleeps, got " + std::to_string(sleeps));
	}

	// --- max_window keeps the automatic bitrate's probe-up path alive ------
	check(shard_pacer::max_window < 0.6f, "the window ceiling stays under the utilisation_increase threshold");
}

void part_b()
{
	std::printf("Part B: pacing_slot window sharing\n");

	const float window = 0.4f;
	const int64_t full = int64_t(period * window);

	// --- One stream: the whole window every frame --------------------------
	{
		pacing_slot slot;
		int64_t t = 1'000'000'000;
		bool all_full = true;
		for (int i = 0; i < 10; ++i, t += period)
		{
			if (slot.begin_frame(t, period, window, 0) != full)
				all_full = false;
		}
		check(all_full, "a single stream gets the whole window on every frame");
	}

	// --- Three streams finishing together share one window -----------------
	{
		pacing_slot slot;
		const int64_t t = 2'000'000'000;

		const int64_t a = slot.begin_frame(t, period, window, 2);
		const int64_t b = slot.begin_frame(t + a, period, window, 1);
		const int64_t c = slot.begin_frame(t + a + b, period, window, 0);

		check(a == full / 3, "the first of three frames gets a third of the window");
		check(a > 0 and b > 0 and c > 0, "and none of the three is left unpaced");
		check(a + b + c <= full, "together they never exceed one window, got " + std::to_string(a + b + c));
		check(t + a + b + c <= t + int64_t(period * shard_pacer::max_window),
		      "so the slot always completes well inside the frame period");
	}

	// --- A late encoder loses the window it did not use --------------------
	{
		pacing_slot slot;
		const int64_t t = 3'000'000'000;

		check(slot.begin_frame(t, period, window, 0) == full, "the frame that opens the slot gets the window");
		// A second frame of the same slot arriving after the window is spent
		check(slot.begin_frame(t + full + ms, period, window, 0) == 0,
		      "a frame arriving after the window is spent is blasted");
		// A frame a whole period later opens a new slot
		check(slot.begin_frame(t + period, period, window, 0) == full,
		      "a frame a period later opens a fresh slot");
	}

	// --- No frame period, no pacing ----------------------------------------
	{
		pacing_slot slot;
		check(slot.begin_frame(1000, 0, window, 0) == 0, "an unknown frame period disables pacing");
		check(slot.begin_frame(1000, period, 0, 0) == 0, "a zero window disables pacing");
	}

	// --- The configured window is clamped ----------------------------------
	{
		pacing_slot slot;
		const int64_t budget = slot.begin_frame(0, period, 5.0f, 0);
		check(budget == int64_t(period * shard_pacer::max_window),
		      "an out of range window is clamped to max_window");
		check(budget < period, "which is still inside a frame period");
	}

	// --- reset() forgets the slot, as the path switch does -----------------
	{
		pacing_slot slot;
		slot.begin_frame(0, period, window, 0);
		check(slot.begin_frame(ms, period, window, 0) < full, "the same slot is shared");
		slot.reset();
		check(slot.begin_frame(2 * ms, period, window, 0) == full, "reset opens a fresh slot");
	}
}

// Reads the mark back from the option that applies to `domain`
int read_tos(int fd, int level, int option)
{
	int value = -1;
	socklen_t len = sizeof(value);
	if (getsockopt(fd, level, option, &value, &len) < 0)
		return -1;
	return value;
}

void part_c()
{
	std::printf("Part C: DSCP marks on real sockets\n");

	struct
	{
		const char * name;
		int domain;
		int type;
	} cases[] = {
	        {"IPv6 UDP", AF_INET6, SOCK_DGRAM},
	        {"IPv6 TCP", AF_INET6, SOCK_STREAM},
	        {"IPv4 UDP", AF_INET, SOCK_DGRAM},
	        {"IPv4 TCP", AF_INET, SOCK_STREAM},
	};

	for (const auto & c: cases)
	{
		int fd = ::socket(c.domain, c.type, 0);
		if (fd < 0)
		{
			std::printf("  SKIP: %s unavailable: %s\n", c.name, strerror(errno));
			continue;
		}

		check(set_socket_tos(fd, tos::dscp_ef), std::string(c.name) + ": EF is accepted");
		check(read_tos(fd, IPPROTO_IP, IP_TOS) == tos::dscp_ef,
		      std::string(c.name) + ": IP_TOS reads back EF");
		if (c.domain == AF_INET6)
			check(read_tos(fd, IPPROTO_IPV6, IPV6_TCLASS) == tos::dscp_ef,
			      std::string(c.name) + ": IPV6_TCLASS reads back EF");

		check(set_socket_tos(fd, tos::dscp_af41), std::string(c.name) + ": AF41 is accepted");
		check(read_tos(fd, IPPROTO_IP, IP_TOS) == tos::dscp_af41,
		      std::string(c.name) + ": IP_TOS reads back AF41");
		if (c.domain == AF_INET6)
			check(read_tos(fd, IPPROTO_IPV6, IPV6_TCLASS) == tos::dscp_af41,
			      std::string(c.name) + ": IPV6_TCLASS reads back AF41");

		check(set_socket_tos(fd, tos::best_effort), std::string(c.name) + ": clearing is accepted");
		check(read_tos(fd, IPPROTO_IP, IP_TOS) == 0, std::string(c.name) + ": IP_TOS is cleared");
		if (c.domain == AF_INET6)
			check(read_tos(fd, IPPROTO_IPV6, IPV6_TCLASS) == 0,
			      std::string(c.name) + ": IPV6_TCLASS is cleared");

		::close(fd);
	}

	// The mark survives a connect, which is when the server applies it
	{
		int a = ::socket(AF_INET6, SOCK_DGRAM, 0);
		int b = ::socket(AF_INET6, SOCK_DGRAM, 0);
		if (a >= 0 and b >= 0)
		{
			sockaddr_in6 addr{};
			addr.sin6_family = AF_INET6;
			addr.sin6_addr = in6addr_loopback;
			addr.sin6_port = 0;

			socklen_t len = sizeof(addr);
			check(::bind(b, (sockaddr *)&addr, sizeof(addr)) == 0, "loopback bind");
			check(::getsockname(b, (sockaddr *)&addr, &len) == 0, "loopback getsockname");
			check(::connect(a, (sockaddr *)&addr, sizeof(addr)) == 0, "loopback connect");

			set_socket_tos(a, tos::dscp_af41);
			const char payload[] = "shard";
			check(::send(a, payload, sizeof(payload), 0) == ssize_t(sizeof(payload)),
			      "a marked datagram is still sendable over loopback");
			check(read_tos(a, IPPROTO_IPV6, IPV6_TCLASS) == tos::dscp_af41,
			      "the mark survives sending");
		}
		if (a >= 0)
			::close(a);
		if (b >= 0)
			::close(b);
	}

	check(tos::dscp_ef == 0xb8, "EF is the 0xb8 TOS byte");
	check(tos::dscp_af41 == 0x88, "AF41 is the 0x88 TOS byte");
}
} // namespace

int main()
{
	part_a();
	part_b();
	part_c();

	std::printf("\n%d checks, %d failure(s)\n", checks, failures);
	return failures ? 1 : 0;
}
