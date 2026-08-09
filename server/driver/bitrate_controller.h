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

#include "wivrn_packets.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

namespace wivrn
{

// Automatic bitrate control.
//
// The client acknowledges every video frame with a from_headset::feedback packet that carries,
// among others, the time the first and the last packet of that frame were received (both in the
// client clock, so their difference needs no clock offset). The fraction of a frame period spent
// receiving a frame is a direct measure of how much of the link capacity the stream is using:
//
//     utilisation = (received_last_packet - received_first_packet) / frame_period
//
// A utilisation close to (or above) 1 means the wireless link cannot deliver a frame within the
// time budget of a frame, which is exactly what "the connection lags" feels like. Frames that
// never arrive completely (no sent_to_decoder, same test the IDR handler uses) and frames that are
// decoded but dropped before ever being shown are counted as delivery failures.
//
// Two regimes coexist:
//   * gradual degradation (walking away from the router): gentle multiplicative decrease followed
//     by slow additive probing back up;
//   * acute lag spike: a deep drop, which in practice also flushes whatever queue got wedged, then
//     a fast slow-start style rebound back to the pre-drop bitrate. If congestion returns during
//     the rebound the rebound target itself is lowered (the classic ssthresh idea) so the two do
//     not oscillate.
//
// All decisions are taken from a sliding window of per-frame samples, using a high percentile so
// that a single unlucky frame does not move the bitrate. The window is flushed after every change
// so that stale samples cannot trigger a second change.
//
// On top of that, the headset reports its Wi-Fi radio state about once a second
// (from_headset::wifi_state). Frame timings are a *lagging* indicator: by the time the utilisation
// rises the packets are already late. The radio is a *leading* one — the signal starts falling a
// second or two before the rate adaptation gives up and the first packet is lost. The controller
// therefore steps down preemptively on a falling signal, while the frame timings still look fine.
// Only the trend is used: absolute dBm says nothing portable, a fall of several dB over a few
// seconds says the user is walking away from the access point. Radio input can only ever *lower*
// the bitrate; it never raises one, and it never runs while the deep-drop recovery is in charge.
class bitrate_controller
{
public:
	using clock = std::chrono::steady_clock;

	struct config
	{
		// NX default is on
		bool enabled = true;
		// Never go below this, whatever the measurements say
		uint32_t min_bitrate_bps = 10'000'000;
	};

	// --- Measurement window -------------------------------------------------------------
	// Duration of the sliding window of per-frame samples.
	static constexpr std::chrono::milliseconds window_duration{2000};
	// No decision is taken with fewer samples than this in the window.
	static constexpr size_t min_samples = 20;
	// Upper bound on how often the policy is evaluated.
	static constexpr std::chrono::milliseconds evaluation_interval{250};
	// Percentile of the utilisation samples used for every threshold comparison.
	static constexpr double utilisation_percentile = 0.9;
	// Number of frames tracked simultaneously; a frame is turned into a sample once this many
	// newer frames have been seen, which is enough for the late "displayed" feedback to arrive.
	static constexpr size_t frame_ring_size = 16;
	// One video stream per encoder: left, right, alpha. Anything else is not a video frame.
	//
	// Deliberately not the promoted quad layer stream, which is one index further:
	// it is silent on every frame that promotes no layer, and the headset reports the
	// frames it never received as lost, which here would read as a saturated link.
	static constexpr uint8_t video_stream_count = 3;

	// --- Thresholds ---------------------------------------------------------------------
	// p90 utilisation above this: the link is saturated, decrease.
	static constexpr double utilisation_decrease = 0.85;
	// p90 utilisation below this: there is spare capacity, probe upwards.
	// The gap with utilisation_decrease is the hysteresis band, and is mandatory: inside it
	// nothing happens at all.
	static constexpr double utilisation_increase = 0.60;
	// p90 utilisation above this: acute congestion, a frame no longer fits in a frame period.
	static constexpr double utilisation_severe = 1.00;
	// Frames that never arrived completely, counted over the window.
	static constexpr size_t lost_frames_decrease = 1;
	static constexpr size_t lost_frames_severe = 3;
	// Frames decoded but dropped before being displayed, counted over the window.
	static constexpr size_t late_frames_decrease = 4;
	static constexpr size_t late_frames_severe = 10;

	// --- Decrease -----------------------------------------------------------------------
	// Gentle multiplicative decrease, for gradual degradation.
	static constexpr double decrease_factor = 0.7;
	// Deep drop on an acute lag spike. The drop itself is therapeutic: it lets whatever queue
	// piled up on the link drain, after which the same bitrate is usually fine again.
	static constexpr double deep_decrease_factor = 0.4;
	// Minimum time between two decreases, so a single bad patch cannot collapse the bitrate.
	static constexpr std::chrono::milliseconds decrease_cooldown{2000};

