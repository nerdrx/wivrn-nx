// Audio on the loss-tolerant path: the sequence tracker and the packet loss
// concealment, checked without a socket, a headset or an audio device.
//
// Part A: an unbroken stream. Every packet is delivered byte for byte, nothing is
// concealed and nothing is dropped — turning the toggle on must not change a single
// sample while the network behaves.
// Part B: a single gap. The missing span is synthesized from the last packet under
// a linear fade whose slope is 1/conceal_fade, the length is the lost packets'
// worth of PCM, and the first real packet after it is ramped from the gain the
// concealment ended at back to unity.
// Part C: reordering and duplicates. A packet whose slot has already been played is
// dropped, whichever way it arrives, and dropping it must not move the tracker.
// Part D: long gaps. Past conceal_fade the concealment is exactly silence, and past
// conceal_max nothing is synthesized at all: the consumer's own buffer takes over.
// Part E: the wrap of the 16 bit counter, and the resync when a sender restarts.
// Part F: packet shaping — the split of a capture buffer into datagram sized,
// frame aligned pieces, and the round trip of the sequence number through the real
// serialization.
//
// Build (the boost include is wherever the build fetched it):
//   g++ -std=c++23 -I common -I build-client/common -I external \
//       -I build-client/_deps/boost-src/libs/pfr/include \
//       -o audio_plc_test tests/audio_plc_test.cpp common/smp.cpp -lcrypto
//   ./audio_plc_test

#include "audio_plc.h"
#include "wivrn_packets.h"
#include "wivrn_serialization.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>
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

// What the server's pipewire sink and the headset's speaker actually run at
constexpr uint32_t rate = 48000;
constexpr uint8_t channels = 2;
constexpr size_t frame_bytes = channels * sizeof(int16_t);
// One 5 ms packet, the quantum audio_pipewire asks pipewire for
constexpr size_t packet_frames = rate * 5 / 1000;
constexpr size_t packet_bytes = packet_frames * frame_bytes;

std::vector<uint8_t> make_packet(int16_t base, size_t frames = packet_frames)
{
	std::vector<uint8_t> out(frames * frame_bytes);
	for (size_t f = 0; f < frames; ++f)
	{
		for (size_t c = 0; c < channels; ++c)
		{
			// Non-zero everywhere, so that a fade to silence is unmistakable
			const int16_t s = int16_t(base + int16_t(f % 97) * 100 + int16_t(c) * 7);
			std::memcpy(out.data() + (f * channels + c) * sizeof(int16_t), &s, sizeof(int16_t));
		}
	}
	return out;
}

int16_t sample_at(std::span<const uint8_t> pcm, size_t index)
{
	int16_t s;
	std::memcpy(&s, pcm.data() + index * sizeof(int16_t), sizeof(int16_t));
	return s;
}

size_t frames_of(size_t bytes)
{
	return bytes / frame_bytes;
}

constexpr size_t fade_frames = rate * size_t(audio_plc::conceal_fade.count()) / 1000;
constexpr size_t max_frames = rate * size_t(audio_plc::conceal_max.count()) / 1000;

// ---------------------------------------------------------------------------
// Part A: an unbroken stream is delivered untouched
// ---------------------------------------------------------------------------
void test_in_order_passthrough()
{
	std::printf("Part A: in-order passthrough\n");

	audio_plc plc(rate, channels);

	for (int i = 0; i < 200; ++i)
	{
		auto pcm = make_packet(int16_t(1000 + i));
		const auto reference = pcm;

		auto r = plc.receive(uint16_t(i), pcm);

		CHECK(not r.drop);
		CHECK(r.concealment.empty());
		// Byte for byte what the sender put on the wire
		CHECK(pcm == reference);
	}

	// A packet with no sequence number resets the tracker, so that a peer moving
	// audio back to the control socket and then back onto the stream socket does
	// not read as a gap of thousands of packets
	plc.reset();
	auto pcm = make_packet(7);
	auto r = plc.receive(40000, pcm);
	CHECK(not r.drop);
	CHECK(r.concealment.empty());
}

