// Multipath stage 2 test harness.
//
// Part A drives wivrn::path_selector (the state machine shared by the server's
// wivrn_connection and the headset's wivrn_session) on a virtual clock.
// Part B wires it to two real socketpairs standing in for the primary and the
// secondary path, kills the primary, and checks that sends land on the
// secondary and come back to the primary once it has been healthy long enough.
//
// Build:
//   g++ -std=c++23 -I common -o failover_test failover_test.cpp && ./failover_test

#include "path_selector.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace std::chrono_literals;
using wivrn::path_selector;
using clock_t_ = path_selector::clock;

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                      \
	do                                                                   \
	{                                                                    \
		++checks;                                                        \
		if (not(cond))                                                   \
		{                                                                \
			++failures;                                                  \
			std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                \
	} while (0)

namespace
{

// --- Part A ---------------------------------------------------------------

// A selector on a virtual clock, so that a 5 s hysteresis costs no wall time
struct harness
{
	path_selector sel;
	clock_t_::time_point now = clock_t_::time_point{} + 1h;

	harness()
	{
		sel.reset(now);
		sel.set_secondary_usable(true);
	}

	// Advance, feeding a keepalive on the primary every 250 ms if alive
	std::vector<path_selector::switch_event> run(std::chrono::milliseconds duration, bool primary_alive)
	{
		std::vector<path_selector::switch_event> events;
		auto end = now + duration;
		while (now < end)
		{
			now += 20ms;
			// The headset pings every 250 ms; approximate with one packet
			// every 240 ms so that it always beats the 400 ms deadline
			static int tick = 0;
			if (primary_alive and ++tick % 12 == 0)
				sel.on_primary_received(false, now);
			if (auto e = sel.update(now))
				events.push_back(*e);
		}
		return events;
	}
};

void test_flip_on_silence()
{
	std::printf("flip on silence\n");
	harness h;

	// Healthy: no switch
	auto events = h.run(2s, true);
	CHECK(events.empty());
	CHECK(not h.sel.on_secondary());

	// Primary goes quiet: flips within ~400-500 ms
	auto before = h.now;
	events = h.run(2s, false);
	CHECK(events.size() == 1);
	CHECK(h.sel.on_secondary());
	if (not events.empty())
	{
		CHECK(events[0].on_secondary);
		std::printf("  flipped after %ld ms, reason: %s\n",
		            (long)std::chrono::duration_cast<std::chrono::milliseconds>(h.now - before).count() - 1500,
		            events[0].reason.c_str());
	}
}

void test_no_flip_without_secondary()
{
	std::printf("no flip without a usable secondary\n");
	harness h;
	h.sel.set_secondary_usable(false);

	auto events = h.run(3s, false);
	CHECK(events.empty());
	CHECK(not h.sel.on_secondary());
}

void test_flip_on_control_send_error()
{
	std::printf("flip on a control send error\n");
	harness h;
	h.run(1s, true);

	CHECK(h.sel.on_control_send_error());
	// A second error does not report again
	CHECK(not h.sel.on_control_send_error());
	h.sel.request(true, "the primary control socket failed");

	auto events = h.run(100ms, true);
	CHECK(events.size() == 1);
	CHECK(h.sel.on_secondary());
	CHECK(not h.sel.control_up());

	// Traffic on the primary stream socket alone must not bring back a control
	// socket that is broken for good
	events = h.run(10s, true);
	CHECK(events.empty());
	CHECK(h.sel.on_secondary());
}

void test_stream_error_grace()
{
	std::printf("datagram send errors are transient\n");
	harness h;
	h.run(1s, true);

	// A single latched error (ICMP unreachable) must not flip anything
	h.sel.on_stream_send_error(h.now);
	auto events = h.run(300ms, true);
	CHECK(events.empty());
	CHECK(not h.sel.on_secondary());

	h.sel.on_stream_send_ok();
	events = h.run(2s, true);
	CHECK(events.empty());

	// A run of errors longer than the grace does flip, even though packets keep
	// arriving on the control socket
	h.sel.on_stream_send_error(h.now);
	events = h.run(1s, true);
	// The flip clears the run: the stream socket is idle from now on
	CHECK(events.size() == 1);
	CHECK(h.sel.on_secondary());
	if (not events.empty())
		std::printf("  reason: %s\n", events[0].reason.c_str());
}

void test_flip_back_hysteresis()
{
	std::printf("flip back only after the hysteresis window\n");
	harness h;
	h.run(1s, true);
	h.run(1s, false); // flip to secondary
	CHECK(h.sel.on_secondary());

	// Primary back, but not for long enough yet
	auto events = h.run(4s, true);
	CHECK(events.empty());
	CHECK(h.sel.on_secondary());

	// A blip during the window restarts it
	events = h.run(600ms, false);
	CHECK(events.empty());
	events = h.run(4s, true);
	CHECK(events.empty());
	CHECK(h.sel.on_secondary());

	events = h.run(1500ms, true);
	CHECK(events.size() == 1);
	CHECK(not h.sel.on_secondary());
	if (not events.empty())
	{
		CHECK(not events[0].on_secondary);
		std::printf("  flipped back, reason: %s, %ld ms after the first switch\n",
		            events[0].reason.c_str(),
		            (long)events[0].since_previous.count());
	}
}

void test_secondary_lost_forces_primary()
{
	std::printf("losing the secondary forces the primary back\n");
	harness h;
	h.run(1s, true);
	h.run(1s, false);
	CHECK(h.sel.on_secondary());

	h.sel.set_secondary_usable(false);
	auto events = h.run(100ms, false);
	CHECK(events.size() == 1);
	CHECK(not h.sel.on_secondary());
	if (not events.empty())
		std::printf("  reason: %s\n", events[0].reason.c_str());
}

void test_reset()
{
	std::printf("reset goes back to the primary\n");
	harness h;
	h.run(1s, true);
	h.run(1s, false);
	CHECK(h.sel.on_secondary());

	h.sel.reset(h.now);
	CHECK(not h.sel.on_secondary());
	CHECK(h.sel.control_up());
	auto events = h.run(100ms, true);
	CHECK(events.empty());
}

// --- Part B: real sockets -------------------------------------------------

// Mirrors the routing of wivrn_connection: control and video go to the primary
// until the selector says otherwise, and a failed send is absorbed when a
// secondary path exists.
struct router
{
	path_selector sel;
	int control = -1; // primary, reliable
	int stream = -1;  // primary, datagram
	int secondary = -1;
	bool secondary_usable = false;
	int fatal_errors = 0;

	// Where the last send went, for the assertions
	enum class went
	{
		none,
		control,
		stream,
		secondary
	};

	went send_control(const std::string & payload)
	{
		if (sel.on_secondary() and secondary >= 0)
			return send_secondary(payload);

		if (::send(control, payload.data(), payload.size(), MSG_NOSIGNAL) < 0)
		{
			sel.on_control_send_error();
			if (secondary_usable)
			{
				sel.request(true, "the primary control socket failed");
				return send_secondary(payload);
			}
			++fatal_errors;
			return went::none;
		}
		return went::control;
	}

	went send_stream(const std::string & payload)
	{
		if (sel.on_secondary() and secondary >= 0)
			return send_secondary(payload);

		if (::send(stream, payload.data(), payload.size(), MSG_NOSIGNAL) < 0)
		{
			sel.on_stream_send_error(path_selector::clock::now());
			return went::none;
		}
		sel.on_stream_send_ok();
		return went::stream;
	}

	went send_secondary(const std::string & payload)
	{
		if (secondary < 0)
			return went::none;
		if (::send(secondary, payload.data(), payload.size(), MSG_NOSIGNAL) < 0)
			return went::none;
		return went::secondary;
	}
};

size_t pending_bytes(int fd)
{
	char buffer[4096];
	ssize_t n = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
	return n > 0 ? size_t(n) : 0;
}

void test_sockets()
{
	std::printf("routing over real sockets\n");

	int primary_control[2];
	int primary_stream[2];
	int secondary[2];
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, primary_control) == 0);
	CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, primary_stream) == 0);
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, secondary) == 0);

	router r;
	r.control = primary_control[0];
	r.stream = primary_stream[0];
	r.secondary = secondary[0];
	r.secondary_usable = true;

	auto now = path_selector::clock::now();
	r.sel.reset(now);
	r.sel.set_secondary_usable(true);

	// Everything goes over the primary while it is healthy
	CHECK(r.send_control("hello") == router::went::control);
	CHECK(r.send_stream("frame") == router::went::stream);
	CHECK(pending_bytes(primary_control[1]) == 5);
	CHECK(pending_bytes(primary_stream[1]) == 5);
	CHECK(pending_bytes(secondary[1]) == 0);

	// Kill the primary: the peer of both sockets goes away
	::close(primary_control[1]);
	::close(primary_stream[1]);

	// The first write after the peer is gone still succeeds on a socketpair;
	// the one after that fails. Either way the send must not be fatal.
	for (int i = 0; i < 4; ++i)
	{
		r.send_control("hello");
		r.send_stream("frame");
	}
	CHECK(r.fatal_errors == 0);
	CHECK(r.sel.on_secondary());
	CHECK(not r.sel.control_up());

	// ... and the video now lands on the secondary
	while (pending_bytes(secondary[1]))
	{
	}
	CHECK(r.send_stream("frame after failover") == router::went::secondary);
	CHECK(r.send_control("control after failover") == router::went::secondary);
	CHECK(pending_bytes(secondary[1]) == std::strlen("frame after failovercontrol after failover"));

	// Flip back: a fresh primary, healthy for the whole hysteresis window
	int new_control[2];
	int new_stream[2];
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, new_control) == 0);
	CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, new_stream) == 0);
	::close(primary_control[0]);
	::close(primary_stream[0]);
	r.control = new_control[0];
	r.stream = new_stream[0];
	r.sel.on_stream_send_ok();

	// A control socket that failed stays down until it receives again
	CHECK(not r.sel.control_up());

	bool flipped_back = false;
	// 6 s of virtual keepalives, 250 ms apart
	auto t = now;
	for (int i = 0; i < 24; ++i)
	{
		t += 250ms;
		r.sel.on_primary_received(i % 2 == 0, t);
		if (auto e = r.sel.update(t); e and not e->on_secondary)
		{
			flipped_back = true;
			std::printf("  flipped back after %d keepalives (%s)\n", i + 1, e->reason.c_str());
			break;
		}
	}
	CHECK(flipped_back);
	CHECK(not r.sel.on_secondary());
	CHECK(r.sel.control_up());

	CHECK(r.send_control("back on the primary") == router::went::control);
	CHECK(pending_bytes(new_control[1]) == std::strlen("back on the primary"));
	CHECK(pending_bytes(secondary[1]) == 0);

	::close(new_control[0]);
	::close(new_control[1]);
	::close(new_stream[0]);
	::close(new_stream[1]);
	::close(secondary[0]);
	::close(secondary[1]);
}

} // namespace

int main()
{
	test_flip_on_silence();
	test_no_flip_without_secondary();
	test_flip_on_control_send_error();
	test_stream_error_grace();
	test_flip_back_hysteresis();
	test_secondary_lost_forces_primary();
	test_reset();
	test_sockets();

	std::printf("\n%d checks, %d failure(s)\n", checks, failures);
	return failures ? 1 : 0;
}