	// --- Slow additive increase (above the recovery target) -----------------------------
	// Increase step, whichever is larger.
	static constexpr uint32_t increase_step_min = 2'000'000;
	static constexpr double increase_step_ratio = 0.05; // of the ceiling
	// The link must measure healthy for this long before every increase. As the window is
	// flushed on every change this also acts as the increase cooldown.
	static constexpr std::chrono::milliseconds increase_hold{5000};

	// --- Fast recovery (after a deep drop, up to the pre-drop bitrate) ------------------
	// Healthy period required before the first rebound step.
	static constexpr std::chrono::milliseconds recovery_confirm{2500};
	// Healthy period required before each subsequent rebound step.
	static constexpr std::chrono::milliseconds recovery_step_interval{1000};
	// Rebound steps are multiplicative, so the pre-drop level is reached in a few seconds.
	static constexpr double recovery_factor = 2.0;
	// If congestion comes back while rebounding, lower the rebound target by this factor.
	static constexpr double recovery_target_backoff = 0.75;

	// --- Radio trend (preemptive decrease) ----------------------------------------------
	// Length of the trend window. Long enough that the ±3 dB the radio reports while the
	// user stands still averages out, short enough to still be ahead of the loss.
	static constexpr std::chrono::milliseconds radio_trend_window{4000};
	// A sample older than this is stale: the headset stopped reporting (an older client, a
	// non-Android one, a failed read), and a trend from before the gap says nothing about
	// the link now. Stale data is ignored entirely and releases any hold.
	static constexpr std::chrono::milliseconds radio_max_age{5000};
	// No trend is computed from fewer samples, i.e. not before ~3 s of reports at 1 Hz.
	static constexpr size_t radio_min_samples = 4;
	// Smoothing of the reported RSSI before it is used as "the current level". One sample
	// per second, so this is a time constant of roughly two seconds.
	static constexpr double radio_ema_alpha = 0.4;
	// The fall, in dB over the window, that separates walking away from the access point
	// from the noise of standing still. Read off the least squares slope times the time the
	// window actually covers.
	static constexpr double radio_fall_db = 6.0;
	// ... but a fall only matters once the absolute level is low enough that the radio's
	// rate adaptation is about to start dropping MCS. -65 dBm is roughly where a 5 GHz link
	// leaves its top rates; above it there is margin for the fall to eat.
	static constexpr double radio_low_rssi_dbm = -65.0;
	// Second, independent trigger: the radio's own rate adaptation already halved the PHY
	// rate compared to the best it saw in the window, *and* what is left is less than this
	// multiple of the bitrate being sent. PHY rates are nominal — aggregation, contention
	// and the uplink all take their share — so twice the video bitrate is already the edge.
	static constexpr double radio_link_speed_headroom = 2.0;
	static constexpr double radio_link_speed_collapse = 0.5;
	// Minimum time between two preemptive steps, on top of decrease_cooldown: a preemptive
	// step is a guess, and the frame timings must be given time to confirm or deny it.
	static constexpr std::chrono::milliseconds radio_step_interval{4000};
	// The signal coming back up by this much over the window releases the hold that a
	// preemptive step put on the normal probing-upwards path.
	static constexpr double radio_rise_db = 3.0;

	bitrate_controller() = default;

	// Set the configuration and the initial ceiling. The ceiling is the bitrate the client asked
	// for; the controller never goes above it. client_enabled is the headset side switch: the
	// control only runs when both it and the server configuration are enabled. radio_aware is
	// the headset side switch for the preemptive radio trend, which additionally requires the
	// automatic bitrate to be on at all.
	void configure(const config &, uint32_t ceiling_bps, bool client_enabled, bool radio_aware);

	bool enabled() const;
	uint32_t current() const;

	// The headset toggled its own switch. Starts over, as measurements taken under the other
	// setting say nothing; switching off therefore restores the full ceiling. Returns the
	// bitrate to apply, if any.
	std::optional<uint32_t> set_client_enabled(bool);

	// The headset toggled the radio trend switch. Never changes the bitrate on its own:
	// switching off only stops the preemptive steps and releases any hold they left, the
	// normal AIMD takes it from there.
	void set_radio_aware(bool);

	// A new ceiling was requested (client settings, or a manual change from the dashboard).
	// Resets the controller to it. Returns the bitrate to apply.
	std::optional<uint32_t> set_ceiling(uint32_t ceiling_bps);

