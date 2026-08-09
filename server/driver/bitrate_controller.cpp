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

#include "bitrate_controller.h"

#include "util/u_logging.h"

#include <algorithm>
#include <vector>

namespace wivrn
{

namespace
{
constexpr double to_mbits = 1e-6;
}

void bitrate_controller::configure(const config & c, uint32_t ceiling_bps, bool client_enabled_)
{
	std::lock_guard lock(mutex);

	conf = c;
	client_enabled = client_enabled_;
	ceiling = ceiling_bps;
	auto eff = effective_ceiling();
	// A client asking for less than the configured minimum wins: the ceiling is always honoured.
	min_bitrate = std::min(conf.min_bitrate_bps, eff ? eff : conf.min_bitrate_bps);
	bitrate = eff;
	recovery_target = eff;
	st = state::steady;
	first_recovery_step = true;
	frames = {};
	has_frames = false;
	flush();

	if (conf.enabled and client_enabled and eff)
		U_LOG_I("Automatic bitrate enabled, ceiling %.1f Mbit/s, floor %.1f Mbit/s",
		        eff * to_mbits,
		        min_bitrate * to_mbits);
	else if (conf.enabled and not client_enabled and eff)
		U_LOG_I("Automatic bitrate disabled on the headset, using %.1f Mbit/s", eff * to_mbits);
}

uint32_t bitrate_controller::effective_ceiling() const
{
	if (path_ceiling and ceiling)
		return std::min(ceiling, *path_ceiling);
	return ceiling;
}

bool bitrate_controller::enabled() const
{
	std::lock_guard lock(mutex);
	return conf.enabled and client_enabled;
}

std::optional<uint32_t> bitrate_controller::set_client_enabled(bool enabled_)
{
	std::lock_guard lock(mutex);

	if (enabled_ == client_enabled)
		return {};

	client_enabled = enabled_;

	U_LOG_I("Automatic bitrate %s on the headset", client_enabled ? "enabled" : "disabled");

	// Measurements taken under the other setting say nothing about it now. Starting over also
	// puts the bitrate back to the ceiling, which is what switching off must do.
	return reset_locked();
}

uint32_t bitrate_controller::current() const
{
	std::lock_guard lock(mutex);
	return bitrate;
}

uint32_t bitrate_controller::clamp(uint64_t value) const
{
	return uint32_t(std::clamp<uint64_t>(value, min_bitrate, effective_ceiling()));
}

void bitrate_controller::flush()
{
	window.clear();
	healthy_since.reset();
	last_evaluation = {};
}

std::optional<uint32_t> bitrate_controller::set_ceiling(uint32_t ceiling_bps)
{
	std::lock_guard lock(mutex);

	if (not ceiling_bps)
		return {};

	ceiling = ceiling_bps;
	min_bitrate = std::min(conf.min_bitrate_bps, effective_ceiling());
	return reset_locked();
}

std::optional<uint32_t> bitrate_controller::set_path_ceiling(std::optional<uint32_t> ceiling_bps)
{
	std::lock_guard lock(mutex);

	if (ceiling_bps == path_ceiling)
		return {};

	path_ceiling = ceiling_bps;

	if (not ceiling)
		return {};

	min_bitrate = std::min(conf.min_bitrate_bps, effective_ceiling());

	if (path_ceiling)
		U_LOG_I("Bitrate ceiling for the active path: %.1f Mbit/s (client asked for %.1f Mbit/s)",
		        effective_ceiling() * to_mbits,
		        ceiling * to_mbits);
	else
		U_LOG_I("Bitrate ceiling back to the client's %.1f Mbit/s", ceiling * to_mbits);

	return reset_locked();
}

std::optional<uint32_t> bitrate_controller::reset()
{
	std::lock_guard lock(mutex);
	return reset_locked();
}

std::optional<uint32_t> bitrate_controller::reset_locked()
{
	if (not ceiling)
		return {};

	bitrate = effective_ceiling();
	recovery_target = bitrate;
	st = state::steady;
	first_recovery_step = true;
	frames = {};
	has_frames = false;
	flush();

	return bitrate;
}

void bitrate_controller::close_frame(frame_state & frame, std::chrono::steady_clock::time_point now)
{
	if (frame.index == uint64_t(-1))
		return;

	if (frame.lost)
	{
		window.push_back({.when = now, .lost = true, .late = frame.late});
	}
	else if (frame.valid and frame.first and frame.last >= frame.first and frame_period > 0)
	{
		window.push_back({
		        .when = now,
		        .utilisation = float(double(frame.last - frame.first) / double(frame_period)),
		        .late = frame.late,
		});
	}

	frame = {};
}

bitrate_controller::stats bitrate_controller::analyse(std::chrono::steady_clock::time_point now)
{
	while (not window.empty() and now - window.front().when > window_duration)
		window.pop_front();

	stats res{.count = window.size()};

	std::vector<float> utilisations;
	utilisations.reserve(window.size());
	for (const auto & s: window)
	{
		if (s.lost)
			++res.lost;
		else
			utilisations.push_back(s.utilisation);
		if (s.late)
			++res.late;
	}

	if (not utilisations.empty())
	{
		size_t n = std::min(utilisations.size() - 1,
		                    size_t(utilisation_percentile * utilisations.size()));
		std::nth_element(utilisations.begin(), utilisations.begin() + n, utilisations.end());
		res.utilisation = utilisations[n];
	}

	// A frame that never arrived is at least as bad as a fully saturated one.
	if (res.lost)
		res.utilisation = std::max(res.utilisation, double(utilisation_severe));

	return res;
}

std::optional<uint32_t> bitrate_controller::on_feedback(const from_headset::feedback & feedback, int64_t period_ns, bool streaming)
{
	std::lock_guard lock(mutex);

	if (not conf.enabled or not client_enabled or not streaming or not ceiling or period_ns <= 0)
		return {};

	// Only the video streams (one per encoder) report frame delivery timings.
	if (feedback.stream_index >= video_stream_count)
		return {};

	frame_period = period_ns;

	auto now = std::chrono::steady_clock::now();

	if (not has_frames or feedback.frame_index > newest_frame)
	{
		newest_frame = feedback.frame_index;
		has_frames = true;
	}
	// Feedback for a frame that has already been turned into a sample (the client sends several
	// packets per frame, the last one only after it was displayed).
	else if (feedback.frame_index + frame_ring_size <= newest_frame)
		return {};

	auto & frame = frames[feedback.frame_index % frames.size()];
	if (frame.index != feedback.frame_index)
	{
		close_frame(frame, now);
		frame.index = feedback.frame_index;
	}

	if (not feedback.sent_to_decoder)
	{
		// The frame was given up on before it was complete: packets were lost or arrived far
		// too late. Its received_first_packet is the time it was abandoned, not a wire time.
		frame.lost = true;
	}
	else
	{
		if (feedback.received_first_packet)
			frame.first = frame.first ? std::min(frame.first, feedback.received_first_packet) : feedback.received_first_packet;
		if (feedback.received_last_packet)
			frame.last = std::max(frame.last, feedback.received_last_packet);
		frame.valid = true;
	}

	// Decoded, then evicted by a newer frame without ever being shown. Note that
	// times_displayed == 0 on its own means nothing: it is also the state of the feedback sent
	// as soon as a frame is handed to the decoder.
	if (feedback.received_from_decoder and not feedback.blitted and feedback.times_displayed == 0)
		frame.late = true;

	return evaluate(now);
}

std::optional<uint32_t> bitrate_controller::evaluate(std::chrono::steady_clock::time_point now)
{
	if (now - last_evaluation < evaluation_interval)
		return {};
	last_evaluation = now;

	auto s = analyse(now);
	if (s.count < min_samples)
		return {};

	const bool severe = s.utilisation > utilisation_severe or
	                    s.lost >= lost_frames_severe or
	                    s.late >= late_frames_severe;
	const bool degraded = severe or
	                      s.utilisation > utilisation_decrease or
	                      s.lost >= lost_frames_decrease or
	                      s.late >= late_frames_decrease;
	const bool healthy = s.utilisation < utilisation_increase and s.lost == 0 and s.late == 0;

	const uint32_t previous = bitrate;
	const char * reason = nullptr;

	if (degraded)
	{
		healthy_since.reset();

		if (now - last_decrease < decrease_cooldown)
			return {};
		last_decrease = now;

		if (st == state::recovering)
		{
			// Congestion returned while rebounding: the pre-drop bitrate was too
			// optimistic, aim lower next time.
			recovery_target = clamp(uint64_t(recovery_target * recovery_target_backoff));
		}

		if (severe)
		{
			if (st != state::recovering)
				recovery_target = bitrate;
			bitrate = clamp(uint64_t(bitrate * deep_decrease_factor));
			st = state::recovering;
			first_recovery_step = true;
			reason = "acute congestion";
		}
		else
		{
			bitrate = clamp(uint64_t(bitrate * decrease_factor));
			reason = "congestion";
		}

		if (st == state::recovering and recovery_target <= bitrate)
			st = state::steady;

		flush();
	}
	else if (healthy)
	{
		if (not healthy_since)
			healthy_since = now;

		auto held = now - *healthy_since;

		if (st == state::recovering)
		{
			if (held < (first_recovery_step ? recovery_confirm : recovery_step_interval))
				return {};

			bitrate = std::min(recovery_target, clamp(uint64_t(bitrate * recovery_factor)));
			first_recovery_step = false;
			reason = "link healthy again, rebounding";

			if (bitrate >= recovery_target)
				st = state::steady;
		}
		else
		{
			if (held < increase_hold or bitrate >= effective_ceiling())
				return {};

			uint32_t step = std::max<uint32_t>(increase_step_min, uint32_t(effective_ceiling() * increase_step_ratio));
			bitrate = clamp(uint64_t(bitrate) + step);
			recovery_target = bitrate;
			reason = "spare capacity";
		}

		flush();
	}
	else
	{
		// Between the two thresholds: hysteresis band, hold the current bitrate.
		healthy_since.reset();
		return {};
	}

	if (bitrate == previous)
		return {};

	U_LOG_I("Automatic bitrate: %s, %.1f -> %.1f Mbit/s (p%d utilisation %.2f, %zu lost, %zu late over %zu frames)",
	        reason,
	        previous * to_mbits,
	        bitrate * to_mbits,
	        int(utilisation_percentile * 100),
	        s.utilisation,
	        s.lost,
	        s.late,
	        s.count);

	return bitrate;
}

} // namespace wivrn
