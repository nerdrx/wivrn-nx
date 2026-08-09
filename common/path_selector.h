/*
 * WiVRn VR streaming
 * Copyright (C) 2026  WiVRn NX contributors
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

#include <atomic>
#include <chrono>
#include <format>
#include <mutex>
#include <optional>
#include <string>

namespace wivrn
{

// Which of the two paths of a multipath session carries the output, and when to
// change that. Both ends run one: the server's decides where video and control
// go, the headset's decides where tracking, input and feedback go. They are
// independent — an asymmetric state (server on the secondary, headset still on
// the primary) is valid and costs nothing.
//
// Events (packet received, send failed) come from whatever thread does the I/O
// and only set flags. Decisions are taken by update(), which must be called from
// one thread only, typically the one polling the sockets. A switch requested
// from an I/O thread is applied immediately — routing must not wait — but is
// reported by the next update(), so that the (expensive) reaction to a switch
// always runs on that one thread.
class path_selector
{
public:
	using clock = std::chrono::steady_clock;

	struct config
	{
		// A path is declared down after this long without a single packet on it.
		// The keepalive is 250 ms, so this tolerates one lost keepalive.
		std::chrono::milliseconds dead_after{400};
		// ... and healthy again only after this long of uninterrupted traffic
		std::chrono::milliseconds healthy_for{5000};
		// Send errors on the datagram stream socket are transient until they
		// have been going on for this long: a connected UDP socket reports a
		// latched error (an ICMP unreachable while the link is down) to one
		// operation and is usable again right after.
		std::chrono::milliseconds stream_error_grace{500};
	};

	struct switch_event
	{
		bool on_secondary;
		std::string reason;
		// Time since the previous switch, zero for the first one
		std::chrono::milliseconds since_previous;
	};

	path_selector() = default;

	explicit path_selector(config c) :
	        conf(c) {}

	// --- Events, any thread -------------------------------------------------

	// Any packet received on the primary path, whatever it is. from_control
	// tells which of the two primary sockets it came from.
	//
	// A receive says nothing about our ability to send, so it does not clear the
	// run of stream send errors: only a send that works does (on_stream_send_ok).
	void on_primary_received(bool from_control, clock::time_point now)
	{
		primary_last_receive = now;

		// A TCP socket that failed to send is broken for good, and a broken
		// socket never receives again, so this cannot flap
		if (from_control)
			control_up_ = true;
	}

	// A send on the primary control socket failed. Returns true the first time,
	// so that the caller logs it once.
	bool on_control_send_error()
	{
		return control_up_.exchange(false);
	}

	// A send on the primary stream (datagram) socket failed. Returns true if it
	// starts a new run of errors.
	bool on_stream_send_error(clock::time_point now)
	{
		++stream_errors;

		clock::time_point none{};
		return stream_error_since.compare_exchange_strong(none, now);
	}

	void on_stream_send_ok()
	{
		if (stream_error_since.load() != clock::time_point{}) [[unlikely]]
			stream_error_since = clock::time_point{};
	}

	// Whether a secondary path exists and could carry the session on its own
	void set_secondary_usable(bool usable)
	{
		secondary_usable = usable;
	}

	// Force a switch. Takes effect at once; reported by the next update().
	void request(bool secondary, std::string reason)
	{
		if (on_secondary_.exchange(secondary) == secondary)
			return;

		// The stream socket is idle while the secondary path carries the
		// session, so its history of send errors says nothing about it any more
		// — and would otherwise keep the primary marked down for good.
		stream_error_since = clock::time_point{};

		std::lock_guard lock(mutex);
		pending = switch_event{secondary, std::move(reason), std::chrono::milliseconds(0)};
	}

	// --- State, any thread --------------------------------------------------

	bool on_secondary() const
	{
		return on_secondary_;
	}

	bool control_up() const
	{
		return control_up_;
	}

	uint64_t stream_send_errors() const
	{
		return stream_errors;
	}

	// --- Decisions, one thread ----------------------------------------------

	// Back to a fresh primary path, e.g. after a reconnect
	void reset(clock::time_point now)
	{
		on_secondary_ = false;
		control_up_ = true;
		stream_error_since = clock::time_point{};
		primary_last_receive = now;
		primary_healthy_since = now;
		last_switch = clock::time_point{};

		std::lock_guard lock(mutex);
		pending.reset();
	}

	// Evaluate both paths, switch if needed, and report a switch (from here or
	// from an I/O thread) exactly once.
	std::optional<switch_event> update(clock::time_point now)
	{
		const bool stream_down = [&] {
			auto since = stream_error_since.load();
			return since != clock::time_point{} and now - since > conf.stream_error_grace;
		}();

		const auto silent_for = std::chrono::duration_cast<std::chrono::milliseconds>(now - primary_last_receive.load());
		const bool silent = silent_for > conf.dead_after;
		const bool primary_bad = silent or not control_up_ or stream_down;

		if (primary_bad)
			primary_healthy_since = clock::time_point{};
		else if (primary_healthy_since == clock::time_point{})
			primary_healthy_since = now;

		if (not on_secondary_)
		{
			if (primary_bad and secondary_usable)
				request(true,
				        silent        ? std::format("nothing received on the primary path for {} ms", silent_for.count())
				        : stream_down ? std::string("the primary stream socket keeps failing")
				                      : std::string("the primary control socket failed"));
		}
		else if (not secondary_usable)
		{
			request(false, "secondary path is gone");
		}
		else if (not primary_bad and now - primary_healthy_since >= conf.healthy_for)
		{
			request(false,
			        std::format("primary path healthy again for {} ms",
			                    std::chrono::duration_cast<std::chrono::milliseconds>(now - primary_healthy_since).count()));
		}

		std::optional<switch_event> event;
		{
			std::lock_guard lock(mutex);
			event.swap(pending);
		}

		if (event)
		{
			event->since_previous = last_switch == clock::time_point{}
			                                ? std::chrono::milliseconds(0)
			                                : std::chrono::duration_cast<std::chrono::milliseconds>(now - last_switch);
			last_switch = now;
		}

		return event;
	}

private:
	config conf;

	std::atomic<bool> on_secondary_ = false;
	std::atomic<bool> secondary_usable = false;
	std::atomic<bool> control_up_ = true;
	std::atomic<clock::time_point> primary_last_receive{};
	std::atomic<clock::time_point> stream_error_since{};
	std::atomic<uint64_t> stream_errors = 0;

	// update() only
	clock::time_point primary_healthy_since{};
	clock::time_point last_switch{};

	std::mutex mutex;
	std::optional<switch_event> pending;
};

} // namespace wivrn
