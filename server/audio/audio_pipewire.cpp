/*
 * WiVRn VR streaming
 * Copyright (C) 2024  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "audio_pipewire.h"

#include "audio_plc.h"
#include "driver/wivrn_session.h"
#include "os/os_time.h"
#include "util/u_logging.h"
#include "utils/ring_buffer.h"
#include <atomic>
#include <cstring>
#include <magic_enum.hpp>
#include <memory>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

namespace wivrn
{

namespace
{
struct deleter
{
	void operator()(pw_main_loop * loop)
	{
		pw_main_loop_destroy(loop);
	}
	void operator()(pw_stream * stream)
	{
		pw_stream_destroy(stream);
	}
};

struct pipewire_device : public audio_device
{
	to_headset::audio_stream_description desc;
	wivrn_session & session;

	std::unique_ptr<pw_main_loop, deleter> pw_loop;

	std::unique_ptr<pw_stream, deleter> speaker;
	pw_stream_events speaker_events{
	        .version = PW_VERSION_STREAM_EVENTS,
	        .process = &pipewire_device::speaker_process,
	};

	// Whether the speaker stream goes out on the loss-tolerant path. Written by the
	// network thread when the headset's settings change, read by the pipewire
	// speaker thread on every buffer.
	std::atomic<bool> low_latency{true};
	// Position of the next speaker packet in the stream, pipewire thread only. It
	// keeps counting while audio is on the control path, so that flipping the
	// toggle back on never looks like a gap.
	uint16_t speaker_seq = 0;

	utils::ring_buffer<audio_data, 100> mic_samples;
	std::atomic<size_t> mic_buffer_size_bytes;
	audio_data mic_current;
	// Sequence tracking and concealment for the microphone, network thread only
	audio_plc mic_plc;
	std::unique_ptr<pw_stream, deleter> microphone;
	std::atomic<std::underlying_type_t<pw_stream_state>> mic_state{PW_STREAM_STATE_UNCONNECTED};
	pw_stream_events mic_events{
	        .version = PW_VERSION_STREAM_EVENTS,
	        .state_changed = &pipewire_device::mic_state_changed,
	        .process = &pipewire_device::mic_process,
	};
	std::jthread thread;

	static void speaker_process(void * self_v);
	static void mic_process(void * self_v);
	static void mic_state_changed(void * self_v, pw_stream_state old, pw_stream_state state, const char * error);

	void process_mic_data(wivrn::audio_data &&) override;
	void set_low_latency(bool) override;
	void pause() override;
	void resume() override;

	~pipewire_device()
	{
		pw_main_loop_quit(pw_loop.get());
	};

	pipewire_device(
	        const std::string & source_name,
	        const std::string & source_description,
	        const std::string & sink_name,
	        const std::string & sink_description,
	        const wivrn::from_headset::headset_info_packet & info,
	        wivrn::wivrn_session & session) :
	        session(session)
	{
		int argc = 0;
		pw_init(&argc, nullptr);

		low_latency = info.settings.low_latency_audio;

		pw_loop.reset(pw_main_loop_new(nullptr));
		if (info.speaker)
		{
			desc.speaker = {
			        .num_channels = info.speaker->num_channels,
			        .sample_rate = info.speaker->sample_rate,
			};

			// Calculate quantum size: 5ms buffer for low latency while maintaining stability
			// Smaller buffers (<5ms) risk underruns, larger ones (>10ms) add perceptible latency
			uint32_t quantum_size = (desc.speaker->sample_rate * 5) / 1000;
			uint32_t frame_size = desc.speaker->num_channels * sizeof(int16_t);

			std::string rate_str = std::format("1/{}", desc.speaker->sample_rate);
			std::string latency_str = std::format("{}/{}", quantum_size, desc.speaker->sample_rate);

			speaker.reset(pw_stream_new_simple(
			        pw_main_loop_get_loop(pw_loop.get()),
			        sink_name.c_str(),
			        pw_properties_new(
			                PW_KEY_NODE_NAME,
			                sink_name.c_str(),
			                PW_KEY_NODE_DESCRIPTION,
			                sink_description.c_str(),
			                PW_KEY_MEDIA_TYPE,
			                "Audio",
			                PW_KEY_MEDIA_CATEGORY,
			                "Capture",
			                PW_KEY_MEDIA_CLASS,
			                "Audio/Sink",
			                PW_KEY_MEDIA_ROLE,
			                "Game",
			                // Set stream rate to match client, preventing PipeWire from doing
			                // unnecessary resampling which degrades audio quality
			                PW_KEY_NODE_RATE,
			                rate_str.c_str(),
			                // Declare target latency to help PipeWire optimize buffering
			                PW_KEY_NODE_LATENCY,
			                latency_str.c_str(),
			                NULL),
			        &speaker_events,
			        this));

			std::vector<uint8_t> buffer(1024);
			spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer.data(), uint32_t(buffer.size()));

			spa_audio_info_raw audio_info{
			        .format = SPA_AUDIO_FORMAT_S16,
			        .rate = desc.speaker->sample_rate,
			        .channels = desc.speaker->num_channels,
			};

			switch (audio_info.channels)
			{
				case 1:
					audio_info.position[0] = SPA_AUDIO_CHANNEL_MONO;
					break;
				case 2:
					audio_info.position[0] = SPA_AUDIO_CHANNEL_FL;
					audio_info.position[1] = SPA_AUDIO_CHANNEL_FR;
					break;
				default:
					U_LOG_W("No known audio mapping for %d channels speaker", audio_info.channels);
			}

			const spa_pod * params[1];
			params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &audio_info);

			// Stream flags:
			// - DRIVER: makes this node the graph clock master, preventing sync drift in the audio chain
			if (pw_stream_connect(
			            speaker.get(),
			            PW_DIRECTION_INPUT,
			            PW_ID_ANY,
			            pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
			            params,
			            1) < 0)
				throw std::runtime_error("failed to connect speaker stream");
			U_LOG_I("pipewire speaker stream created (quantum: %u frames, %.2f ms)", quantum_size, (quantum_size * 1000.0) / desc.speaker->sample_rate);
		}

		if (info.microphone)
		{
			desc.microphone = {
			        .num_channels = info.microphone->num_channels,
			        .sample_rate = info.microphone->sample_rate,
			};
			mic_plc = audio_plc(desc.microphone->sample_rate, desc.microphone->num_channels);

			// Calculate quantum size: 5ms buffer for low latency while maintaining stability
			// Smaller buffers (<5ms) risk underruns, larger ones (>10ms) add perceptible latency
			uint32_t quantum_size = (desc.microphone->sample_rate * 5) / 1000;
			uint32_t frame_size = desc.microphone->num_channels * sizeof(int16_t);

			std::string rate_str = std::format("1/{}", desc.microphone->sample_rate);
			std::string latency_str = std::format("{}/{}", quantum_size, desc.microphone->sample_rate);

			microphone.reset(pw_stream_new_simple(
			        pw_main_loop_get_loop(pw_loop.get()),
			        source_name.c_str(),
			        pw_properties_new(
			                PW_KEY_NODE_NAME,
			                source_name.c_str(),
			                PW_KEY_NODE_DESCRIPTION,
			                source_description.c_str(),
			                PW_KEY_MEDIA_TYPE,
			                "Audio",
			                PW_KEY_MEDIA_CATEGORY,
			                "Playback",
			                PW_KEY_MEDIA_CLASS,
			                "Audio/Source",
			                PW_KEY_MEDIA_ROLE,
			                "Game",
			                // Set stream rate to match client, preventing PipeWire from doing
			                // unnecessary resampling which degrades audio quality
			                PW_KEY_NODE_RATE,
			                rate_str.c_str(),
			                // Declare target latency to help PipeWire optimize buffering
			                PW_KEY_NODE_LATENCY,
			                latency_str.c_str(),
			                NULL),
			        &mic_events,
			        this));
			std::vector<uint8_t> buffer(1024);
			spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer.data(), uint32_t(buffer.size()));

			spa_audio_info_raw audio_info{
			        .format = SPA_AUDIO_FORMAT_S16,
			        .rate = desc.microphone->sample_rate,
			        .channels = desc.microphone->num_channels,
			};

			switch (audio_info.channels)
			{
				case 1:
					audio_info.position[0] = SPA_AUDIO_CHANNEL_MONO;
					break;
				case 2:
					audio_info.position[0] = SPA_AUDIO_CHANNEL_FL;
					audio_info.position[1] = SPA_AUDIO_CHANNEL_FR;
					break;
				default:
					U_LOG_W("No known audio mapping for %d channels microphone", audio_info.channels);
			}

			const spa_pod * params[1];
			params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &audio_info);

			if (pw_stream_connect(
			            microphone.get(),
			            PW_DIRECTION_OUTPUT,
			            PW_ID_ANY,
			            pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
			            params,
			            1) < 0)
				throw std::runtime_error("failed to connect microphone stream");
			U_LOG_I("pipewire microphone stream created (quantum: %u frames, %.2f ms)", quantum_size, (quantum_size * 1000.0) / desc.microphone->sample_rate);
		}

		if (desc.speaker or desc.microphone)
			thread = std::jthread(
			        [this](std::stop_token) {
				pw_main_loop_run(pw_loop.get());
				speaker.reset();
				microphone.reset();
				; });
	}
};
} // namespace

void pipewire_device::mic_process(void * self_v)
{
	// std::cerr << "mic_process" << std::endl;
	auto self = (pipewire_device *)self_v;
	auto buffer = pw_stream_dequeue_buffer(self->microphone.get());
	if (not buffer)
	{
		U_LOG_W("Out of buffers: %s", strerror(errno));
		return;
	}

	const auto & data = buffer->buffer->datas[0];
	uint8_t * data_ptr = (uint8_t *)data.data;
	if (not data.data)
	{
		pw_stream_queue_buffer(self->microphone.get(), buffer);
		return;
	}

	const size_t frame_size = self->desc.microphone->num_channels * sizeof(int16_t);

	// Use consistent buffer size based on stream quantum
#if PW_CHECK_VERSION(0, 3, 49)
	size_t num_frames = buffer->requested;
#else
	size_t num_frames = 0;
#endif
	if (num_frames == 0)
	{
		uint32_t quantum_size = (self->desc.microphone->sample_rate * 5) / 1000;
		num_frames = std::min<size_t>(quantum_size, data.maxsize / frame_size);
	}
	data.chunk->offset = 0;
	data.chunk->size = 0;
	data.chunk->stride = frame_size;

	while (num_frames != 0)
	{
		// remaining bytes in existing buffer
		auto & current = self->mic_current;
		ptrdiff_t tmp_remain = current.payload.size_bytes();
		// limit to requested frames
		tmp_remain = std::min<ptrdiff_t>(tmp_remain, num_frames * frame_size);
		if (tmp_remain)
		{
			memcpy(data_ptr, current.payload.data(), tmp_remain);
			current.payload = current.payload.subspan(tmp_remain);
			data_ptr += tmp_remain;
			data.chunk->size += tmp_remain;
			num_frames -= tmp_remain / frame_size;
			self->mic_buffer_size_bytes -= tmp_remain;
		}
		else
		{
			auto tmp = self->mic_samples.read();
			if (not tmp)
				break;
			self->mic_current = std::move(*tmp);
		}
	}
	pw_stream_queue_buffer(self->microphone.get(), buffer);

	// discard excess data, so we don't accumulate latency
	size_t target_buffer_size = frame_size * self->desc.microphone->sample_rate * 0.08;
	while (self->mic_buffer_size_bytes > target_buffer_size and self->mic_samples.size() > 1)
	{
		auto tmp = self->mic_samples.read();
		if (not tmp)
			break;
		self->mic_buffer_size_bytes -= tmp->payload.size_bytes();
		U_LOG_D("Audio sync: discard %zd bytes", tmp->payload.size_bytes());
	}
}

void pipewire_device::mic_state_changed(void * self_v, pw_stream_state old, pw_stream_state state, const char * error)
{
	auto self = (pipewire_device *)self_v;
	self->mic_state = state;
	if (error)
		U_LOG_I("Microphone state changed from %s to %s (error: %s)", magic_enum::enum_name(old).data(), magic_enum::enum_name(state).data(), error);
	else
		U_LOG_I("Microphone state changed from %s to %s", magic_enum::enum_name(old).data(), magic_enum::enum_name(state).data());
	switch (state)
	{
		case PW_STREAM_STATE_ERROR:
			U_LOG_W("Error on microphone stream: %s", error);
			return;
		case PW_STREAM_STATE_UNCONNECTED:
		case PW_STREAM_STATE_CONNECTING:
		case PW_STREAM_STATE_PAUSED:
			try
			{
				self->session.send_control(to_headset::feature_control{to_headset::feature_control::microphone, false});
			}
			catch (std::exception & e)
			{
				U_LOG_W("failed to update microphone state: %s", e.what());
			}
			return;
		case PW_STREAM_STATE_STREAMING:
			try
			{
				self->session.send_control(to_headset::feature_control{to_headset::feature_control::microphone, true});
			}
			catch (std::exception & e)
			{
				U_LOG_W("failed to update microphone state: %s", e.what());
			}
			return;
	}
}

void pipewire_device::speaker_process(void * self_v)
{
	auto self = (pipewire_device *)self_v;
	auto buffer = pw_stream_dequeue_buffer(self->speaker.get());
	if (not buffer)
	{
		U_LOG_W("Out of buffers: %s", strerror(errno));
		return;
	}

	const auto & data = buffer->buffer->datas[0];
	if (not data.data)
		return;

	const bool lossy = self->low_latency.load(std::memory_order_relaxed);
	const size_t frame_size = std::max<size_t>(sizeof(int16_t), self->desc.speaker->num_channels * sizeof(int16_t));
	// The graph quantum is asked to be 5 ms (960 bytes at 48 kHz stereo), but
	// nothing guarantees it: a buffer longer than one datagram may carry is cut on
	// frame boundaries, each piece a packet of its own.
	const size_t chunk = audio_frames_per_packet(frame_size, audio_data::max_payload_size) * frame_size;

	uint8_t * const pcm = (uint8_t *)data.data + data.chunk->offset;
	const size_t total = data.chunk->size - data.chunk->size % frame_size;
	const XrTime now = self->session.get_offset().to_headset(os_monotonic_get_ns());

	try
	{
		for (size_t offset = 0; offset < total; offset += chunk)
		{
			const size_t size = std::min(chunk, total - offset);
			const uint16_t seq = self->speaker_seq++;

			audio_data packet{
			        // The buffer is one capture instant; the pieces of it play
			        // one after the other
			        .timestamp = now + XrTime(offset / frame_size) * 1'000'000'000 / self->desc.speaker->sample_rate,
			        .payload = std::span(pcm + offset, size),
			};

			if (lossy)
			{
				packet.seq = seq;
				self->session.send_stream(std::move(packet));
			}
			else
				self->session.send_control(std::move(packet));
		}
	}
	catch (std::exception & e)
	{
		U_LOG_D("Failed to send audio data: %s", e.what());
	}
	pw_stream_queue_buffer(self->speaker.get(), buffer);
}

void pipewire_device::process_mic_data(wivrn::audio_data && sample)
{
	if (sample.seq)
	{
		auto r = mic_plc.receive(*sample.seq, sample.payload);
		if (r.drop)
			return;

		if (not r.concealment.empty())
		{
			// Pushed as a packet of its own, ahead of the real one: the pipewire
			// side pulls from the same ring and cannot tell the difference. Not
			// filling the hole at all would only shorten a buffer, which is the
			// same silence with the whole stream advanced by the lost span.
			const size_t size = r.concealment.size();
			audio_data filler;
			// It plays out immediately before the packet that revealed the gap
			filler.timestamp = sample.timestamp - mic_plc.ns_for(size);
			filler.data.c = std::make_shared_for_overwrite<uint8_t[]>(size);
			memcpy(filler.data.c.get(), r.concealment.data(), size);
			filler.payload = std::span(filler.data.c.get(), size);

			if (mic_samples.write(std::move(filler)))
				mic_buffer_size_bytes += size;
		}
	}
	else
	{
		// The headset put the microphone back on the control path
		mic_plc.reset();
	}

	auto size = sample.payload.size_bytes();
	if (mic_samples.write(std::move(sample)))
		mic_buffer_size_bytes += size;
}

void pipewire_device::set_low_latency(bool enabled)
{
	low_latency.store(enabled, std::memory_order_relaxed);
}

void pipewire_device::pause()
{
}

void pipewire_device::resume()
{
	session.send_control(to_headset::audio_stream_description{desc});
	session.send_control(to_headset::feature_control{to_headset::feature_control::microphone, mic_state == PW_STREAM_STATE_STREAMING});
}

std::unique_ptr<audio_device> create_pipewire_handle(
        const std::string & source_name,
        const std::string & source_description,
        const std::string & sink_name,
        const std::string & sink_description,
        const wivrn::from_headset::headset_info_packet & info,
        wivrn_session & session)
{
	try
	{
		return std::make_unique<pipewire_device>(
		        source_name, source_description, sink_name, sink_description, info, session);
	}
	catch (std::exception & e)
	{
		U_LOG_I("Pipewire backend creation failed: %s", e.what());
		return nullptr;
	}
}
} // namespace wivrn
