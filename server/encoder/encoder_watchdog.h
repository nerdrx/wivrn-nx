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

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace wivrn
{

// Health of one video stream's hardware encoder, and the decision to give up on it.
//
// Two failure modes, because those are the two the drivers actually produce:
//
//  * a hard error — the encode call returns a failure status or throws. Every
//    backend ends up here: NVENC's NvEncEncodePicture, VAAPI's avcodec_send_frame /
//    avcodec_receive_packet, and any Vulkan call the video encode queue rejects all
//    raise an exception that the compositor catches per frame. One is enough: a
//    session whose encode call failed once fails on every following frame too.
//
//  * silence — the frame went in and nothing came out. The backends all have a path
//    that logs a timeout and returns no picture, and a driver that wedges blocks
//    inside the encode call instead of returning at all. Both are measured the same
//    way: how long it has been since a picture last came out, counted only over
//    frames that were really submitted. Frames the IDR handler skips, and the
//    streams that are silent by design (the alpha plane on an opaque frame, the quad
//    stream with nothing promoted) never reach the encoder and never count.
//
// Deliberately not counted: a frame the sender dropped, or a frame the headset never
// acknowledged. Those are network problems and swapping the encoder would not fix
// one of them.
//
// Events come from the thread that runs the encoder. Decisions are taken by poll(),
// which must be called from one thread only — the one that is able to act on them,
// which is the thread that presents images, since the encoder thread may be the one
// wedged inside the driver. A decision is reported exactly once.
//
// The whole thing is a pure function of its events and a clock, so it is unit tested
// on a virtual clock in tests/encoder_failover_test.cpp.
class encoder_watchdog
{
public:
	struct config
	{
		// A stream that has gone this long without a picture, having been asked
		// for one, earns a strike. Roughly 45 frames at 90 Hz: far longer than
		// any legitimate hiccup, short enough that three of them still land
		// inside the couple of seconds a user would call "it froze".
		int64_t stall_ns = 500'000'000;
		// Strikes before the encoder is written off. A hard error skips them.
		int strikes = 3;
	};

	struct decision
	{
		// Why the encoder was written off, ready to be logged
		std::string reason;
	};

	encoder_watchdog() = default;

	explicit encoder_watchdog(config c) :
	        conf(c) {}

	// --- Setup --------------------------------------------------------------

	// Whether there is anything to fall back to. False for the software encoder
	// itself (nothing below it), for a stream whose bitstream a software encoder
	// could not stand in for, and for a stream that has already been swapped.
	void set_eligible(bool eligible)
	{
		std::lock_guard lock(mutex);
		eligible_ = eligible;
	}

	// The feature switch: the server configuration and the headset toggle, ANDed
	// by the caller. Live — turning it off mid-session stops the watchdog acting
	// but does not undo a swap that already happened.
	void set_enabled(bool enabled)
	{
		std::lock_guard lock(mutex);
		if (enabled and not enabled_)
		{
			// Start from a clean slate: what the stream did while nobody was
			// allowed to act on it is not evidence to act on now.
			window_since = 0;
			silent_frames = 0;
			strikes_ = 0;
		}
		enabled_ = enabled;
	}

	bool eligible() const
	{
		std::lock_guard lock(mutex);
		return eligible_;
	}

	// --- Events, encoder thread ---------------------------------------------

	// A frame was handed to the encoder. Only called for frames that really reach
	// it: skipped frames are not the encoder's fault.
	void encode_begin(int64_t now_ns)
	{
		std::lock_guard lock(mutex);
		in_call_since = now_ns;
		if (window_since == 0)
			window_since = now_ns;
	}

	// The encode call returned. `produced` is whether it gave back a picture.
	void encode_end(int64_t now_ns, bool produced)
	{
		std::lock_guard lock(mutex);
		in_call_since = 0;
		if (produced)
		{
			// Healthy traffic wipes the slate: strikes are meant to catch a
			// stream that stopped, not one that hiccupped an hour ago.
			strikes_ = 0;
			silent_frames = 0;
			window_since = now_ns;
		}
		else
			++silent_frames;
	}

	// The encode call failed. Written off at once, whatever the strike count.
	void encode_error(int64_t now_ns, std::string_view what)
	{
		std::lock_guard lock(mutex);
		in_call_since = 0;
		if (not enabled_ or not eligible_ or pending or done)
			return;
		pending = decision{std::string("the encoder failed: ") + std::string(what)};
		(void)now_ns;
	}

	// --- Decisions, one thread ----------------------------------------------

	// Look at the clock, count a strike if the stream has gone quiet, and report a
	// write-off exactly once.
	std::optional<decision> poll(int64_t now_ns)
	{
		std::lock_guard lock(mutex);

		if (not pending and enabled_ and eligible_ and not done)
		{
			// A call in progress when the window opened, because the watchdog
			// was switched on with the encoder already wedged inside the driver
			if (window_since == 0 and in_call_since != 0)
				window_since = in_call_since;

			// A call that never returned is as silent as one that returned
			// nothing, and it is the mode that hurts most: the encoder thread
			// is stuck in the driver and cannot notice anything itself. A call
			// that merely happens to be running right now is not wedged — the
			// present thread polls while the encoder thread works.
			const bool wedged = in_call_since != 0 and now_ns - in_call_since >= conf.stall_ns;
			const bool asked = wedged or silent_frames > 0;

			if (asked and window_since != 0 and now_ns - window_since >= conf.stall_ns)
			{
				++strikes_;
				silent_frames = 0;
				// The next strike is a whole window from here, not from the
				// last picture: without this a wedged call would strike out
				// on three consecutive polls.
				window_since = now_ns;

				if (strikes_ >= conf.strikes)
					pending = decision{
					        (wedged ? "the encode call has not returned for " : "no picture out of the encoder for ") +
					        std::to_string((conf.strikes * conf.stall_ns) / 1'000'000) + " ms"};
			}
		}

		if (not pending)
			return std::nullopt;

		// Once written off, stay written off: the replacement encoder gets a
		// watchdog of its own.
		done = true;
		eligible_ = false;
		std::optional<decision> out;
		out.swap(pending);
		return out;
	}

	// Number of strikes standing, for logs and tests
	int strikes() const
	{
		std::lock_guard lock(mutex);
		return strikes_;
	}

private:
	config conf;

	mutable std::mutex mutex;

	bool eligible_ = false;
	bool enabled_ = true;
	bool done = false;
	std::optional<decision> pending;

	// Start of the window the current strike is measured over: the last picture,
	// the last strike, or the first frame ever submitted.
	int64_t window_since = 0;
	// Submitted frames that produced nothing since then
	int silent_frames = 0;
	// When the encode call in progress started, 0 when none is
	int64_t in_call_since = 0;
	int strikes_ = 0;
};

} // namespace wivrn
