// Transport HUD: the wire format and the arithmetic behind the headset's Transport page,
// checked without a headset, a server or a link.
//
// Part A: to_headset::transport_status survives a serialization round trip, on its own and
// as an alternative of the packet variant the client actually deserializes — the enums, the
// per-stream software-encoder bitmask and the flags all have to come back unchanged, and
// the variant has to come back as the same alternative.
// Part B: from_headset::transport_status_subscribe does the same, and the lease constants
// are consistent: the server must send faster than the headset renews, and the headset must
// renew comfortably before the lease lapses, or a page that is open will blink.
// Part C: the server's side of the lease. A model of wivrn_session::send_transport_status'
// deadline test — subscribing opens the feed, an explicit unsubscribe closes it at once,
// and silence closes it after the timeout and not before.
// Part D: wivrn::counter_rate and the trend filter behind the page's rates and its RSSI
// arrow (client/transport_rates.h): the first sample, a zero interval, a counter reset
// under the page, and a signal sitting exactly on the dead band.
//
// Build (the boost include is wherever the build fetched it):
//   g++ -std=c++23 -I common -I client -I build-client/common -I external \
//       -I build-client/_deps/boost-src/libs/pfr/include \
//       -o transport_status_test tests/transport_status_test.cpp common/smp.cpp -lcrypto
//   ./transport_status_test

#include "transport_rates.h"
#include "wivrn_packets.h"
#include "wivrn_serialization.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <vector>

using namespace wivrn;

static int failures = 0;
static int checks = 0;

#define CHECK(...)                                                                           \
	do                                                                                   \
	{                                                                                    \
		++checks;                                                                    \
		if (not(__VA_ARGS__))                                                        \
		{                                                                            \
			++failures;                                                          \
			std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__); \
		}                                                                            \
	} while (0)

