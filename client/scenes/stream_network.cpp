/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "stream.h"

#include "application.h"
#include "crypto.h"
#include "utils/i18n.h"
#include "utils/named_thread.h"

#include <chrono>
#include <fstream>
#include <spdlog/spdlog.h>
#include <thread>
#include <uni_algo/case.h>

void scenes::stream::process_packets()
{
#ifdef __ANDROID__
	application::instance().setup_jni();
#endif
	while (state_ != state::shutdown)
	{
		try
		{
			// Short enough that the path selector, evaluated at the end of every
			// poll, still reacts within a few hundred ms once the primary path
			// has gone completely silent
			network_session->poll(*this, std::chrono::milliseconds(100));
		}
		catch (std::exception & e)
		{
			spdlog::info("Exception in network thread: {}", e.what());

			// Seamless reconnect: hold the stream scene alive and re-handshake in the
			// background rather than dropping straight to the lobby. Returns false (and
			// falls through to exit()) when the feature is off, the window is exhausted,
			// or the user cancels, i.e. exactly the old behaviour.
			if (not try_seamless_reconnect())
			{
				spdlog::info("Network thread exiting");
				exit();
			}
		}
	}
}

namespace
{
// How long, in total, a seamless reconnect keeps trying before giving up and falling
// back to the lobby.
constexpr std::chrono::seconds reconnect_window{30};
// Backoff between attempts, doubling from the first to the second bound.
constexpr std::chrono::milliseconds reconnect_backoff_min{500};
constexpr std::chrono::milliseconds reconnect_backoff_max{2000};
} // namespace

