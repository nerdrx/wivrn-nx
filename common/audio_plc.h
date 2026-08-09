/*
 * WiVRn VR streaming
 * Copyright (C) 2026  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2026  Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace wivrn
{

// Number of whole audio frames that fit in one packet's payload. A frame is one
// sample per channel, so a packet always ends on a frame boundary and a receiver
// never has to reassemble anything.
constexpr size_t audio_frames_per_packet(size_t frame_bytes, size_t max_payload)
{
	if (frame_bytes == 0)
		return 0;
	return std::max<size_t>(1, max_payload / frame_bytes);
}

// Receiver side of one direction's audio stream: sequence tracking and packet loss
// concealment. Both ends run one of these — the headset for the speaker, the server
// for the microphone — and both feed it every packet that arrived with a sequence
// number, in arrival order.
//
// What it decides, per packet:
// - the expected number: play it, byte for byte, exactly as the control path would
//   have delivered it;
// - a number ahead of the expected one: datagrams were lost. Emit concealment for
//   the missing span first, then play the packet;
// - a number behind: a duplicate, or a straggler whose span has already been
//   concealed and played. Drop it. Reordering a real-time stream is not possible
//   once its slot is gone, and holding packets back to allow for it would spend
//   exactly the latency this path exists to save;
// - a number far outside either window: the sender restarted (a reconnect, or the
//   toggle flipped). Resynchronize without concealing anything.
//
// The concealment itself is waveform repetition, the cheapest method that is not
// silence: the last packet received is repeated, sample for sample, under a linear
// fade to silence reached at conceal_fade. Short gaps therefore come out at nearly
// full level and long ones die away instead of buzzing; the first real packet after
// a gap is ramped from the gain the concealment ended at back up to unity over
// fade_in, so neither edge is a step. Past conceal_fade the output is silence, and
// past conceal_max nothing is synthesized at all: an outage that long is not a
// glitch to paper over, and the consumer's own buffer (the headset's ring underrun,
// a short pipewire buffer on the server) produces silence anyway.
//
// v2, if the artefacts of the repetition ever prove audible enough to be worth it:
// pitch-synchronous repetition (find the period in the last packet and repeat that
// rather than the whole packet, G.711 Appendix I style), or a codec with its own
// concealment (Opus) instead of raw PCM.
class audio_plc
{
public:
	// Waveform repetition is faded to silence over this span
	static constexpr std::chrono::milliseconds conceal_fade{60};
	// Nothing is synthesized for a gap longer than this
	static constexpr std::chrono::milliseconds conceal_max{200};
	// Ramp applied to the first real packet after a concealed gap
	static constexpr std::chrono::milliseconds fade_in{5};
	// A sequence number up to this far behind the expected one is a late arrival
	// or a duplicate
	static constexpr int reorder_window = 512;
	// A gap wider than this is not a gap, it is a sender that restarted
	static constexpr int resync_ahead = 2048;

	struct result
	{
		// The packet is a duplicate or a straggler: discard it whole
		bool drop = false;
		// PCM to play immediately before the packet, empty when nothing was lost
		std::vector<uint8_t> concealment;
	};

	audio_plc() = default;

	audio_plc(uint32_t sample_rate, uint8_t num_channels) :
	        sample_rate(sample_rate ? sample_rate : 48000),
	        frame_bytes(std::max<size_t>(sizeof(int16_t), size_t(num_channels) * sizeof(int16_t)))
	{}

	// Forget everything, so that the next packet starts a fresh stream. Called when
	// a packet arrives without a sequence number (the peer put audio back on the
	// control path) so that flipping the toggle back does not read as a huge gap.
	void reset()
	{
		synced = false;
		last.clear();
		gain = 1.f;
	}

	// Feed one received packet, in arrival order. payload is modified in place when
	// it has to be ramped back up after concealment; it is untouched otherwise, so
	// an unbroken stream is delivered byte for byte.
	result receive(uint16_t seq, std::span<uint8_t> payload)
	{
		result r;

		if (not synced)
		{
			synced = true;
			expected = uint16_t(seq + 1);
			remember(payload);
			return r;
		}

		const int delta = int16_t(seq - expected);

		if (delta < 0)
		{
			if (delta > -reorder_window)
			{
				r.drop = true;
				return r;
			}
			// Far enough behind to be a restart rather than a straggler
		}
		else if (delta > 0 and delta <= resync_ahead)
		{
			r.concealment = conceal(size_t(delta));
			ramp_in(payload);
		}

		expected = uint16_t(seq + 1);
		remember(payload);
		return r;
	}

	// How long a span of PCM plays for, so that a caller can date a concealment
	// buffer relative to the packet it precedes
	int64_t ns_for(size_t bytes) const
	{
		return int64_t(bytes / frame_bytes) * 1'000'000'000 / sample_rate;
	}

private:
	uint32_t sample_rate = 48000;
	size_t frame_bytes = 4;

	bool synced = false;
	uint16_t expected = 0;
	// Copy of the last packet played, the material the concealment repeats
	std::vector<uint8_t> last;
	// Gain the last concealment ended at, 1 when the stream is unbroken
	float gain = 1.f;

	size_t frames_in(std::chrono::milliseconds d) const
	{
		return size_t(sample_rate) * size_t(d.count()) / 1000;
	}

	void remember(std::span<const uint8_t> payload)
	{
		last.assign(payload.begin(), payload.end());
	}

	// Synthesize the span of `missing` lost packets, each assumed to be the size of
	// the last one received — the senders emit a steady cadence, so that is the only
	// estimate available and the right one.
	std::vector<uint8_t> conceal(size_t missing)
	{
		const size_t src_bytes = last.size() - last.size() % frame_bytes;
		if (src_bytes == 0)
		{
			// Nothing to repeat; the caller still gets the packet, un-ramped
			gain = 1.f;
			return {};
		}

		const size_t max_bytes = frames_in(conceal_max) * frame_bytes;
		size_t bytes = std::min(missing * src_bytes, max_bytes);
		bytes -= bytes % frame_bytes;

		std::vector<uint8_t> out(bytes);
		const size_t src_samples = src_bytes / sizeof(int16_t);
		const size_t out_samples = bytes / sizeof(int16_t);
		const size_t channels = frame_bytes / sizeof(int16_t);
		const size_t fade_frames = std::max<size_t>(1, frames_in(conceal_fade));

		for (size_t i = 0; i < out_samples; ++i)
		{
			const size_t frame = i / channels;
			const float g = frame >= fade_frames
			                        ? 0.f
			                        : 1.f - float(frame) / float(fade_frames);

			int16_t sample;
			memcpy(&sample, last.data() + (i % src_samples) * sizeof(int16_t), sizeof(int16_t));
			sample = int16_t(std::lround(sample * g));
			memcpy(out.data() + i * sizeof(int16_t), &sample, sizeof(int16_t));
		}

		const size_t out_frames = out_samples / channels;
		gain = out_frames >= fade_frames ? 0.f : 1.f - float(out_frames) / float(fade_frames);
		return out;
	}

	// Bring the first real packet after a gap from the concealment's final gain back
	// to unity, so that the seam is a ramp and not a step
	void ramp_in(std::span<uint8_t> payload)
	{
		const float g0 = gain;
		gain = 1.f;
		if (g0 >= 1.f)
			return;

		const size_t channels = std::max<size_t>(1, frame_bytes / sizeof(int16_t));
		const size_t frames = payload.size_bytes() / frame_bytes;
		const size_t ramp = std::min(frames, std::max<size_t>(1, frames_in(fade_in)));

		for (size_t frame = 0; frame < ramp; ++frame)
		{
			const float g = g0 + (1.f - g0) * float(frame) / float(ramp);
			for (size_t c = 0; c < channels; ++c)
			{
				uint8_t * p = payload.data() + (frame * channels + c) * sizeof(int16_t);
				int16_t sample;
				memcpy(&sample, p, sizeof(int16_t));
				sample = int16_t(std::lround(sample * g));
				memcpy(p, &sample, sizeof(int16_t));
			}
		}
	}
};

} // namespace wivrn