namespace
{

template <typename T>
T round_trip(const T & value)
{
	serialization_packet packet;
	packet.serialize(value);

	std::vector<uint8_t> flat;
	for (const auto & span: static_cast<std::vector<std::span<uint8_t>> &>(packet))
		flat.insert(flat.end(), span.begin(), span.end());

	auto memory = std::shared_ptr<uint8_t[]>(new uint8_t[flat.size()]);
	std::memcpy(memory.get(), flat.data(), flat.size());

	deserialization_packet in{memory, std::span<uint8_t>(memory.get(), flat.size())};
	return in.deserialize<T>();
}

using controller_state = to_headset::transport_status::controller_state;
using path_state = to_headset::transport_status::path_state;

void test_status_round_trip()
{
	std::printf("Part A: the status packet on the wire\n");

	const to_headset::transport_status sent{
	        .bitrate_bps = 87'500'000,
	        .ceiling_bps = 120'000'000,
	        .mode = bitrate_mode::bbr,
	        .state = controller_state::probe,
	        .path = path_state::wifi_usb_ready,
	        .radio_hold = true,
	        .pacing_active = true,
	        .fec_active = false,
	        // Streams 0 and 2 lost their hardware encoder, stream 1 did not
	        .software_encoders = 0b101,
	};

	const auto got = round_trip(sent);

	CHECK(got.bitrate_bps == sent.bitrate_bps);
	CHECK(got.ceiling_bps == sent.ceiling_bps);
	CHECK(got.mode == bitrate_mode::bbr);
	CHECK(got.state == controller_state::probe);
	CHECK(got.path == path_state::wifi_usb_ready);
	CHECK(got.radio_hold);
	CHECK(got.pacing_active);
	CHECK(not got.fec_active);
	CHECK(got.software_encoders == 0b101);

	// The bitmask is read bit by bit by the page, so check it means what the page reads
	CHECK((got.software_encoders & (1u << 0)) != 0);
	CHECK((got.software_encoders & (1u << 1)) == 0);
	CHECK((got.software_encoders & (1u << 2)) != 0);

	// Every state has to survive: they are the whole point of the packet, and an enum
	// whose value is out of the serializer's range comes back as something else entirely
	for (auto state: {controller_state::off,
	                  controller_state::steady,
	                  controller_state::recovering,
	                  controller_state::startup,
	                  controller_state::probe})
	{
		to_headset::transport_status s{};
		s.state = state;
		CHECK(round_trip(s).state == state);
	}

	for (auto path: {path_state::wifi_only, path_state::wifi_usb_ready, path_state::usb})
	{
		to_headset::transport_status s{};
		s.path = path;
		CHECK(round_trip(s).path == path);
	}

	// The client deserializes the variant, not the struct: a packet that round trips on
	// its own but lands on the wrong alternative would be dispatched to the wrong handler
	const to_headset::packets wrapped{sent};
	const auto back = round_trip(wrapped);
	CHECK(std::holds_alternative<to_headset::transport_status>(back));
	if (auto * p = std::get_if<to_headset::transport_status>(&back))
	{
		CHECK(p->bitrate_bps == sent.bitrate_bps);
		CHECK(p->state == controller_state::probe);
		CHECK(p->software_encoders == sent.software_encoders);
	}

	// An all-default status is what a session that has not decided anything yet sends
	const to_headset::transport_status idle{};
	const auto idle_back = round_trip(idle);
	CHECK(idle_back.bitrate_bps == 0);
	CHECK(not idle_back.radio_hold);
	CHECK(idle_back.software_encoders == 0);
}

void test_subscribe_round_trip()
{
	std::printf("Part B: the subscription on the wire\n");

	CHECK(round_trip(from_headset::transport_status_subscribe{.active = true}).active);
	CHECK(not round_trip(from_headset::transport_status_subscribe{.active = false}).active);

	const from_headset::packets wrapped{from_headset::transport_status_subscribe{.active = true}};
	const auto back = round_trip(wrapped);
	CHECK(std::holds_alternative<from_headset::transport_status_subscribe>(back));

	// The server sends faster than the headset renews, so the page always has something
	// fresher than its own request cadence to show
	CHECK(transport_status_interval < transport_status_refresh);
	// and the headset renews with room to spare, so one lost renewal does not stop the
	// feed. Two full refresh periods have to fit inside the lease.
	CHECK(2 * transport_status_refresh <= transport_status_timeout);
}

// The deadline test at the top of wivrn_session::send_transport_status, on a clock we own
struct lease
{
	using clock = std::chrono::steady_clock;

	int64_t until = 0;

	void subscribe(bool active, clock::time_point now)
	{
		until = active ? (now + transport_status_timeout).time_since_epoch().count() : 0;
	}