std::unique_ptr<wivrn_session> scenes::stream::build_reconnect_session()
{
	if (not reconnect_target)
		return nullptr;

	// Reload the headset keypair from disk. crypto::key is move-only, so it is never
	// copied into the scene; it is read back here exactly as the lobby first loaded it.
	crypto::key keypair;
	try
	{
		std::ifstream f{application::get_config_path() / "private_key.pem"};
		std::string pem{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
		if (pem.empty())
			throw std::runtime_error("private key is empty or missing");
		keypair = crypto::key::from_private_key(pem);
	}
	catch (std::exception & e)
	{
		spdlog::warn("Seamless reconnect: cannot load the headset keypair: {}", e.what());
		return nullptr;
	}

	const auto & target = *reconnect_target;
	// A paired reconnect never triggers a PIN prompt (the server answers with
	// client_already_paired or encryption_disabled), so the callback just hands back the
	// stored PIN with no headset UI. If the server has genuinely forgotten the pairing it
	// is no longer a short outage, and the handshake failing here drops us to the lobby.
	auto pin_cb = [pin = target.pin](int) { return pin.empty() ? std::string("000000") : pin; };

	return std::visit([&](auto address) -> std::unique_ptr<wivrn_session> {
		return std::make_unique<wivrn_session>(address, target.port, target.tcp_only, keypair, pin_cb);
	},
	                  target.address);
}

void scenes::stream::refresh_reconnect_watchdog()
{
	// The held frames still carry the decoder-receipt time from before the outage. Once
	// the state returns to streaming the 1 s output watchdog would compare that stale time
	// against now and fire immediately, dumping to the lobby before the first fresh frame
	// of the resumed stream can arrive. Bumping the timestamps forward gives the resumed
	// stream a full second to deliver its IDR, which on a LAN it does in a few ms.
	std::unique_lock lock(frames_mutex);
	const XrTime now = instance.now();
	for (auto & decoder: decoders)
		for (auto & frame: decoder.latest_frames)
			if (frame)
				frame->feedback.received_from_decoder = now;

	// The outage is not evidence about the link on the far side of it — the last samples in
	// the de-jitter window are frames that were arriving as the connection died, all of them
	// wildly late. Keeping them would peg the playout delay at its ceiling for the first
	// seconds of the resumed stream. Start the measurement again instead.
	dejitter.reset();
}

bool scenes::stream::try_seamless_reconnect()
{
	if (not application::get_config().seamless_reconnect or not reconnect_target or state_ == state::shutdown)
		return false;

	spdlog::info("Seamless reconnect: primary path lost, holding the stream and re-handshaking");
	set_state(state::reconnecting);
	reconnect_cancelled = false;
	// The render and tracking threads keep sending on the dead primary until it is adopted;
	// swallow those failures so they do not tear the session down mid-reconnect.
	network_session->set_suppress_send_errors(true);

	// Subtle overlay over the frozen frame. Refreshed each frame by render() so it does
	// not fade while the reconnect is in progress.
	{
		auto toast = gui_toast.lock();
		toast->emplace(_("Reconnecting…"), false);
	}
	gui_status_last_change = instance.now();

	const auto deadline = std::chrono::steady_clock::now() + reconnect_window;
	auto backoff = reconnect_backoff_min;

	const auto interruptible_sleep = [this](std::chrono::milliseconds d) {
		const auto wake = std::chrono::steady_clock::now() + d;
		while (std::chrono::steady_clock::now() < wake and state_ != state::shutdown and not reconnect_cancelled)
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
	};

	while (std::chrono::steady_clock::now() < deadline and state_ != state::shutdown and not reconnect_cancelled)
	{
		std::unique_ptr<wivrn_session> fresh;
		try
		{
			// Blocks for the handshake (fast when the server is already re-accepting,
			// fails fast with ECONNREFUSED while it is still tearing the old one down).
			fresh = build_reconnect_session();
		}
		catch (std::exception & e)
		{
			spdlog::warn("Seamless reconnect attempt failed: {}", e.what());
		}

		if (fresh)
		{
			try
			{
				// The headset info must be the first control packet the server reads on
				// the new socket, so it goes out on the fresh session while no other
				// thread can touch it, before the swap.
				send_initial_control_packets(*fresh, guessed_fps);
				network_session->adopt_primary(std::move(*fresh));
				refresh_reconnect_watchdog();
				// New sockets are live again: let send failures be fatal once more.
				network_session->set_suppress_send_errors(false);
				set_state(state::streaming);
				gui_toast.lock()->reset();
				gui_status_last_change = instance.now();
				spdlog::info("Seamless reconnect: stream resumed");
				return true;
			}
			catch (std::exception & e)
			{
				// The new session handshook but resending the headset info failed; treat
				// it as a failed attempt and keep going.
				spdlog::warn("Seamless reconnect: could not resume the stream: {}", e.what());
			}
		}

		interruptible_sleep(backoff);
		backoff = std::min(backoff * 2, reconnect_backoff_max);
	}

	spdlog::warn("Seamless reconnect: giving up, falling back to the lobby");
	gui_toast.lock()->reset();
	return false;
}

void scenes::stream::operator()(to_headset::server_message && message)
{
	switch (message.kind)
	{
		case to_headset::server_message::kind::toast:
		case to_headset::server_message::kind::toast_urgent: {
			auto toast = gui_toast.lock();
			toast->emplace(message.msg, message.kind == to_headset::server_message::kind::toast_urgent);

			gui_status_last_change = instance.now();
			break;
		}

		case to_headset::server_message::kind::error: {
			auto queue = stream_error_queue.lock();
			queue->emplace(std::move(message.msg));

			break;
		}
	}
}

void scenes::stream::operator()(to_headset::video_stream_data_shard && shard)
{
	std::shared_lock lock(decoder_mutex);
	uint8_t idx = shard.stream_item_idx;
	if (idx >= decoders.size() or not decoders[idx].decoder)
	{
		// We don't know (yet?) about this stream, ignore packet
		return;
	}
	decoders[idx].decoder->push_shard(std::move(shard));
}

void scenes::stream::operator()(to_headset::video_stream_parity_shard && parity)
{
	std::shared_lock lock(decoder_mutex);
	uint8_t idx = parity.stream_item_idx;
	if (idx >= decoders.size() or not decoders[idx].decoder)
	{
		// We don't know (yet?) about this stream, ignore packet
		return;
	}
	decoders[idx].decoder->push_parity(std::move(parity));
}

void scenes::stream::operator()(to_headset::motion_field && chunk)
{
	// A field arrives as several chunks; only a complete one is ever warped along.
	auto field = motion_field.lock();
	const bool was_complete = field->complete();
	field->add(chunk);

	// Count the chunk that completed a field, not every chunk: a field the link tore in
	// half is not one the warp can use, and the Transport page is there to show that.
	if (field->complete() and not was_complete)
	{
		motion_field_last = instance.now();
		++motion_field_count;
	}
}

void scenes::stream::operator()(to_headset::feature_control && control)
{
	switch (control.f)
	{
		case wivrn::to_headset::feature_control::hid_input:
			hid_forwarding = control.state;
			if (not control.state and (application::get_config().forward_keyboard or application::get_config().forward_mouse or application::get_config().forward_gamepad))
			{
				auto toast = gui_toast.lock();
				toast->emplace(_("The server does not allow forwarded input devices"), true);
				gui_status_last_change = instance.now();
			}
			return;
		case wivrn::to_headset::feature_control::microphone:
			if (audio_handle)
				audio_handle->set_mic_state(control.state);
			return;
	}
}

void scenes::stream::operator()(to_headset::audio_stream_description && desc)
{
	audio_handle.emplace(desc, *network_session, instance);
}

void scenes::stream::operator()(to_headset::video_stream_description && desc)
{
	setup(desc);

	if (not tracking_thread)
	{
		tracking_thread = utils::named_thread("tracking_thread", &stream::tracking, this);
	}
}

void scenes::stream::operator()(to_headset::refresh_rate_change && rate)
{
	spdlog::info("refresh rate change request: {}", rate.hz);
	session.set_refresh_rate(rate.hz);
	std::shared_lock lock(decoder_mutex);
	if (video_stream_description)
		video_stream_description->refresh_rate = rate.hz;
}

void scenes::stream::operator()(to_headset::stream_tab_change && tab)
{
	next_gui_status = tab.tab;
}

void scenes::stream::operator()(to_headset::transport_status && status)
{
	// Arrives only while the Transport page holds a subscription. Dated on arrival: a feed
	// that stops has to look different from one reporting unchanging numbers.
	*transport_status.lock() = std::move(status);
	transport_status_received = instance.now();
}

void scenes::stream::operator()(to_headset::timesync_query && query)
{
	network_session->send_stream(from_headset::timesync_response{
	        .query = query.query,
	        .response = instance.now(),
	});
}

void scenes::stream::operator()(to_headset::path_pong && pong)
{
	auto now = std::chrono::steady_clock::now();
	int64_t rtt = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count() - pong.timestamp;

	// The echo comes back on the path the ping went out on; the network thread
	// has already counted it as liveness for that path
	if (pong.path_id == 0)
	{
		primary_rtt_ns = rtt;

		if (now >= primary_rtt_next_log)
		{
			primary_rtt_next_log = now + std::chrono::seconds(5);
			spdlog::debug("Primary path RTT {:.2f} ms", rtt / 1.e6);
		}
		return;
	}

	secondary_rtt_ns = rtt;

	if (now >= secondary_rtt_next_log)
	{
		secondary_rtt_next_log = now + std::chrono::seconds(5);
		spdlog::info("Secondary path {} RTT {:.2f} ms", int(pong.path_id), rtt / 1.e6);
	}
}

void scenes::stream::operator()(audio_data && data)
{
	if (audio_handle)
		(*audio_handle)(std::move(data));
}

void scenes::stream::send_feedback(const wivrn::from_headset::feedback & feedback)
{
	// A frame reported without a sent_to_decoder time never made it out of the accumulator,
	// and that report is exactly what makes the server force an IDR on this stream. The
	// headset never asks for one, so this is the only count of them it can keep.
	if (feedback.sent_to_decoder == 0)
		++incomplete_frames;

	try
	{
		network_session->send_control(wivrn::from_headset::feedback{feedback});
	}
	catch (std::exception & e)
	{
		spdlog::warn("Exception while sending feedback packet: {}", e.what());
	}
}

void scenes::stream::send_nack(const wivrn::from_headset::nack & nack)
{
	try
	{
		network_session->send_stream(wivrn::from_headset::nack{nack});
	}
	catch (std::exception & e)
	{
		// A request that could not be sent costs one round and nothing else: the
		// frame falls back on the incomplete-frame path exactly as it always did.
		spdlog::warn("Exception while sending shard retransmission request: {}", e.what());
	}
}

void scenes::stream::operator()(to_headset::application_list && l)
{
	apps(std::move(l));
}

void scenes::stream::operator()(to_headset::application_icon && icon)
{
	apps(std::move(icon));
}

void scenes::stream::operator()(to_headset::running_applications && apps)
{
	*running_applications.lock() = std::move(apps);
}

void scenes::stream::start_application(std::string appid)
{
	network_session->send_control(wivrn::from_headset::start_app{
	        .app_id = std::move(appid),
	});
}