// ---------------------------------------------------------------------------
// Part B: one gap, concealed
// ---------------------------------------------------------------------------
void test_single_gap()
{
	std::printf("Part B: single gap concealment\n");

	audio_plc plc(rate, channels);

	auto first = make_packet(500);
	CHECK(plc.receive(0, first).concealment.empty());

	// Packets 1 and 2 are lost, 3 arrives
	auto next = make_packet(900);
	const auto next_reference = next;
	auto r = plc.receive(3, next);

	CHECK(not r.drop);
	// Two lost packets, each estimated at the size of the last one received
	CHECK(r.concealment.size() == 2 * packet_bytes);

	// The concealment is the last packet repeated under a linear fade of slope
	// 1/conceal_fade: frame f of the concealment is frame (f % packet_frames) of
	// the source, scaled by 1 - f/fade_frames
	bool envelope_ok = true;
	bool repetition_ok = true;
	for (size_t f = 0; f < frames_of(r.concealment.size()); ++f)
	{
		const float g = 1.f - float(f) / float(fade_frames);
		for (size_t c = 0; c < channels; ++c)
		{
			const size_t i = f * channels + c;
			const int16_t src = sample_at(first, (i % (first.size() / sizeof(int16_t))));
			const int16_t got = sample_at(r.concealment, i);
			const int16_t want = int16_t(std::lround(src * g));

			if (got != want)
				envelope_ok = false;
			// The material really is the previous packet, not silence
			if (src != 0 and got == 0)
				repetition_ok = false;
		}
	}
	CHECK(envelope_ok);
	CHECK(repetition_ok);

	// The gap is well short of conceal_fade, so the concealment ends near full
	// level and the ramp back up is correspondingly gentle
	const float end_gain = 1.f - float(frames_of(r.concealment.size())) / float(fade_frames);
	CHECK(end_gain > 0.8f and end_gain < 1.f);

	// The first real packet after the gap is ramped from end_gain to unity over
	// fade_in, and is untouched after that
	const size_t ramp = rate * size_t(audio_plc::fade_in.count()) / 1000;
	bool ramp_ok = true;
	for (size_t f = 0; f < ramp; ++f)
	{
		const float g = end_gain + (1.f - end_gain) * float(f) / float(ramp);
		for (size_t c = 0; c < channels; ++c)
		{
			const size_t i = f * channels + c;
			if (sample_at(next, i) != int16_t(std::lround(sample_at(next_reference, i) * g)))
				ramp_ok = false;
		}
	}
	CHECK(ramp_ok);
	CHECK(std::memcmp(next.data() + ramp * frame_bytes,
	                  next_reference.data() + ramp * frame_bytes,
	                  next.size() - ramp * frame_bytes) == 0);

	// The stream picks up again from where the gap left it
	auto after = make_packet(1234);
	const auto after_reference = after;
	auto r2 = plc.receive(4, after);
	CHECK(not r2.drop);
	CHECK(r2.concealment.empty());
	CHECK(after == after_reference);
}

// ---------------------------------------------------------------------------
// Part C: reordering and duplicates
// ---------------------------------------------------------------------------
void test_reorder_and_duplicates()
{
	std::printf("Part C: reordering and duplicates\n");

	audio_plc plc(rate, channels);

	auto p0 = make_packet(100);
	CHECK(not plc.receive(0, p0).drop);

	// 2 overtakes 1: the gap it opens is concealed
	auto p2 = make_packet(200);
	auto r2 = plc.receive(2, p2);
	CHECK(not r2.drop);
	CHECK(r2.concealment.size() == packet_bytes);

	// 1 turns up late. Its slot has been played; there is nowhere to put it
	auto p1 = make_packet(300);
	const auto p1_reference = p1;
	auto r1 = plc.receive(1, p1);
	CHECK(r1.drop);
	CHECK(r1.concealment.empty());
	// A dropped packet is left alone
	CHECK(p1 == p1_reference);

	// ...and the tracker did not move: 3 is still the next one expected
	auto p3 = make_packet(400);
	auto r3 = plc.receive(3, p3);
	CHECK(not r3.drop);
	CHECK(r3.concealment.empty());

	// An exact repeat of a packet already played is a duplicate
	auto dup = make_packet(400);
	CHECK(plc.receive(3, dup).drop);
	CHECK(plc.receive(0, dup).drop);

	// A straggler right at the edge of the window is still a straggler, one past
	// it is a sender that restarted
	audio_plc edge(rate, channels);
	auto e = make_packet(1);
	edge.receive(1000, e);
	CHECK(edge.receive(uint16_t(1001 - audio_plc::reorder_window + 1), e).drop);
	auto restart = edge.receive(uint16_t(1001 - audio_plc::reorder_window - 1), e);
	CHECK(not restart.drop);
	CHECK(restart.concealment.empty());
}

