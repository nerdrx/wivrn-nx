/*
 * WiVRn VR streaming — NX Warp codec
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "nxwarp_decoder.h"

#include "wivrn_config.h"

#if WIVRN_USE_NXWARP

#include <spdlog/spdlog.h>

#include <chrono>

#include <nxvc/transport/wire.h>

namespace
{
// The transport takes a client-clock microsecond stamp and only ever compares stamps with
// each other, so the steady clock is the right one.
uint64_t now_us()
{
	return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
	                        std::chrono::steady_clock::now().time_since_epoch())
	                        .count());
}

vk::raii::Sampler make_sampler(vk::raii::Device & device, vk::SamplerYcbcrConversion conv)
{
	vk::StructureChain info{
	        vk::SamplerCreateInfo{
	                .magFilter = vk::Filter::eLinear,
	                .minFilter = vk::Filter::eLinear,
	                .mipmapMode = vk::SamplerMipmapMode::eNearest,
	                .addressModeU = vk::SamplerAddressMode::eClampToEdge,
	                .addressModeV = vk::SamplerAddressMode::eClampToEdge,
	                .addressModeW = vk::SamplerAddressMode::eClampToEdge,
	                .maxAnisotropy = 1,
	        },
	        vk::SamplerYcbcrConversionInfo{
	                .conversion = conv,
	        },
	};
	return vk::raii::Sampler(device, info.get());
}

struct nxwarp_blit_handle : public wivrn::decoder::blit_handle
{
	std::atomic_bool & free;
	nxwarp_blit_handle(const wivrn::from_headset::feedback & feedback,
	                   const wivrn::to_headset::video_stream_data_shard::view_info_t & view_info,
	                   vk::ImageView image_view,
	                   vk::Image image,
	                   vk::Extent2D extent,
	                   vk::ImageLayout & current_layout,
	                   vk::Semaphore semaphore,
	                   uint64_t & semaphore_val,
	                   std::atomic_bool & free) :
	        wivrn::decoder::blit_handle{feedback, view_info, image_view, image, extent, current_layout, semaphore, &semaphore_val},
	        free(free) {}
	~nxwarp_blit_handle()
	{
		free = true;
	}
};

// path_id 0xFF is not a path: it marks the codec's raw stream header, sent on the control
// socket. See server/encoder/nxwarp_packetize.h.
constexpr uint8_t kStreamHeaderPath = 0xFF;
} // namespace

namespace wivrn
{

nxwarp_decoder::nxwarp_decoder(vk::raii::Device & device,
                               vk::raii::PhysicalDevice & physical_device,
                               uint32_t vk_queue_family_index,
                               const wivrn::to_headset::video_stream_description & description,
                               uint8_t stream_index,
                               nxwarp_host & host,
                               shard_accumulator * accumulator) :
        device(device),
        physical_device(physical_device),
        queue_family_index(vk_queue_family_index),
        ycbcr_conversion(device, vk::SamplerYcbcrConversionCreateInfo{
                                         .format = vk::Format::eG8B8R82Plane420Unorm,
                                         .ycbcrModel = vk::SamplerYcbcrModelConversion::eYcbcr709,
                                         .ycbcrRange = vk::SamplerYcbcrRange::eItuFull,
                                         .chromaFilter = vk::Filter::eLinear,
                                 }),
        sampler_(make_sampler(device, *ycbcr_conversion)),
        command_pool(device, vk::CommandPoolCreateInfo{
                                     .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                                     .queueFamilyIndex = vk_queue_family_index,
                             }),
        cmd(device.allocateCommandBuffers(vk::CommandBufferAllocateInfo{
                                                  .commandPool = *command_pool,
                                                  .commandBufferCount = 1,
                                          })[0]
                    .release()),
        fence(device, vk::FenceCreateInfo{.flags = vk::FenceCreateFlagBits::eSignaled}),
        stream_index(stream_index),
        extent{
                .width = description.stream_size(stream_index).first,
                .height = description.stream_size(stream_index).second,
        },
        host(host),
        accumulator(accumulator)
{
	for (auto & item: image_pool)
	{
		item.image = image_allocation(
		        device,
		        vk::ImageCreateInfo{
		                .imageType = vk::ImageType::e2D,
		                .format = vk::Format::eG8B8R82Plane420Unorm,
		                .extent = {.width = extent.width, .height = extent.height, .depth = 1},
		                .mipLevels = 1,
		                .arrayLayers = 1,
		                .tiling = vk::ImageTiling::eOptimal,
		                .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
		        },
		        {.usage = VMA_MEMORY_USAGE_AUTO},
		        "nxwarp image");

		vk::SamplerYcbcrConversionInfo conv{.conversion = *ycbcr_conversion};
		item.view_full = vk::raii::ImageView(
		        device,
		        vk::ImageViewCreateInfo{
		                .pNext = &conv,
		                .image = item.image,
		                .viewType = vk::ImageViewType::e2D,
		                .format = vk::Format::eG8B8R82Plane420Unorm,
		                .subresourceRange = {
		                        .aspectMask = vk::ImageAspectFlagBits::eColor,
		                        .levelCount = 1,
		                        .layerCount = 1,
		                },
		        });

		// The Pico 4's Adreno 650 driver advertises timeline semaphores and refuses to
		// create one (vkCreateSemaphore returns VK_INCOMPLETE); nxvc's own decoder falls
		// back to a fence for the same reason. Without one, the copy below is waited for
		// on the host and the frame is published with no semaphore, which the render
		// thread already treats as "nothing to wait for".
		if (not host_sync)
		{
			try
			{
				item.semaphore = vk::raii::Semaphore(
				        device,
				        vk::StructureChain{
				                vk::SemaphoreCreateInfo{},
				                vk::SemaphoreTypeCreateInfo{.semaphoreType = vk::SemaphoreType::eTimeline},
				        }
				                .get());
			}
			catch (const std::exception & e)
			{
				host_sync = true;
				spdlog::warn("nxwarp[{}]: timeline semaphore unavailable ({}); frames will be fenced on the host", stream_index, e.what());
			}
		}
	}
	if (host_sync)
		for (auto & item: image_pool)
			item.semaphore = nullptr;

	// The band deadlines are fractions of one frame; take the rate from the stream
	// description and fall back to 90 Hz when it says nothing usable.
	if (description.frame_rate > 1.0f)
		frame_period_us = uint32_t(1'000'000.0f / description.frame_rate);

	worker = std::thread([this]() {
		try
		{
			while (true)
			{
				auto job = jobs.pop();
				jobs_pending--;
				decode_unit(job);
			}
		}
		catch (const utils::sync_queue_closed &)
		{
		}
		catch (const std::exception & e)
		{
			spdlog::error("nxwarp decoder worker: {}", e.what());
		}
	});
}

nxwarp_decoder::~nxwarp_decoder()
{
	jobs.close();
	if (worker.joinable())
		worker.join();
	if (nxvc)
		nxvc_vk_decoder_destroy(nxvc);
	spdlog::info("nxwarp[{}]: {} frames decoded, {} dropped with a hole, {} refused by the codec, {} published with no view_info",
	             stream_index, frames_decoded, frames_dropped_holes, frames_dropped_codec, frames_no_view_info);
}

// Network thread. The first thing that arrives on an NX Warp stream, and the only thing
// that can create the decoder and the receiver: the stream header carries the geometry
// both of them are sized from.
bool nxwarp_decoder::on_stream_header(std::span<const uint8_t> header)
{
	if (nxvc or nxvc_failed or header.empty())
		return nxvc != nullptr;

	nxvc_vkd_create_info ci;
	nxvc_vk_decoder_create_info_default(&ci);
	// Device adoption: the WiVRn client owns the VkDevice and the queue, and nxvc must
	// allocate nothing it does not own (nxvc_vk.h, vk/common). All five handles or none.
	ci.instance = host.instance();
	ci.physical_device = *physical_device;
	ci.device = *device;
	host.with_queue([&](vk::Queue q) { ci.queue = q; });
	ci.queue_family = queue_family_index;
	// Two-plane 4:2:0 passthrough: what the reprojection shader already samples, and half
	// the reference-ring memory of an RGBA8 store.
	ci.output_format = NXVC_VKD_OUT_YCBCR420;
	ci.flags = 0;

	if (auto st = nxvc_vk_decoder_create(&ci, &nxvc); st != NXVC_VKD_OK)
	{
		spdlog::error("nxwarp[{}]: nxvc_vk_decoder_create: {}", stream_index,
		              nxvc_vk_decoder_status_string(st));
		nxvc = nullptr;
		nxvc_failed = true;
		return false;
	}

	size_t consumed = 0;
	if (auto st = nxvc_vk_decoder_parse_stream_header(nxvc, header.data(), header.size(), &consumed);
	    st != NXVC_VKD_OK)
	{
		spdlog::error("nxwarp[{}]: stream header refused: {} ({})", stream_index,
		              nxvc_vk_decoder_status_string(st), nxvc_vk_decoder_last_error(nxvc));
		nxvc_vk_decoder_destroy(nxvc);
		nxvc = nullptr;
		nxvc_failed = true;
		return false;
	}

	nxvc_vkd_stream_info si{};
	nxvc_vk_decoder_stream_info(nxvc, &si);
	if (not si.tiles_x or not si.tile_count)
	{
		spdlog::error("nxwarp[{}]: stream header has an empty tile grid", stream_index);
		nxvc_failed = true;
		return false;
	}

	// Every field here is fixed by the contract in server/encoder/nxwarp_packetize.h and
	// video_encoder_nxwarp.cpp. They must agree exactly: the receiver derives its nonces,
	// its band boundaries and its run payload budget from them, so a single disagreement
	// shows up as an authentication failure on every datagram rather than as anything
	// diagnostic.
	cfg.stream_id = stream_index;
	cfg.cols = uint16_t(si.tiles_x);
	cfg.rows = uint16_t(si.tile_count / si.tiles_x);
	cfg.band_rows = uint16_t(std::min<uint32_t>(cfg.rows, 6));
	cfg.layers = 1;
	cfg.mtu = 1280;
	cfg.caps = nxt::kCapFec | nxt::kCapPoseHdr | nxt::kCapRleFeedback;

	// nxvc_transport never generates or exchanges keys; the integration supplies them.
	// This transport rides inside WiVRn's own authenticated, encrypted stream socket, so a
	// second real AEAD would encrypt ciphertext. The NullAead is keyed and detects
	// corruption but is NOT cryptography, and it is correct here only because of that
	// outer layer. The constants match video_encoder_nxwarp.cpp byte for byte.
	aead = nxt::make_null_aead();
	nxt::Key key{}, salt{};
	for (size_t i = 0; i < key.size(); ++i)
	{
		key[i] = uint8_t(i);
		salt[i] = uint8_t(0xA0 + i);
	}
	receiver = std::make_unique<nxt::Receiver>(cfg, aead.get(), key, salt);
	receiver->set_negotiated_caps(cfg.caps);

	chunk = nxwarp_wire::chunk_bytes(cfg);
	// Every entry of the window is sized once, here, and keeps its allocation for the
	// life of the stream: the network thread must not be allocating per-tile vectors.
	for (auto & f: window)
	{
		f = {};
		f.slots.assign(cfg.tiles_per_frame(), {});
		f.band_fired.assign(cfg.bands(), 0);
	}
	seen_any_frame = false;
	any_retired = false;

	spdlog::info("nxwarp[{}]: {}x{} on {}, {} x {} tiles, {} bytes per tile",
	             stream_index, si.width, si.height, nxvc_vk_decoder_device_name(nxvc),
	             cfg.cols, cfg.rows, chunk);
	return true;
}

void nxwarp_decoder::push_datagram(to_headset::nxwarp_datagram && dg)
{
	if (dg.path_id == kStreamHeaderPath)
	{
		on_stream_header(dg.payload);
		return;
	}
	if (not receiver or dg.payload.size() < nxt::kHeaderBytes)
		return;

	nxt::DatagramHeader h{};
	if (not nxt::decode_header(dg.payload.data(), &h))
		return;

	const uint16_t frame_id = uint16_t(h.frame_id);

	// Out of order means "belongs to a frame older than the newest one seen", which is
	// the only kind of reordering that costs anything: within a frame the tiles are
	// placed by index and the order they arrive in does not matter.
	const bool out_of_order = seen_any_frame and seq_lt(frame_id, newest_frame);
	if (out_of_order)
		++net_out_of_order;
	if (not seen_any_frame or seq_lt(newest_frame, frame_id))
	{
		newest_frame = frame_id;
		seen_any_frame = true;
	}

	// A newer frame beyond the window closes the frames that have fallen out of it.
	evict_below_window();

	// A datagram for a frame that has already been closed -- retired, or now below the
	// window floor -- can no longer change any outcome. Dropping it is what keeps
	// `retired` monotonic, and therefore what keeps the cascade from ever starting.
	if (any_retired and not seq_lt(retired_frame, frame_id))
	{
		++stragglers_dropped;
		return;
	}
	if (seq_lt(frame_id, uint16_t(newest_frame - (kFrameWindow - 1))))
	{
		++stragglers_dropped;
		return;
	}

	inflight_frame * f = frame_slot(frame_id, dg.path_id);
	if (not f)
		return;
	if (out_of_order)
		f->reordered = true;
	f->path_id = dg.path_id;

	// The pose rides the frame's first datagram and only that one; take it whichever
	// datagram it turns up on, so that reordering on the wire does not lose it.
	if (dg.view_info)
	{
		f->view_info = *dg.view_info;
		f->have_view_info = true;
	}

	const uint64_t arrival_us = now_us();
	std::vector<nxt::TileOutput> tiles;
	receiver->on_datagram(std::span<const uint8_t>(dg.payload.data(), dg.payload.size()),
	                      dg.path_id, arrival_us, &tiles);

	// The receiver may deliver tiles of any frame it currently holds, not only this
	// datagram's -- a parity datagram repairs the frame it belongs to. Route each tile
	// to its own frame's entry; a tile for a frame outside the window has nowhere to go
	// and is dropped, which is the same statement as the straggler check above.
	for (const auto & t: tiles)
	{
		if (t.layer_id != 0)
			continue;
		inflight_frame * target = t.frame_id == f->frame_id ? f : nullptr;
		if (not target)
		{
			for (auto & w: window)
			{
				if (w.used and w.frame_id == t.frame_id)
				{
					target = &w;
					break;
				}
			}
		}
		if (not target)
			continue;
		const uint32_t idx = cfg.tile_index(t.row, t.col);
		if (idx >= target->slots.size())
			continue;
		// The receiver's spans point into scratch it reuses on the next call, so the
		// bytes have to be taken now.
		target->slots[idx].assign(t.bytes.begin(), t.bytes.end());
	}

	f->fb.received_last_packet = host.now();

	// Band deadlines. See THE BAND DEADLINE POLICY in the header: never on this frame's
	// own datagram order, because ordinary within-frame reordering then reports almost
	// every tile that did arrive as late.
	//
	// A later frame carrying data for band b is direct evidence that band b of every
	// older frame is finished: the sender emits frames in order.
	if (h.band < cfg.bands() and not h.is_parity())
	{
		for (auto & w: window)
		{
			if (w.used and seq_lt(w.frame_id, frame_id))
				fire_bands_through(w, h.band);
		}
	}
	// And the clock, for the bands no later frame has spoken for yet -- including the
	// case the arrival rule cannot cover at all, a sender that went quiet.
	fire_elapsed_deadlines(arrival_us);

	if (h.flags & nxt::kFlagLastRunOfFrame)
		f->have_last_run = true;

	// A frame is complete when its last run has arrived AND its bytes are whole -- the
	// second half matters because the last run's datagram routinely overtakes an earlier
	// one of the same frame, and closing on the flag alone closes the frame with a hole
	// it was about to fill. Checked for every entry, not just this datagram's: a parity
	// datagram repairs the frame it belongs to, which need not be this one.
	for (auto & w: window)
	{
		if (w.used and w.have_last_run and not w.complete and
		    nxwarp_wire::is_complete(cfg, w.slots, chunk))
			w.complete = true;
	}

	// A complete frame goes to the worker as soon as every older frame has been closed,
	// so the worker sees frames in frame order.
	close_complete_prefix();
}

// End of stream. Closes everything still in the window, oldest first, so a frame whose
// tail was merely late is not counted as lost because nothing came after it to push it
// out. Same thread as push_datagram.
void nxwarp_decoder::flush_frames()
{
	while (auto * f = oldest_in_flight())
		close_frame(*f);
}

nxwarp_decoder::inflight_frame * nxwarp_decoder::oldest_in_flight()
{
	inflight_frame * oldest = nullptr;
	for (auto & f: window)
	{
		if (not f.used)
			continue;
		if (not oldest or seq_lt(f.frame_id, oldest->frame_id))
			oldest = &f;
	}
	return oldest;
}

// Find the entry for `frame_id`, opening one if this is the frame's first datagram. The
// caller has already established that the frame is inside the window and not retired, so
// a free entry always exists: the window holds at most kFrameWindow frames and the window
// itself spans exactly kFrameWindow ids.
nxwarp_decoder::inflight_frame * nxwarp_decoder::frame_slot(uint16_t frame_id, uint8_t path_id)
{
	for (auto & f: window)
	{
		if (f.used and f.frame_id == frame_id)
			return &f;
	}
	inflight_frame * free_slot = nullptr;
	for (auto & f: window)
	{
		if (not f.used)
		{
			free_slot = &f;
			break;
		}
	}
	if (not free_slot)
	{
		// Cannot happen by the argument above; if it ever does, make room the same way
		// the window does rather than dropping the newest frame on the floor.
		if (auto * oldest = oldest_in_flight())
		{
			close_frame(*oldest);
			free_slot = oldest;
		}
	}
	if (not free_slot)
		return nullptr;

	for (auto & slot: free_slot->slots)
		slot.clear();
	std::fill(free_slot->band_fired.begin(), free_slot->band_fired.end(), 0);
	free_slot->used = true;
	free_slot->frame_id = frame_id;
	free_slot->have_last_run = false;
	free_slot->complete = false;
	free_slot->reordered = false;
	free_slot->path_id = path_id;
	free_slot->fb = from_headset::feedback{};
	free_slot->fb.stream_index = stream_index;
	free_slot->fb.received_first_packet = host.now();
	free_slot->first_us = now_us();
	free_slot->view_info = {};
	free_slot->have_view_info = false;
	return free_slot;
}

// Close the leading run of complete frames. A frame that completed while an older one is
// still in flight waits for it, which is what keeps the worker's input in frame order;
// the wait is bounded by the window, because the older frame is closed the moment a frame
// kFrameWindow ahead of it arrives.
void nxwarp_decoder::close_complete_prefix()
{
	while (auto * f = oldest_in_flight())
	{
		if (not f->complete)
			break;
		close_frame(*f);
	}
}

// Everything below the window floor has had its chance: a newer frame is that far ahead,
// so nothing more is coming. Oldest first, so frames still reach the worker in order.
void nxwarp_decoder::evict_below_window()
{
	if (not seen_any_frame)
		return;
	const uint16_t floor = uint16_t(newest_frame - (kFrameWindow - 1));
	while (auto * f = oldest_in_flight())
	{
		if (not seq_lt(f->frame_id, floor))
			break;
		close_frame(*f);
	}
}

// The clock half of the band deadline policy: band b of a frame is due at that frame's
// first arrival plus (b + 1) / bands of the frame period. Runs over every frame in flight,
// so a frame whose sender went quiet still reports.
void nxwarp_decoder::fire_elapsed_deadlines(uint64_t now)
{
	const size_t bands = cfg.bands();
	if (not bands)
		return;
	for (auto & f: window)
	{
		if (not f.used)
			continue;
		for (uint8_t b = 0; b < bands; ++b)
		{
			if (f.band_fired[b])
				continue;
			const uint64_t due = f.first_us + (uint64_t(b) + 1) * frame_period_us / bands;
			if (now < due)
				break; // deadlines are increasing in b; nothing later is due either
			fire_bands_through(f, b);
		}
	}
}

void nxwarp_decoder::fire_bands_through(inflight_frame & f, uint8_t last_band)
{
	for (uint8_t b = 0; b <= last_band and b < f.band_fired.size(); ++b)
	{
		if (f.band_fired[b])
			continue;
		f.band_fired[b] = 1;
		auto packet = receiver->band_deadline(f.frame_id, b, now_us(), 0, f.path_id);
		if (packet.empty())
			continue;
		host.send_feedback(stream_index, f.path_id, std::move(packet));
	}
}

// Network thread. Retires one frame of the window and hands it to the worker. Called only
// on the oldest frame in flight (close_complete_prefix, evict_below_window, flush_frames),
// which is what makes `retired_frame` monotonic and the worker's input frame-ordered.
void nxwarp_decoder::close_frame(inflight_frame & f)
{
	if (not f.used)
		return;
	f.used = false;
	// The frame's remaining band deadlines, so its feedback goes out whether it arrived
	// whole or not -- unchanged, and still the only thing that tells the encoder to
	// refresh what the headset never received.
	fire_bands_through(f, uint8_t(cfg.bands() - 1));
	if (not any_retired or seq_lt(retired_frame, f.frame_id))
	{
		retired_frame = f.frame_id;
		any_retired = true;
	}

	auto unit = nxwarp_wire::reassemble(cfg, f.slots, chunk);
	// The network side of the two-second report: what arrived, what had a hole,
	// and how deep the worker's queue is. Printed here because a stalled worker
	// prints nothing at all, which is exactly the case this line exists for.
	{
		net_frames++;
		if (unit.empty())
			net_holes++;
		// A frame that arrived whole only because the window held it open after a newer
		// frame had already started. This is the count that says what the window is
		// worth: every one of these was a hole before it existed.
		if (not unit.empty() and f.reordered)
			net_late_completed++;
		const auto now = std::chrono::steady_clock::now();
		if (now - net_since > std::chrono::seconds(2))
		{
			const uint64_t placed = receiver->stats.tiles_placed - net_tiles_placed_at;
			const uint64_t late = receiver->stats.tiles_late - net_tiles_late_at;
			spdlog::info("nxwarp[{}] net: {} frames closed in {:.1f} s, {} with a hole, {} out-of-order datagrams, {} frames completed late, {} tiles placed of which {} after their band deadline, {} queued for the worker, {} decoded so far, {} stragglers dropped",
			             stream_index, net_frames, std::chrono::duration<double>(now - net_since).count(), net_holes,
			             net_out_of_order, net_late_completed, placed, late,
			             jobs_pending.load(), frames_decoded, stragglers_dropped);
			net_tiles_placed_at = receiver->stats.tiles_placed;
			net_tiles_late_at = receiver->stats.tiles_late;
			net_frames = 0;
			net_holes = 0;
			net_out_of_order = 0;
			net_late_completed = 0;
			net_since = now;
		}
	}
	if (unit.empty())
	{
		// A hole, a short chunk in the middle, or fewer bytes than the length prefix
		// declares. The feedback for the band has already gone out, which is how the
		// encoder learns to refresh.
		++frames_dropped_holes;
		return;
	}

	host.on_frame_unit(unit);

	decode_job job;
	job.frame_id = f.frame_id;
	job.unit = std::move(unit);
	job.fb = f.fb;
	// Numbered here rather than when the frame opened: frames are closed in frame order,
	// but they are *opened* in arrival order, and a frame whose first datagram was late
	// would otherwise carry a frame_index out of step with everything else.
	job.fb.frame_index = ++wivrn_frame_idx;
	job.view_info = f.view_info;
	job.have_view_info = f.have_view_info;
	// The network delivers at the server's rate; the worker decodes at whatever
	// rate this device manages. When the device cannot keep up, a queued frame is
	// nothing but latency the user will wear -- 90 fps arriving against a 57 ms
	// decode grew the queue past 2000 frames on an Adreno 650, i.e. minutes of
	// lag. Keep only the newest: on a live stream late is worse than missing.
	//
	// This is sound while the stream is all-intra (every frame stands alone).
	// An inter stream must additionally tell the encoder what it dropped --
	// nxvc_vk_decoder_mark_missing() plus the received-tiles feedback -- or the
	// encoder's shadow of the client's reference ring drifts (SYNTAX 6.11,
	// INTEGRATION-DECISIONS 6). Wire that up before enabling inter here.
	if (jobs_pending.load() >= kMaxQueuedFrames)
	{
		jobs.drop_until([](const decode_job &) { return false; });
		frames_dropped_late += jobs_pending.exchange(0);
	}
	jobs_pending++;
	jobs.push(std::move(job));
}

void nxwarp_decoder::decode_unit(decode_job & job)
{
	size_t consumed = 0;
	VkSemaphore dec_sem = VK_NULL_HANDLE;
	uint64_t dec_val = 0;
	bool refused = false;
	// nxvc submits on the queue it adopted, which is the host's one graphics queue; hold
	// the same lock every other submitter in the process holds.
	// The decoder owns one command buffer, one staging buffer and (since the
	// inter path) a reference ring that frame N writes and frame N+1 reads.
	// Until nxvc grows a command-buffer ring, an async submit must be fenced
	// before the next record: wait here, not at present time -- and outside
	// the queue lock, since a host wait needs no queue and the render thread
	// does.
	const auto t_wait0 = std::chrono::steady_clock::now();
	nxvc_vk_decoder_wait(nxvc, UINT64_MAX);
	const auto t_decode0 = std::chrono::steady_clock::now();
	host.with_queue([&](vk::Queue) {
		auto st = nxvc_vk_decode_frame_ex(nxvc, job.unit.data(), job.unit.size(),
		                                  NXVC_VKD_SUBMIT_ASYNC, &consumed);
		if (st != NXVC_VKD_OK)
		{
			spdlog::warn("nxwarp[{}]: frame {} refused: {} ({})", stream_index, job.frame_id,
			             nxvc_vk_decoder_status_string(st), nxvc_vk_decoder_last_error(nxvc));
			++frames_dropped_codec;
			refused = true;
			return;
		}
		dec_sem = nxvc_vk_decoder_timeline(nxvc);
		dec_val = nxvc_vk_decoder_timeline_value(nxvc);
	});
	if (refused)
		return;

	nxvc_vkd_images img{};
	if (nxvc_vk_decoder_images(nxvc, &img) != NXVC_VKD_OK or img.count < 2)
	{
		++frames_dropped_codec;
		return;
	}

	auto item = get_free();
	if (not item)
	{
		spdlog::warn("nxwarp[{}]: no image available in pool, discard frame", stream_index);
		return;
	}

	if (device.waitForFences(*fence, true, UINT64_MAX) != vk::Result::eSuccess)
		spdlog::warn("nxwarp: waitForFences failed");

	cmd.reset();
	cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

	// nxvc leaves its output in GENERAL and overwrites it in place on the next frame; the
	// copy below is what decouples the codec's own images from the pool of frames the
	// render thread picks from.
	std::array<vk::ImageMemoryBarrier, 3> pre{
	        vk::ImageMemoryBarrier{
	                .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
	                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
	                .oldLayout = vk::ImageLayout::eGeneral,
	                .newLayout = vk::ImageLayout::eGeneral,
	                .image = img.image[0],
	                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
	        },
	        vk::ImageMemoryBarrier{
	                .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
	                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
	                .oldLayout = vk::ImageLayout::eGeneral,
	                .newLayout = vk::ImageLayout::eGeneral,
	                .image = img.image[1],
	                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
	        },
	        vk::ImageMemoryBarrier{
	                .srcAccessMask = vk::AccessFlagBits::eNone,
	                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
	                .oldLayout = vk::ImageLayout::eUndefined,
	                .newLayout = vk::ImageLayout::eTransferDstOptimal,
	                .image = item->image,
	                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
	        },
	};
	item->current_layout = vk::ImageLayout::eTransferDstOptimal;
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                    vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, pre);

	vk::ImageCopy luma{
	        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
	        .dstSubresource = {vk::ImageAspectFlagBits::ePlane0, 0, 0, 1},
	        .extent = {std::min(img.width[0], extent.width), std::min(img.height[0], extent.height), 1},
	};
	vk::ImageCopy chroma{
	        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
	        .dstSubresource = {vk::ImageAspectFlagBits::ePlane1, 0, 0, 1},
	        .extent = {std::min(img.width[1], extent.width / 2), std::min(img.height[1], extent.height / 2), 1},
	};
	cmd.copyImage(img.image[0], vk::ImageLayout::eGeneral, item->image,
	              vk::ImageLayout::eTransferDstOptimal, luma);
	cmd.copyImage(img.image[1], vk::ImageLayout::eGeneral, item->image,
	              vk::ImageLayout::eTransferDstOptimal, chroma);

	vk::ImageMemoryBarrier to_read{
	        .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
	        .dstAccessMask = vk::AccessFlagBits::eShaderRead,
	        .oldLayout = item->current_layout,
	        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	        .image = item->image,
	        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
	};
	item->current_layout = to_read.newLayout;
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                    vk::PipelineStageFlagBits::eFragmentShader, {}, {}, {}, to_read);
	cmd.end();

	// The pose, the fov and the foveation runs the reprojection pass needs come off the
	// frame's first datagram (to_headset::nxwarp_datagram::view_info), and nothing else
	// carries them: the nxt::PoseHeader inside the payload is quantised and has no fov.
	//
	// This is reachable, and not only as a bug. The transport's own FEC rebuilds a lost
	// datagram's *tiles* from the parity of its group, but view_info rides on the WiVRn
	// packet around it, so a frame whose first datagram was lost and whose tiles were
	// then recovered arrives whole with no pose. It is rare and it is a loss of quality
	// rather than of the picture, so say it once, count it, and publish the default
	// rather than dropping a frame that did decode.
	if (not job.have_view_info)
	{
		++frames_no_view_info;
		if (not warned_view_info)
		{
			warned_view_info = true;
			spdlog::warn("nxwarp[{}]: frame {} decoded with no view_info on its first datagram "
			             "(lost, and its tiles recovered by FEC); published with a default pose "
			             "and foveation (see docs/nxwarp.md)",
			             stream_index, job.frame_id);
		}
	}

	job.fb.sent_to_decoder = host.now();
	job.fb.received_from_decoder = job.fb.sent_to_decoder;
	auto handle = std::make_shared<nxwarp_blit_handle>(
	        job.fb,
	        job.view_info,
	        *item->view_full,
	        item->image,
	        extent,
	        item->current_layout,
	        *item->semaphore,
	        item->semaphore_val,
	        item->free);

	// nxvc returns no timeline when it is on its fence fallback: the decode is then
	// complete on the host before the copy is even recorded (see the wait above the
	// decode call), so there is nothing for the queue to wait on.
	if (dec_sem == VK_NULL_HANDLE)
		nxvc_vk_decoder_wait(nxvc, UINT64_MAX);
	const bool signal_on_queue = *item->semaphore != VK_NULL_HANDLE;

	device.resetFences(*fence);
	host.with_queue([&](vk::Queue queue) {
		const vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eTransfer;
		const uint64_t signal_val = ++item->semaphore_val;
		std::array<vk::Semaphore, 1> wait{dec_sem};
		std::array<vk::Semaphore, 1> signal{*item->semaphore};
		const uint32_t n_wait = dec_sem != VK_NULL_HANDLE ? 1 : 0;
		const uint32_t n_signal = signal_on_queue ? 1 : 0;
		queue.submit(
		        vk::StructureChain{
		                vk::SubmitInfo{
		                        .waitSemaphoreCount = n_wait,
		                        .pWaitSemaphores = wait.data(),
		                        .pWaitDstStageMask = &wait_stage,
		                        .commandBufferCount = 1,
		                        .pCommandBuffers = &cmd,
		                        .signalSemaphoreCount = n_signal,
		                        .pSignalSemaphores = signal.data(),
		                },
		                vk::TimelineSemaphoreSubmitInfo{
		                        .waitSemaphoreValueCount = n_wait,
		                        .pWaitSemaphoreValues = &dec_val,
		                        .signalSemaphoreValueCount = n_signal,
		                        .pSignalSemaphoreValues = &signal_val,
		                },
		        }
		                .get(),
		        *fence);
	});
	// No semaphore to hand the render thread: make the copy complete before it can see
	// the frame. One frame of pipelining lost, on the driver that gives no other choice.
	if (not signal_on_queue and device.waitForFences(*fence, true, UINT64_MAX) != vk::Result::eSuccess)
		spdlog::warn("nxwarp: waitForFences failed");

	++frames_decoded;
	host.publish(accumulator, std::move(handle));

	// Where the time goes, once every two seconds per stream: the wait on the
	// previous frame, the wall time of this one, nxvc's own pass timings, and
	// the unit size. This is the number a live session on a headset is about.
	{
		const auto t_end = std::chrono::steady_clock::now();
		const auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
		nxvc_vkd_stats st{};
		nxvc_vk_decoder_stats(nxvc, &st);
		prof.n++;
		prof.wait_ms += ms(t_decode0 - t_wait0);
		prof.wall_ms += ms(t_end - t_decode0);
		prof.pass_a_ms += st.pass_a_ms;
		prof.pass_b_ms += st.pass_b_ms;
		prof.gpu_ms += st.gpu_ms;
		prof.bytes += job.unit.size();
		if (t_end - prof.since > std::chrono::seconds(2))
		{
			const double n = prof.n;
			spdlog::info("nxwarp[{}]: {} frames in {:.1f} s: {:.0f} B/frame, wait-prev {:.1f} ms, wall {:.1f} ms, nxvc passA {:.1f} passB {:.1f} gpu {:.1f} ms; holes {}, refused {}",
			             stream_index, prof.n, ms(t_end - prof.since) / 1000.0, prof.bytes / n,
			             prof.wait_ms / n, prof.wall_ms / n, prof.pass_a_ms / n, prof.pass_b_ms / n, prof.gpu_ms / n,
			             frames_dropped_holes, frames_dropped_codec);
			spdlog::info("nxwarp[{}]: {} frames dropped late (queue kept to {})",
			             stream_index, frames_dropped_late, kMaxQueuedFrames);
			prof = {};
			prof.since = t_end;
		}
	}
}

nxwarp_decoder::image * nxwarp_decoder::get_free()
{
	for (auto & item: image_pool)
	{
		if (item.free.exchange(false))
			return &item;
	}
	return nullptr;
}

std::vector<wivrn::video_codec> nxwarp_decoder::supported_codecs()
{
	return {video_codec::nxwarp};
}

} // namespace wivrn

#endif // WIVRN_USE_NXWARP
