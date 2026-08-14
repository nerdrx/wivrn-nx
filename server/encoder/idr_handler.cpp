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

#include "idr_handler.h"

#include "util/u_logging.h"
#include "utils/overloaded.h"

namespace wivrn
{
idr_handler::~idr_handler() = default;

void default_idr_handler::on_feedback(const from_headset::feedback & f)
{
	std::unique_lock lock(mutex);
	std::visit(utils::overloaded{
	                   [](need_idr) {},
	                   [](idr_received) {},
	                   [](need_refresh) {},
	                   [this, &f](wait_idr_feedback s) {
		                   if (f.frame_index == s.idr_id)
		                   {
			                   if (f.sent_to_decoder)
			                   {
				                   U_LOG_D("IDR frame received, stream %d", f.stream_index);
				                   state = idr_received{};
			                   }
			                   else
			                   {
				                   U_LOG_W("IDR frame dropped, stream %d", f.stream_index);
				                   state = need_idr{};
			                   }
		                   }
	                   },
	                   [this, &f](refreshing & r) {
		                   // A loss inside a sweep is noted but never restarts it: the sweep
		                   // only makes progress on the frames that actually go out, so
		                   // restarting it on every report from a link that is dropping frames
		                   // would leave the picture permanently half refreshed. Whether the
		                   // sweep worked is judged once, when it ends, in get_type.
		                   if (not f.sent_to_decoder and f.frame_index >= r.first and not is_non_ref_frame(f.frame_index))
			                   r.lost = true;
	                   },
	                   [this, &f](running r) {
		                   if (not f.sent_to_decoder and f.frame_index >= r.first_p and not is_non_ref_frame(f.frame_index))
		                   {
			                   if (refresh_enabled)
			                   {
				                   U_LOG_I("Intra refresh needed on stream %d", f.stream_index);
				                   state = need_refresh{};
			                   }
			                   else
			                   {
				                   U_LOG_I("IDR frame needed on stream %d", f.stream_index);
				                   state = need_idr{};
			                   }
		                   }
	                   },
	           },
	           state);
}

void default_idr_handler::reset()
{
	std::unique_lock lock(mutex);
	U_LOG_D("IDR handler reset");
	// Deliberately a real IDR and never a refresh: reset() is what a reconnect, an encoder
	// reconfiguration and a failover swap all go through, and in all three the headset's
	// decoder holds nothing this encoder could have it predict from. Only recovery from
	// loss on an otherwise running stream takes the gentle path.
	state = need_idr{};
	failed_refreshes = 0;
	non_ref_frames.assign(512, uint64_t(-1));
}

void default_idr_handler::set_intra_refresh(bool enabled, uint32_t sweep_frames)
{
	std::unique_lock lock(mutex);
	refresh_enabled = enabled;
	if (sweep_frames)
		refresh_frames = sweep_frames;

	// Turned off before the sweep it asked for had started: fall back to the keyframe that
	// sweep was replacing. One already under way is left to finish — it is repairing the
	// picture, and an IDR on top of it would cost bandwidth for nothing.
	if (not enabled and std::holds_alternative<need_refresh>(state))
		state = need_idr{};
}

bool default_idr_handler::should_skip(uint64_t frame_id)
{
	std::unique_lock lock(mutex);
	return std::visit(utils::overloaded{
	                          [this, frame_id](wait_idr_feedback w) {
		                          if (frame_id > w.idr_id + 100)
		                          {
			                          U_LOG_W("IDR frame timeout");
			                          state = need_idr{};
			                          return false;
		                          }
		                          return true;
	                          },
	                          // Never skip during a sweep: the intra blocks that repair the
	                          // picture ride the frames, so skipping is skipping the repair.
	                          [](auto) {
		                          return false;
	                          },
	                  },
	                  state);
}

void default_idr_handler::set_non_ref(uint64_t frame_index)
{
	std::unique_lock lock(mutex);
	non_ref_frames[frame_index % non_ref_frames.size()] = frame_index;
}

bool default_idr_handler::is_non_ref_frame(uint64_t frame_index)
{
	return non_ref_frames[frame_index % non_ref_frames.size()] == frame_index;
}

default_idr_handler::frame_type default_idr_handler::get_type(uint64_t frame_index)
{
	std::unique_lock lock(mutex);
	return std::visit(utils::overloaded{
	                          [this, frame_index](need_idr) {
		                          U_LOG_D("IDR frame needed");
		                          state = wait_idr_feedback{frame_index};
		                          return frame_type::i;
	                          },
	                          [this, frame_index](idr_received) {
		                          state = running{frame_index};
		                          return frame_type::p;
	                          },
	                          [this, frame_index](need_refresh) {
		                          U_LOG_D("Intra refresh started");
		                          state = refreshing{
		                                  .first = frame_index,
		                                  .until = frame_index + refresh_frames + refresh_slack_frames,
		                                  .lost = false,
		                          };
		                          return frame_type::refresh;
	                          },
	                          [this, frame_index](refreshing r) {
		                          if (frame_index < r.until)
			                          return frame_type::p;

		                          if (not r.lost)
		                          {
			                          U_LOG_D("Intra refresh complete");
			                          failed_refreshes = 0;
			                          state = running{frame_index};
			                          return frame_type::p;
		                          }

		                          // Frames went missing while the sweep was crossing the picture, so
		                          // part of it was never refreshed. Try again, but not forever: a link
		                          // this bad is one an IDR has a better chance of fixing than another
		                          // sweep it will lose just as much of.
		                          ++failed_refreshes;
		                          if (refresh_enabled and failed_refreshes < max_failed_refreshes)
		                          {
			                          U_LOG_I("Intra refresh lost frames, sweeping again (%u)", failed_refreshes);
			                          state = refreshing{
			                                  .first = frame_index,
			                                  .until = frame_index + refresh_frames + refresh_slack_frames,
			                                  .lost = false,
			                          };
			                          return frame_type::refresh;
		                          }

		                          U_LOG_W("Intra refresh failed %u times, falling back to an IDR frame", failed_refreshes);
		                          failed_refreshes = 0;
		                          state = wait_idr_feedback{frame_index};
		                          return frame_type::i;
	                          },
	                          [](auto) {
		                          return frame_type::p;
	                          },
	                  },
	                  state);
}
} // namespace wivrn