	bool sending(clock::time_point now) const
	{
		return until != 0 and now.time_since_epoch().count() < until;
	}
};

void test_lease()
{
	std::printf("Part C: the subscription lease\n");

	const auto t0 = lease::clock::time_point{} + std::chrono::hours(1);
	lease l;

	// Nothing is sent before anyone asks
	CHECK(not l.sending(t0));

	l.subscribe(true, t0);
	CHECK(l.sending(t0));
	CHECK(l.sending(t0 + transport_status_refresh));

	// The page renews well before the lease lapses, so the feed never stops while it is open
	l.subscribe(true, t0 + transport_status_refresh);
	CHECK(l.sending(t0 + transport_status_timeout));
	CHECK(l.sending(t0 + transport_status_refresh + transport_status_timeout - std::chrono::milliseconds(1)));

	// Silence: the feed stops on its own, and not one tick early
	CHECK(not l.sending(t0 + transport_status_refresh + transport_status_timeout));
	CHECK(not l.sending(t0 + std::chrono::hours(1)));

	// Closing the page stops it at once rather than waiting the timeout out
	l.subscribe(true, t0);
	CHECK(l.sending(t0));
	l.subscribe(false, t0);
	CHECK(not l.sending(t0));

	// A headset that vanishes right after subscribing costs exactly one lease of traffic
	l.subscribe(true, t0);
	CHECK(l.sending(t0 + transport_status_timeout - std::chrono::milliseconds(1)));
	CHECK(not l.sending(t0 + transport_status_timeout));
}

void test_rates()
{
	std::printf("Part D: the page's arithmetic\n");

	constexpr int64_t half_second = 500'000'000;

	// 4 shards rebuilt in half a second is 8 a second
	CHECK(counter_rate(100, 104, half_second) == 8);
	// Nothing happened
	CHECK(counter_rate(100, 100, half_second) == 0);
	// Bytes to bits over a whole second
	CHECK(counter_rate(0, 12'500'000, 1'000'000'000) * 8 == 100'000'000);

	// A zero or negative interval is the page being drawn twice on the same timestamp:
	// no division, no infinity on a plot
	CHECK(counter_rate(0, 1000, 0) == 0);
	CHECK(counter_rate(0, 1000, -half_second) == 0);

	// A counter that went backwards is an object that was recreated under the page (a
	// decoder after a stream reconfiguration, the audio handle after a device change).
	// Zero, not an enormous negative rate wrapped into a positive one.
	CHECK(counter_rate(1000, 3, half_second) == 0);

	// Rates are per second whatever the sampling period is
	CHECK(counter_rate(0, 1, 1'000'000'000) == 1);
	CHECK(counter_rate(0, 1, 100'000'000) == 10);

	// The trend filter: seeded with the first sample, both averages agree, no arrow
	float fast = -50, slow = -50;
	CHECK(trend_direction(fast, slow, 0.8f) == 0);

	// Walking away from the access point: the fast average leads the slow one down
	for (int i = 0; i < 8; ++i)
	{
		const float sample = -50.f - float(i + 1) * 2.f;
		fast = ema_step(fast, sample, 0.5f);
		slow = ema_step(slow, sample, 0.08f);
	}
	CHECK(fast < slow);
	CHECK(trend_direction(fast, slow, 0.8f) == -1);

	// Walking back: the same filter has to turn around, and within a handful of samples
	for (int i = 0; i < 8; ++i)
	{
		const float sample = -66.f + float(i + 1) * 2.f;
		fast = ema_step(fast, sample, 0.5f);
		slow = ema_step(slow, sample, 0.08f);
	}
	CHECK(fast > slow);
	CHECK(trend_direction(fast, slow, 0.8f) == 1);

	// A steady signal, however noisy, must not move the arrow
	fast = slow = -60;
	for (int i = 0; i < 40; ++i)
	{
		const float sample = -60.f + (i % 2 ? 0.5f : -0.5f);
		fast = ema_step(fast, sample, 0.5f);
		slow = ema_step(slow, sample, 0.08f);
	}
	CHECK(trend_direction(fast, slow, 0.8f) == 0);

	// The dead band is inclusive: sitting exactly on it reads as steady, so an arrow
	// cannot flicker between two frames on a signal that is not moving
	CHECK(trend_direction(-50.0f, -50.8f, 0.8f) == 0);
	CHECK(trend_direction(-50.8f, -50.0f, 0.8f) == 0);
	CHECK(trend_direction(-50.0f, -50.9f, 0.8f) == 1);
	CHECK(trend_direction(-50.9f, -50.0f, 0.8f) == -1);

	// An average converges on a constant signal rather than drifting past it
	float avg = 0;
	for (int i = 0; i < 200; ++i)
		avg = ema_step(avg, -55.f, 0.5f);
	CHECK(std::fabs(avg + 55.f) < 1e-3f);
}

} // namespace

int main()
{
	test_status_round_trip();
	test_subscribe_round_trip();
	test_lease();
	test_rates();

	std::printf("\n%d checks, %d failure(s)\n", checks, failures);
	return failures ? 1 : 0;
}