// ---------------------------------------------------------------------------
// Part D: long gaps
// ---------------------------------------------------------------------------
void test_long_gap()
{
	std::printf("Part D: long gap clamps\n");

	// A gap longer than conceal_fade but shorter than conceal_max: the tail past
	// conceal_fade is exactly silence
	{
		audio_plc plc(rate, channels);
		auto p0 = make_packet(600);
		plc.receive(0, p0);

		// 30 packets of 5 ms = 150 ms, inside conceal_max (200 ms)
		auto p = make_packet(700);
		auto r = plc.receive(31, p);
		CHECK(r.concealment.size() == 30 * packet_bytes);
		CHECK(frames_of(r.concealment.size()) > fade_frames);

		bool silent_tail = true;
		for (size_t f = fade_frames; f < frames_of(r.concealment.size()); ++f)
		{
			for (size_t c = 0; c < channels; ++c)
			{
				if (sample_at(r.concealment, f * channels + c) != 0)
					silent_tail = false;
			}
		}
		CHECK(silent_tail);

		// The concealment reached silence, so the real packet is ramped from zero
		CHECK(sample_at(p, 0) == 0);
	}

	// A gap longer than conceal_max: nothing past it is synthesized at all
	{
		audio_plc plc(rate, channels);
		auto p0 = make_packet(600);
		plc.receive(0, p0);

		// 5 seconds' worth of packets
		auto p = make_packet(700);
		auto r = plc.receive(1001, p);
		CHECK(frames_of(r.concealment.size()) == max_frames);
		CHECK(not r.drop);
	}

	// A gap wider than resync_ahead is not a gap, it is a restart: no concealment
	{
		audio_plc plc(rate, channels);
		auto p0 = make_packet(600);
		plc.receive(0, p0);

		auto p = make_packet(700);
		const auto reference = p;
		auto r = plc.receive(uint16_t(audio_plc::resync_ahead + 2), p);
		CHECK(not r.drop);
		CHECK(r.concealment.empty());
		CHECK(p == reference);
	}
}

// ---------------------------------------------------------------------------
// Part E: the counter wraps
// ---------------------------------------------------------------------------
void test_wraparound()
{
	std::printf("Part E: wrap-around at 65535\n");

	audio_plc plc(rate, channels);

	// Walk over the wrap in order: nothing is lost, nothing is concealed
	uint16_t seq = 65530;
	for (int i = 0; i < 12; ++i, ++seq)
	{
		auto pcm = make_packet(int16_t(i));
		const auto reference = pcm;
		auto r = plc.receive(seq, pcm);
		CHECK(not r.drop);
		CHECK(r.concealment.empty());
		CHECK(pcm == reference);
	}

	// A gap straddling the wrap is one gap, not a jump of 65534 packets
	{
		audio_plc p(rate, channels);
		auto a = make_packet(11);
		p.receive(65534, a);
		auto b = make_packet(22);
		auto r = p.receive(1, b); // 65535 and 0 lost
		CHECK(not r.drop);
		CHECK(r.concealment.size() == 2 * packet_bytes);
	}

	// A straggler from before the wrap is still a straggler after it
	{
		audio_plc p(rate, channels);
		auto a = make_packet(33);
		p.receive(65534, a);
		auto b = make_packet(44);
		p.receive(2, b);
		auto c = make_packet(55);
		CHECK(p.receive(65535, c).drop);
		CHECK(p.receive(0, c).drop);
	}
}

