/*
 * WiVRn VR streaming
 * Copyright (C) 2025  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "wivrn_packets.h"

#include <cstdint>
#include <mutex>
#include <variant>

namespace wivrn
{

// Length of one full intra refresh sweep, in frames: how long the rolling column of
// intra coded blocks takes to cross the picture, and therefore how long recovery from
// a loss takes when it is repaired that way rather than with a keyframe.
//
// The trade is quality against recovery time. Every frame of a sweep carries a slice
// of intra coded blocks, so a short sweep spends more of the bit budget on them and
// leaves less for the picture; a long one recovers slowly and, worse, leaves a wider
// window in which a second loss lands inside the sweep and spoils it. 48 frames is
// half a second at 90 Hz and 0.4 s at 120 Hz — well inside the one second of no
// decoded output after which the headset gives up on the stream and drops back to the
// lobby (see the stall watchdog in client/scenes/stream.cpp), and short enough that
// the refresh is over long before a user notices the smeared region.
inline constexpr uint32_t intra_refresh_sweep_frames = 48;

class idr_handler
{
public:
	virtual ~idr_handler();
	virtual void on_feedback(const from_headset::feedback &) = 0;
	virtual void reset() = 0;
	virtual bool should_skip(uint64_t frame_id) = 0;

	// Repair loss with a rolling intra refresh instead of a full keyframe, over a sweep
	// of `sweep_frames` frames. Only meaningful for handlers that ask for keyframes at
	// all: the Vulkan encoders already recover gently, by encoding against the newest
	// frame the headset acknowledged rather than by sending a keyframe, and the raw
	// "encoder" has no inter frame prediction to repair. Both ignore this.
	virtual void set_intra_refresh(bool enabled, uint32_t sweep_frames) {}
};

// handler for unknown P-frames
// any lost frame triggers an I-frame
// skip frames until th I frame is received
//
// With intra refresh enabled (set_intra_refresh) the recovery half of that changes: a
// lost frame starts a rolling intra refresh instead, no frame is skipped while it runs,
// and the stream stays at a near constant bitrate through the repair. The keyframes that
// are not recovery — the first frame of a session, and the one after every reset(), which
// is what a reconnect, an encoder reconfiguration and a failover swap all go through —
// stay real IDRs: they are the points at which the headset's decoder has nothing valid to
// predict from, and only an IDR gives it a clean start.
class default_idr_handler : public idr_handler
{
	std::mutex mutex;
	struct need_idr
	{};
	struct wait_idr_feedback
	{
		uint64_t idr_id;
	};
	struct idr_received
	{};
	struct running
	{
		uint64_t first_p;
	};
	// A sweep has been asked for and starts on the next frame encoded
	struct need_refresh
	{};
	// A sweep is running: it started on `first` and the picture is whole again once
	// `until` has been encoded. `lost` records whether any frame of the sweep failed to
	// reach the headset — one that did takes its slice of intra blocks with it, so the
	// sweep has left a hole and has to be judged a failure.
	struct refreshing
	{
		uint64_t first;
		uint64_t until;
		bool lost;
	};
	std::variant<need_idr, wait_idr_feedback, idr_received, running, need_refresh, refreshing> state;
	std::vector<uint64_t> non_ref_frames{512, uint64_t(-1)};

	// Effective intra refresh state: the two switches ANDed, and whether the encoder
	// behind this handler was actually built with a refresh mechanism.
	bool refresh_enabled = false;
	uint32_t refresh_frames = intra_refresh_sweep_frames;
	// Sweeps in a row that ran with loss inside them. A link losing frames right through
	// a sweep would otherwise restart it forever and never show a whole picture again, so
	// after a few tries the handler gives up on being gentle and sends a real IDR.
	unsigned failed_refreshes = 0;
	static constexpr unsigned max_failed_refreshes = 3;

	// Slack on top of the nominal sweep length before it is judged finished. x264 advances
	// the refresh column by a whole number of macroblock columns per frame, so a sweep can
	// run a frame or two past the length that was asked for.
	static constexpr uint32_t refresh_slack_frames = 4;

public:
	enum class frame_type
	{
		i,
		p,
		// A P frame that also starts an intra refresh sweep. Backends that reach this
		// have a refresh mechanism, because nothing else ever turns one on.
		refresh,
	};

	void on_feedback(const from_headset::feedback &) override;
	void reset() override;
	bool should_skip(uint64_t frame_id) override;
	void set_intra_refresh(bool enabled, uint32_t sweep_frames) override;
	void set_non_ref(uint64_t frame_index);
	bool is_non_ref_frame(uint64_t frame_index);
	frame_type get_type(uint64_t frame_index);
};
} // namespace wivrn
