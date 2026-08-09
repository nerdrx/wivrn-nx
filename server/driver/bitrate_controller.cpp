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

void bitrate_controller::configure(const config & c, uint32_t ceiling_bps, bool client_enabled_, bool radio_aware_)
{
	std::lock_guard lock(mutex);

	conf = c;
	client_enabled = client_enabled_;
	radio_aware = radio_aware_;
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
	flush_radio();

	if (conf.enabled and client_enabled and eff)
		U_LOG_I("Automatic bitrate enabled, ceiling %.1f Mbit/s, floor %.1f Mbit/s, radio trend %s",
		        eff * to_mbits,
		        min_bitrate * to_mbits,
		        radio_aware ? "on" : "off");
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

void bitrate_controller::set_radio_aware(bool enabled_)
{
	std::lock_guard lock(mutex);

	if (enabled_ == radio_aware)
		return;

	radio_aware = enabled_;

	U_LOG_I("Radio-aware bitrate %s on the headset", radio_aware ? "enabled" : "disabled");

	// The trend is only as good as the reports, which stop when the switch goes off. Never
	// touches the bitrate: the AIMD probes back up on its own if the step was unnecessary.
	flush_radio();
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

void bitrate_controller::flush_radio()
{
	radio_window.clear();
	radio_ema.reset();
	last_radio_sample = {};
	last_radio_step = {};
	radio_hold = false;
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

	// The radio samples are a property of the radio, not of the bitrate, and survive a reset;
	// the hold does not, the bitrate is back at the ceiling and probing is allowed again.
	radio_hold = false;
	last_radio_step = {};

	return bitrate;
}

void bitrate_controller::close_frame(frame_state & frame, clock::time_point now)
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

bitrate_controller::stats bitrate_controller::analyse(clock::time_point now)
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

bitrate_controller::radio_trend bitrate_controller::analyse_radio(clock::time_point now)
{
	while (not radio_window.empty() and now - radio_window.front().when > radio_trend_window)
		radio_window.pop_front();

	radio_trend res;
	if (radio_window.size() < radio_min_samples)
		return res;

	const auto & oldest = radio_window.front();
	const auto & newest = radio_window.back();

	res.span_s = std::chrono::duration<double>(newest.when - oldest.when).count();
	if (res.span_s <= 0)
		return res;

	// Least squares slope of the smoothed level against time, in dB/s. Ordinary means and
	// covariance, the window holds a handful of samples.
	double mean_t = 0;
	double mean_y = 0;
	for (const auto & s: radio_window)
	{
		mean_t += std::chrono::duration<double>(s.when - oldest.when).count();
		mean_y += s.rssi_dbm;
	}
	mean_t /= double(radio_window.size());
	mean_y /= double(radio_window.size());

	double cov = 0;
	double var = 0;
	for (const auto & s: radio_window)
	{
		double dt = std::chrono::duration<double>(s.when - oldest.when).count() - mean_t;
		cov += dt * (s.rssi_dbm - mean_y);
		var += dt * dt;
	}
	if (var <= 0)
		return res;

	res.usable = true;
	res.rssi_dbm = newest.rssi_dbm;
	res.slope_db_per_s = cov / var;
	res.delta_db = res.slope_db_per_s * res.span_s;
	res.link_speed_mbps = newest.link_speed_mbps;
	for (const auto & s: radio_window)
		res.link_speed_peak_mbps = std::max(res.link_speed_peak_mbps, s.link_speed_mbps);

	return res;
}

std::optional<uint32_t> bitrate_controller::on_wifi_state(int rssi_dbm, int link_speed_mbps, clock::time_point when)
{
	std::lock_guard lock(mutex);

	// Both switches, plus the radio one, plus a stream to control at all.
	if (not conf.enabled or not client_enabled or not radio_aware or not ceiling)
		return {};

	// Sentinels and nonsense. Android answers -127 when it will not say, and no real Wi-Fi
	// link sits at 0 dBm or below -110 dBm.
	if (rssi_dbm >= 0 or rssi_dbm <= -110)
		return {};

	// A gap in the reports means the trend across it is meaningless: start a new one.
	if (not radio_window.empty() and when - last_radio_sample > radio_max_age)
	{
		radio_window.clear();
		radio_ema.reset();
		radio_hold = false;
	}
	last_radio_sample = when;

	radio_ema = radio_ema
	                    ? radio_ema_alpha * rssi_dbm + (1 - radio_ema_alpha) * *radio_ema
	                    : double(rssi_dbm);
	radio_window.push_back({
	        .when = when,
	        .rssi_dbm = *radio_ema,
	        .link_speed_mbps = std::max(0, link_speed_mbps),
	});

	auto trend = analyse_radio(when);
	if (not trend.usable)
		return {};

	// The deep drop and its rebound are a closed loop of their own; a guess from the radio
	// on top of it would only make the two fight.
	if (st == state::recovering)
		return {};

	// The signal is coming back: let the normal probing upwards resume at once.
	if (radio_hold and trend.delta_db > radio_rise_db and trend.rssi_dbm > radio_low_rssi_dbm)
	{
		radio_hold = false;
		U_LOG_I("Radio-aware bitrate: signal recovering (%+.1f dB over %.1f s, now %.0f dBm), probing allowed again",
		        trend.delta_db,
		        trend.span_s,
		        trend.rssi_dbm);
	}

	// Trigger 1: a real fall, and low enough that the fall has nothing left to eat into.
	const bool falling = trend.delta_db < -radio_fall_db and trend.rssi_dbm < radio_low_rssi_dbm;
	// Trigger 2: the radio's own rate adaptation already gave up half of the PHY rate, and
	// what is left no longer has room for what is being sent.
	const bool starved = trend.link_speed_mbps > 0 and
	                     trend.link_speed_peak_mbps > 0 and
	                     double(trend.link_speed_mbps) <= radio_link_speed_collapse * double(trend.link_speed_peak_mbps) and
	                     double(trend.link_speed_mbps) * 1e6 < radio_link_speed_headroom * double(bitrate);

	if (not falling and not starved)
		return {};

	// Same cooldowns as any other decrease, plus one of its own: a preemptive step is a
	// guess and the frame timings must be given time to confirm or deny it.
	if (when - last_decrease < decrease_cooldown or when - last_radio_step < radio_step_interval)
		return {};

	const uint32_t previous = bitrate;
	bitrate = clamp(uint64_t(bitrate * decrease_factor));

	// Hold the probing back up until the radio says the degradation is over, whether or not
	// the bitrate could actually move (it may already be on the floor).
	radio_hold = true;
	last_decrease = when;
	last_radio_step = when;

	if (bitrate == previous)
		return {};

	// The utilisation samples were taken at the old bitrate and say nothing about the new one.
	flush();

	U_LOG_I("Automatic bitrate: radio degrading, %.1f -> %.1f Mbit/s (%s: RSSI %.0f dBm, %+.1f dB over %.1f s, %.1f dB/s, link %d Mbit/s, peak %d Mbit/s)",
	        previous * to_mbits,
	        bitrate * to_mbits,
	        falling ? "falling signal" : "PHY rate collapse",
	        trend.rssi_dbm,
	        trend.delta_db,
	        trend.span_s,
	        trend.slope_db_per_s,
	        trend.link_speed_mbps,
	        trend.link_speed_peak_mbps);

	return bitrate;
}

std::optional<uint32_t> bitrate_controller::on_feedback(const from_headset::feedback & feedback, int64_t period_ns, bool streaming, clock::time_point now)
{
	std::lock_guard lock(mutex);

	if (not conf.enabled or not client_enabled or not streaming or not ceiling or period_ns <= 0)
		return {};

	// Only the video streams (one per encoder) report frame delivery timings.
	if (feedback.stream_index >= video_stream_count)
		return {};

	frame_period = period_ns;

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

std::optional<uint32_t> bitrate_controller::evaluate(clock::time_point now)
{
	if (now - last_evaluation < evaluation_interval)
		return {};
	last_evaluation = now;

	// The headset stopped reporting its radio: a hold taken on data this old would last
	// forever. Stale radio data does nothing at all, including nothing to the probing.
	if (radio_hold and now - last_radio_sample > radio_max_age)
	{
		radio_hold = false;
		U_LOG_I("Radio-aware bitrate: no Wi-Fi report for %d ms, releasing the hold",
		        int(radio_max_age.count()));
	}

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
			// The deep drop and its rebound are in charge from here; the radio must
			// neither hold the rebound back nor step on top of it.
			radio_hold = false;
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
			// Frame timings look fine, but the radio says the signal is still on the
			// way down: probing back up now only walks into the degradation again.
			if (radio_hold)
				return {};

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