// ---------------------------------------------------------------------------
// Part F: packet shaping and the wire
// ---------------------------------------------------------------------------
std::vector<uint8_t> flatten(serialization_packet & packet)
{
	std::vector<uint8_t> flat;
	for (const auto & span: static_cast<std::vector<std::span<uint8_t>> &>(packet))
		flat.insert(flat.end(), span.begin(), span.end());
	return flat;
}

template <typename T>
T round_trip(const T & value, size_t * wire_size = nullptr)
{
	serialization_packet packet;
	packet.serialize(value);
	auto flat = flatten(packet);
	if (wire_size)
		*wire_size = flat.size();

	auto memory = std::shared_ptr<uint8_t[]>(new uint8_t[flat.size() + 1]);
	std::memcpy(memory.get(), flat.data(), flat.size());

	deserialization_packet in{memory, std::span<uint8_t>(memory.get(), flat.size())};
	return in.deserialize<T>();
}

void test_packet_shape()
{
	std::printf("Part F: packet shaping\n");

	// A packet is a whole number of frames, and a whole datagram stays well under
	// any MTU worth worrying about
	const size_t chunk = audio_frames_per_packet(frame_bytes, audio_data::max_payload_size) * frame_bytes;
	CHECK(chunk % frame_bytes == 0);
	CHECK(chunk <= audio_data::max_payload_size);
	CHECK(chunk + frame_bytes > audio_data::max_payload_size);

	// The 5 ms quantum the audio backends ask for fits in one packet at 48 kHz
	// stereo, so the common case is not split at all
	CHECK(packet_bytes <= chunk);

	// A capture buffer bigger than that is cut into frame aligned pieces that
	// cover it exactly
	{
		auto pcm = make_packet(1, 4096); // 4096 frames, 16 KiB
		size_t covered = 0;
		size_t pieces = 0;
		size_t largest = 0;
		for (size_t offset = 0; offset < pcm.size(); offset += chunk)
		{
			const size_t size = std::min(chunk, pcm.size() - offset);
			CHECK(size % frame_bytes == 0);
			covered += size;
			largest = std::max(largest, size);
			++pieces;
		}
		CHECK(covered == pcm.size());
		CHECK(pieces > 1);
		CHECK(largest <= audio_data::max_payload_size);
	}

	// The sequence number survives the wire, and the whole datagram (framing and
	// all) stays under the MTU and under the receiver's 2048 byte slot
	{
		auto pcm = make_packet(9, chunk / frame_bytes);
		size_t wire = 0;
		audio_data sent{
		        .timestamp = 123'456'789,
		        .seq = 65535,
		        .payload = std::span(pcm),
		};
		auto got = round_trip(sent, &wire);
		CHECK(got.seq.has_value() and *got.seq == 65535);
		CHECK(got.timestamp == sent.timestamp);
		CHECK(got.payload.size_bytes() == pcm.size());
		CHECK(std::memcmp(got.payload.data(), pcm.data(), pcm.size()) == 0);
		CHECK(wire < 1300);
		CHECK(wire < 2048);
	}

	// A packet on the control path carries no sequence number, and costs one byte
	// more than the upstream packet did
	{
		auto pcm = make_packet(9, 16);
		audio_data sent{
		        .timestamp = 42,
		        .payload = std::span(pcm),
		};
		auto got = round_trip(sent);
		CHECK(not got.seq.has_value());
		CHECK(got.payload.size_bytes() == pcm.size());
	}
}

} // namespace

int main()
{
	test_in_order_passthrough();
	test_single_gap();
	test_reorder_and_duplicates();
	test_long_gap();
	test_wraparound();
	test_packet_shape();

	std::printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