	// Extra ceiling imposed by the path currently carrying video (multipath failover), clamped
	// on top of the client's. Empty puts the client ceiling back. Measurements taken on the
	// other path say nothing about this one, so the controller is re-seeded either way. Returns
	// the bitrate to apply — including when the automatic control is off, the path budget is a
	// property of the link and not of the AIMD.
	std::optional<uint32_t> set_path_ceiling(std::optional<uint32_t> ceiling_bps);

	// Forget all measurements and go back to the ceiling, e.g. when the session is resumed.
	std::optional<uint32_t> reset();

	// Feed one feedback packet. frame_period_ns is the current video frame period (from the
	// pacer), streaming tells whether the stream is up at all. Returns a new bitrate to apply,
	// if any. now is injectable so the policy can be driven on a virtual clock by the tests;
	// leaving it out is the real thing.
	std::optional<uint32_t> on_feedback(const from_headset::feedback &, int64_t frame_period_ns, bool streaming, clock::time_point now = clock::now());

	// Feed one Wi-Fi radio report from the headset. Only call it for a sample the headset
	// marked valid; obviously impossible values (a non-negative or absurdly low RSSI, the
	// -127 sentinel) are rejected here as well. link_speed_mbps <= 0 means unknown, the rest
	// of the sample is still used. Returns a lower bitrate to apply, if the trend calls for a
	// preemptive step; never a higher one.
	std::optional<uint32_t> on_wifi_state(int rssi_dbm, int link_speed_mbps, clock::time_point when = clock::now());

private:
	// State of one video frame while its per-stream feedback packets are being collected.
	struct frame_state
	{
		uint64_t index = uint64_t(-1);
		XrTime first = 0; // earliest received_first_packet over all streams
		XrTime last = 0;  // latest received_last_packet over all streams
		bool valid = false;
		bool lost = false; // at least one stream never arrived completely
		bool late = false; // decoded but dropped before being displayed
	};

	struct sample
	{
		clock::time_point when;
		float utilisation = 0;
		bool lost = false;
		bool late = false;
	};

	// One Wi-Fi report, after the sentinel filtering.
	struct radio_sample
	{
		clock::time_point when;
		double rssi_dbm = 0; // smoothed, not the raw report
		int link_speed_mbps = 0;
	};

	// What the window says about the radio right now.
	struct radio_trend
	{
		bool usable = false;
		double rssi_dbm = 0; // smoothed current level
		double slope_db_per_s = 0;
		double span_s = 0;
		// Change over the window, positive while the signal improves
		double delta_db = 0;
		int link_speed_mbps = 0;
		int link_speed_peak_mbps = 0;
	};

	enum class state
	{
		// Slow AIMD around the current bitrate.
		steady,
		// Below a remembered pre-drop bitrate after a deep drop, rebounding fast.
		recovering,
	};

	struct stats
	{
		double utilisation = 0; // high percentile over the window
		size_t lost = 0;
		size_t late = 0;
		size_t count = 0;
	};

	mutable std::mutex mutex;

	config conf;
	// Headset side switch, ANDed with conf.enabled
	bool client_enabled = true;
	// Headset side switch for the radio trend, ANDed with the two above
	bool radio_aware = true;
	// Ceiling requested by the client
	uint32_t ceiling = 0;
	// Ceiling of the path carrying video, if it is more restrictive
	std::optional<uint32_t> path_ceiling;
	uint32_t bitrate = 0;
	uint32_t min_bitrate = 0;

	state st = state::steady;
	uint32_t recovery_target = 0;
	bool first_recovery_step = true;

	std::array<frame_state, frame_ring_size> frames;
	uint64_t newest_frame = 0;
	bool has_frames = false;
	// Current video frame period in ns, from the pacer
	int64_t frame_period = 0;

	std::deque<sample> window;
	std::optional<clock::time_point> healthy_since;
	clock::time_point last_evaluation{};
	clock::time_point last_decrease{};

	std::deque<radio_sample> radio_window;
	// Smoothed RSSI carried across samples, empty until the first one
	std::optional<double> radio_ema;
	clock::time_point last_radio_sample{};
	clock::time_point last_radio_step{};
	// A preemptive step was taken and the radio has not recovered since: hold the normal
	// probing upwards, it would only walk straight back into the degradation. Released by a
	// recovering signal, by the radio data going stale, and by every reset.
	bool radio_hold = false;

	// Ceiling actually in force: the client's, clamped by the current path's
	uint32_t effective_ceiling() const;
	// reset(), with the mutex already held
	std::optional<uint32_t> reset_locked();
	void close_frame(frame_state &, clock::time_point now);
	void flush();
	// Forget the radio trend and release the hold
	void flush_radio();
	stats analyse(clock::time_point now);
	radio_trend analyse_radio(clock::time_point now);
	std::optional<uint32_t> evaluate(clock::time_point now);
	uint32_t clamp(uint64_t value) const;
};

} // namespace wivrn
