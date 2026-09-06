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

#include <cctype>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <string>

// Shared by every NX Warp stream in the process (see the push site).
static std::atomic<uint32_t> g_decode_stride{1};
// Deepest selection this device is ever allowed to make. Eight was reachable and
// unrecoverable; four is already "show one frame in four", which is a picture nobody
// would keep, and past it the stride is not the answer -- the bitrate is.
static constexpr uint32_t kMaxDecodeStride = 4;
// When the stride last moved down, in steady_clock milliseconds. The decay is on a
// TIMER and it is driven from the network thread, because the thing it has to undo is a
// stride that stopped the worker producing samples: a decay that only runs when a frame
// is decoded cannot lower a stride that is the reason no frame is being decoded.
static std::atomic<int64_t> g_stride_decay_ms{0};

// --- are the two eyes sharing one queue, or running on it at the same time?
//
// Every NX Warp stream submits through the same host queue, so their copies' timestamps
// are in ONE device clock and can be compared directly. These two hold the device
// timestamp at which the last copy of any stream ended, and which stream it was; a
// stream whose own copy STARTS after the other eye's ended, by about its own decode
// time, is a stream that waited for the other eye. One that starts before it ended
// overlapped with it. That difference is the whole of hypothesis (2) and neither the
// host clock nor nxvc's own stats can see it.
// --- what every stream tells the server one frame costs.
//
// The pair, not the eye. Measured on a Pico 4 at stream_scale 0.7 with a queue per eye:
// both eyes decoded the same 90-91 frames per two seconds and nxvc's own GPU time was
// the same on both (5.5 vs 5.4 ms), but their WALL times were 10.1-11.2 and 15.3-16.3 ms
// -- because the GPU runs them strictly one after the other and eye 1 is always second
// (its copy began 6.4 ms after eye 0's ended, eye 0's began 15.6 ms after eye 1's). The
// server paces each stream on the number the stream reports, so it paced eye 0 to
// 79.7 fps and eye 1 to 54.9.
//
// That asymmetry is pure waste, not a difference in capability. scenes::stream composes
// only a frame EVERY decoder has produced ("Failed to find a common frame for all
// decoders"), so the 25 fps of extra eye-0 frames could never be shown; they cost wire,
// power and GPU to produce and were then discarded. The pair's cost is what decides
// whether a frame reaches the screen, so the pair's cost is what every stream reports.
//
// The maximum rather than the mean: a pair is ready when the SLOWER eye is ready.
static constexpr size_t kMaxStreams = 8;
static std::atomic<uint16_t> g_decode_us[kMaxStreams] = {};

// This stream's own figure in, the pair's out.
static uint16_t publish_decode_us(uint8_t stream_index, uint16_t own)
{
	if (stream_index < kMaxStreams)
		g_decode_us[stream_index].store(own, std::memory_order_relaxed);
	uint16_t worst = own;
	for (auto & v: g_decode_us)
		worst = std::max(worst, v.load(std::memory_order_relaxed));
	return worst;
}

static std::atomic<uint64_t> g_last_copy_end_ts{0};
static std::atomic<uint32_t> g_last_copy_stream{0xffffffffu};

// One step down, at most once per `period`, from whichever thread gets there first.
static void decay_decode_stride(std::chrono::milliseconds period)
{
	uint32_t cur = g_decode_stride.load(std::memory_order_relaxed);
	if (cur <= 1)
		return;
	const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
	                            std::chrono::steady_clock::now().time_since_epoch())
	                            .count();
	int64_t last = g_stride_decay_ms.load(std::memory_order_relaxed);
	if (last != 0 and now - last < period.count())
		return;
	if (not g_stride_decay_ms.compare_exchange_strong(last, now, std::memory_order_relaxed))
		return;
	g_decode_stride.compare_exchange_strong(cur, cur - 1, std::memory_order_relaxed);
}

// The process-wide decode stride, for the in-view statistics overlay. See the declaration in
// nxwarp_decoder.h: both eyes share one stride, so it is a file static rather than a member, and
// this is the only way out of this translation unit.
uint32_t wivrn::nxwarp_decode_stride()
{
	return g_decode_stride.load(std::memory_order_relaxed);
}

uint64_t wivrn::nxwarp_client_tools(const vk::PhysicalDeviceProperties & props)
{
#ifdef NXVC_VK_DECODER_TOOLS_FOR
	// nxvc derives the per-device mask itself, from the same table the decoder
	// uses, so a handshake answered before the decoder exists cannot disagree with
	// the decoder created later. One call, one rule, on nxvc's side of the line.
	//
	// The drift check in on_stream_header() stays. It costs nothing and it is now
	// pinning something slightly different but still worth pinning: that the mask
	// the handshake sent is the mask the decoder this build actually created will
	// accept.
	return nxvc_vk_decoder_tools_for(props.vendorID, props.deviceName);
#else
	// Fallback for an nxvc from before that export, which this branch still has to
	// build against during the rollout. It is the same rule written a second time,
	// which is the thing the export exists to remove; delete this arm once the
	// floor moves past it.
	//
	// The PER-DEVICE mask, which is what a handshake must send: nxvc's decoder clears
	// XFORM_LARGE (bit 27) on an Adreno, because it decodes 16x16 and 32x32 streams
	// wrong there and WEDGES on the 4:4:4 32x32 conformance vector -- a fence that never
	// signals, no kgsl fault, no GPU reset. A headset that advertised a tool it hangs on
	// would be inviting a conformant encoder to end the session.
	//
	// nxvc derives this in tools_supported_for(vendor_id, device_name)
	// (nx-warp vk/decoder/nxvc_vkdec_parse.cpp), which is NOT in the installed headers,
	// so the vendor test is repeated here. One rule in two places is a thing this
	// project does not accept on trust, so it is not left on trust:
	// nxwarp_decoder::on_stream_header() compares this against nxvc_vk_decoder_tools()
	// as soon as a decoder exists and logs an error if they differ. The permanent fix is
	// an nxvc entry point taking these two properties, and it belongs on that side.
	//
	// 0x5143 is Qualcomm. The name is checked too, case-insensitively, because a
	// Qualcomm part behind a translation layer reports someone else's vendor id.
	constexpr uint64_t nxvc_tool_xform_large = 1ull << 27;
	bool adreno = props.vendorID == 0x5143u;
	if (not adreno)
	{
		std::string name = props.deviceName;
		for (char & c: name)
			c = char(std::tolower((unsigned char)c));
		adreno = name.find("adreno") != std::string::npos;
	}
	const uint64_t all = nxvc_vk_decoder_tools_supported();
	return adreno ? (all & ~nxvc_tool_xform_large) : all;
#endif
}

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

// Kept for readability at the use site; the values themselves live in wivrn_packets.h so
// the two ends cannot drift. Neither is a path: 0xFF marks the codec's raw stream header
// and 0xFE a resync notice, both on the control socket.
constexpr uint8_t kStreamHeaderPath = wivrn::to_headset::nxwarp_stream_header_path;
constexpr uint8_t kResyncPath = wivrn::to_headset::nxwarp_resync_path;
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
	// Timestamps around the copy. Two conditions, and both are real on some device:
	// the physical device must report a non-zero period, and the QUEUE FAMILY must
	// have timestamp bits -- a transfer-only family often has none.
	{
		const auto props = physical_device.getProperties();
		const auto families = physical_device.getQueueFamilyProperties();
		const bool family_ok = vk_queue_family_index < families.size() and
		                       families[vk_queue_family_index].timestampValidBits > 0;
		if (props.limits.timestampPeriod > 0 and family_ok)
		{
			ts_pool = vk::raii::QueryPool(device, vk::QueryPoolCreateInfo{
			                                              .queryType = vk::QueryType::eTimestamp,
			                                              .queryCount = 2,
			                                      });
			ts_period_ns = props.limits.timestampPeriod;
			have_ts = true;
		}
	}

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
	             stream_index, frames_decoded.load(), frames_dropped_holes.load(), frames_dropped_codec.load(),
	             frames_no_view_info.load());
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
	host.with_queue(stream_index, [&](vk::Queue q) { ci.queue = q; });
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

	// The handshake's tool mask was computed BEFORE this decoder existed, from the
	// physical device's properties alone (wivrn::decoder::nxvc_tools), because the server
	// needs it before it encodes anything. That repeats nxvc's own vendor test in a
	// second place, so this is where the two are compared: now there IS a decoder, and it
	// knows the answer for certain. A difference means the copy in decoder.cpp has
	// drifted from tools_supported_for(), and the server has been told the wrong thing.
	{
		const uint64_t advertised = nxwarp_client_tools(physical_device.getProperties());
		const uint64_t actual = nxvc_vk_decoder_tools(nxvc);
		if (advertised != actual)
			spdlog::error("nxwarp[{}]: the tool mask sent at the handshake ({:#x}) is not "
			              "the one this decoder accepts ({:#x}) -- wivrn::decoder::nxvc_tools "
			              "has drifted from nxvc's tools_supported_for()",
			              stream_index, advertised, actual);
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

	// Published for the in-view statistics overlay; nothing else reads these.
	hdr_width.store(si.width, std::memory_order_relaxed);
	hdr_height.store(si.height, std::memory_order_relaxed);
	hdr_tools.store(si.tools, std::memory_order_relaxed);

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
	if (dg.path_id == kResyncPath)
	{
		// "The frame with this id needs no reference." The encoder sends one after
		// every all-zero receipt map, which is what our own report_frame_not_held
		// asked for. Until that frame is decoded, everything this decoder produces
		// is warped from a reference it does not have, and it must not be shown.
		if (dg.payload.size() >= 2)
		{
			const uint16_t id = uint16_t(dg.payload[0] | (dg.payload[1] << 8));
			std::lock_guard lock(resync_mutex);
			resync_ids.push_back(id);
			while (resync_ids.size() > kMaxPendingResync)
				resync_ids.pop_front();
		}
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
	// The server's own timestamps, on the frame's LAST datagram. Copied straight
	// into the feedback this frame will return, so the round trip is the one every
	// other codec already uses (wivrn_session::operator()(from_headset::feedback))
	// and the dashboard needs to know nothing about nxwarp.
	if (dg.timing_info)
	{
		f->fb.encode_begin = dg.timing_info->encode_begin;
		f->fb.encode_end = dg.timing_info->encode_end;
		f->fb.send_begin = dg.timing_info->send_begin;
		f->fb.send_end = dg.timing_info->send_end;
	}
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
	// Numbered from the SENDER's id, here, at the frame's first datagram: the number
	// has to exist before the frame is known to survive, because a frame that does not
	// survive is reported too (see close_frame) and the server joins that report to the
	// other eye's by this very number. See wire_frame_index().
	free_slot->fb.frame_index = wire_frame_index(frame_id);
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

// The worker has reconstructed `frame_id`: fold it into the window the network
// thread puts on every feedback packet.
//
// Lock free because the two threads meet here every frame and the pair has to move
// together -- a base from one update with a mask from another names frames that were
// never built, and the encoder would reference one of them.  A compare-exchange loop
// over the packed word is the whole of it; contention is one writer and one reader, so
// it succeeds first time in practice.
void nxwarp_decoder::note_frame_held(uint16_t frame_id)
{
	uint64_t cur = held_ack.load(std::memory_order_relaxed);
	for (;;)
	{
		const uint16_t base = uint16_t(cur & kAckBaseMask);
		const uint32_t mask = uint32_t(cur >> 16);
		uint16_t nbase = base;
		uint32_t nmask = mask;
		if (mask == 0)
		{
			// Nothing yet: this frame becomes the window.
			nbase = frame_id;
			nmask = 1u;
		}
		else if (seq_lt(base, frame_id))
		{
			// Newer than anything recorded: shift the window forward. A shift of
			// 32 or more is undefined in C++ and would also be meaningless -- the
			// whole window is older than ref_sel can reach -- so it becomes an
			// empty history with this frame in it.
			const uint16_t d = uint16_t(frame_id - base);
			nmask = (d >= 32 ? 0u : (mask << d)) | 1u;
			nbase = frame_id;
		}
		else
		{
			// Older, which a frame decoded out of order can be. Only the window
			// can hold it; anything past 32 back is beyond any reference.
			const uint16_t d = uint16_t(base - frame_id);
			if (d >= 32)
				return;
			nmask = mask | (1u << d);
		}
		const uint64_t next = uint64_t(nbase) | (uint64_t(nmask) << 16);
		if (next == cur)
			return;
		if (held_ack.compare_exchange_weak(cur, next, std::memory_order_relaxed))
			return;
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
		uint16_t ack_base = 0;
		uint32_t ack_mask = 0;
		read_held_ack(ack_base, ack_mask);
		host.send_feedback(stream_index, f.path_id, std::move(packet),
		                   publish_decode_us(stream_index,
		                                     decode_us_report.load(std::memory_order_relaxed)),
		                   ack_base, ack_mask);
	}
}

// The stream's 16-bit frame id, widened to the 64 bits from_headset::feedback carries.
//
// Only the wrap is interesting. Ids are sequential modulo 2^16 and this decoder already
// compares them with seq_lt, so "the sequence moved forward and the number went down" is
// exactly a wrap. An id that is *behind* the newest but numerically above it is the tail
// of the previous epoch -- a straggler that arrived after the wrap -- and belongs one
// epoch back. The epoch starts at 1 so that subtraction can never underflow; the result
// is 0-based.
//
// This is monotonic in the sender's frame order, not in arrival order, which is the whole
// point: two eyes that drop different frames still agree on the number of every frame
// they both saw, and on the number of every frame either of them lost.
uint64_t nxwarp_decoder::wire_frame_index(uint16_t frame_id)
{
	if (not wire_seeded)
	{
		wire_seeded = true;
		wire_newest = frame_id;
	}
	else if (seq_lt(wire_newest, frame_id))
	{
		if (frame_id < wire_newest)
			++wire_epoch;
		wire_newest = frame_id;
	}

	const uint64_t epoch = (seq_lt(frame_id, wire_newest) and frame_id > wire_newest)
	                               ? wire_epoch - 1
	                               : wire_epoch;
	return (epoch << 16 | frame_id) - 0x10000ull;
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
		// net_frames is monotonic (the GUI differences it); the line below wants the
		// count for this window, so it keeps its own mark of where the window began.
		const uint64_t closed = net_frames.fetch_add(1, std::memory_order_relaxed) + 1;
		if (unit.empty())
		{
			net_holes++;
			last_hole = nxwarp_wire::last_report();
		}
		// A frame that arrived whole only because the window held it open after a newer
		// frame had already started. This is the count that says what the window is
		// worth: every one of these was a hole before it existed.
		if (not unit.empty() and f.reordered)
			net_late_completed++;
		const auto now = std::chrono::steady_clock::now();
		if (now - net_since > std::chrono::seconds(2))
		{
			spdlog::info("nxwarp[{}] net: {} frames closed in {:.1f} s, {} with a hole, {} out-of-order datagrams, {} frames completed late, {} queued for the worker, {} decoded so far, {} stragglers dropped; last hole {}/{} chunks, first missing {}, short {}, chunk {} B",
			             stream_index, closed - net_frames_mark, std::chrono::duration<double>(now - net_since).count(), net_holes,
			             net_out_of_order, net_late_completed, jobs_pending.load(), frames_decoded.load(), stragglers_dropped,
			             last_hole.present, last_hole.expected, last_hole.first_missing == UINT32_MAX ? -1 : int(last_hole.first_missing), last_hole.short_chunk, chunk);
			// Printed on the NETWORK thread on purpose: these are the numbers that
			// explain a worker that has stopped reporting, so they must come from a
			// thread that is still running.
			spdlog::info("nxwarp[{}] sel: stride {}, arrival {:.1f} ms, dropped-late {}, withheld {}, decoded {}",
			             stream_index, g_decode_stride.load(),
			             arrival_period_ms.load(std::memory_order_relaxed),
			             frames_dropped_late.load(), frames_withheld.load(std::memory_order_relaxed),
			             frames_decoded.load());
			if (receiver)
			{
				const auto & rs = receiver->stats;
				// Placed and late as deltas over this window, because "late" is the
				// number that goes wrong quietly: a tile placed after its band's
				// deadline is a tile that arrived and that the encoder is told did
				// not. On a healthy link it is zero. Everything else stays a running
				// total, which is what it is useful as.
				spdlog::info("nxwarp[{}] rx: {} datagrams, placed {} (+{}), late {} (+{}), stale_frame {}, bad_range {}, bad_dir {}, bad_caps {}, bad_ver {}, auth_fail {}, replay {}, frozen {}, dup {}",
				             stream_index, rs.datagrams,
				             rs.tiles_placed, rs.tiles_placed - net_tiles_placed_at,
				             rs.tiles_late, rs.tiles_late - net_tiles_late_at,
				             rs.stale_frame, rs.bad_range, rs.bad_directory, rs.bad_caps, rs.bad_version, rs.auth_fail, rs.replay, rs.frozen_band, rs.duplicates);
				net_tiles_placed_at = rs.tiles_placed;
				net_tiles_late_at = rs.tiles_late;
			}
			net_frames_mark = closed;
			net_holes = 0;
			net_out_of_order = 0;
			net_late_completed = 0;
			net_since = now;
		}
	}
	if (unit.empty())
	{
		// A hole, a short chunk in the middle, or fewer bytes than the length prefix
		// declares. The band feedback has already gone out, which is how the encoder
		// learns to refresh what this headset never got.
		//
		// That is the CODEC's half of the loss report and it always was. The other
		// half is the server's: from_headset::feedback with no sent_to_decoder is how
		// every other decoder in this client says "this frame never arrived", and it
		// is what the automatic bitrate counts as a lost frame and what the IDR
		// handler watches. Returning here without sending one -- which is what this
		// did -- makes NX Warp loss invisible to the bitrate controller: it sees a
		// link on which nothing is ever lost, however much is, and the one signal
		// that should make it back off never reaches it.
		//
		// received_first_packet is a real arrival (the frame's first datagram);
		// sent_to_decoder stays zero, which is the "never completed" marker.
		++frames_dropped_holes;
		host.report_frame_lost(f.fb);
		// Both signals, and they are saying different things: the one above tells the
		// bitrate controller the LINK lost something, this one tells the encoder this
		// frame is not in the reference ring. A hole is the one case that is genuinely
		// both.
		host.report_frame_not_held(stream_index, f.frame_id, from_headset::nxwarp_frame_not_held::reason::hole);
		return;
	}

	host.on_frame_unit(f.frame_id, unit);

	// One step off the stride every two seconds, on the thread that is still running
	// when the worker is not. See decay_decode_stride(): the stride's whole failure
	// mode was that it could only be lowered by the code path it had switched off.
	decay_decode_stride(std::chrono::seconds(2));

	// The interval between arriving frames, which is what the decode stride is
	// measured against. Counted over every frame that arrives whole, including the
	// ones the stride is about to discard: it is the SERVER's send rate, and the
	// stride's own decisions must not feed back into it.
	{
		const auto now = std::chrono::steady_clock::now();
		if (arrival_seeded)
		{
			const double dt = std::chrono::duration<double, std::milli>(now - arrival_last).count();
			const double prev = double(arrival_period_ms.load(std::memory_order_relaxed));
			arrival_period_ms.store(float(prev > 0 ? prev + 0.2 * (dt - prev) : dt),
			                        std::memory_order_relaxed);
		}
		arrival_last = now;
		arrival_seeded = true;
	}

	decode_job job;
	job.frame_id = f.frame_id;
	job.unit = std::move(unit);
	job.fb = f.fb;
	job.view_info = f.view_info;
	job.have_view_info = f.have_view_info;
	// The network delivers at the server's rate; the worker decodes at whatever
	// rate this device manages. When the device cannot keep up, a queued frame is
	// nothing but latency the user will wear -- 90 fps arriving against a 57 ms
	// decode grew the queue past 2000 frames on an Adreno 650, i.e. minutes of
	// lag. Keep only the newest: on a live stream late is worse than missing.
	//
	// Dropping a frame is sound on an all-intra stream, where every frame stands
	// alone. On an inter stream it is not sound by itself: the encoder's receipt map
	// is built from the TRANSPORT's report, which was sent when the datagrams landed
	// and truthfully said the tiles arrived, and every drop below happens after that.
	// The encoder would go on coding the next frame as a warp of a picture this
	// device never reconstructed (SYNTAX 6.11, INTEGRATION-DECISIONS 6), which on a
	// headset looks like a few blocks of real image and the rest grey.
	//
	// So every drop below reports itself with report_frame_not_held(), and the server
	// answers with an all-zero receipt map, which nxvc documents as the way to say
	// "the client holds nothing" and which makes the next frame all-INTRA. That is
	// what makes the drop sound again, and it is why these calls are not optional.
	//
	// And the drop must be the SAME decision on every stream: the render thread
	// composes only a frame that every decoder has produced ("Failed to find a
	// common frame for all decoders"), so two eyes each keeping "their newest"
	// drift onto different frames and nothing is ever shown. Hence a shared
	// stride: every stream decodes frames whose id is a multiple of the stride,
	// and the stride is the worst measured decode time over the frame period,
	// so the selected frames are exactly the ones the slowest decoder can keep
	// up with, and they are the same frames on both eyes.
	const uint32_t stride = g_decode_stride.load();
	if (stride > 1 and (job.frame_id % stride) != 0)
	{
		++frames_dropped_late;
		host.report_frame_not_held(stream_index, job.frame_id, from_headset::nxwarp_frame_not_held::reason::stride);
		return;
	}
	if (jobs_pending.load() >= kMaxQueuedFrames)
	{
		// The queued frames are about to be discarded, and each of them is a frame
		// the encoder currently believes this headset holds. Their ids are only
		// knowable here, while they are still in the queue, so the predicate reads
		// them on the way past rather than the count being incremented blindly.
		std::vector<uint16_t> discarded;
		jobs.drop_until([&discarded](const decode_job & j) {
			discarded.push_back(j.frame_id);
			return false;
		});
		frames_dropped_late += jobs_pending.exchange(0);
		for (uint16_t id: discarded)
			host.report_frame_not_held(stream_index, id, from_headset::nxwarp_frame_not_held::reason::backlog);
	}
	jobs_pending++;
	jobs.push(std::move(job));
}

void nxwarp_decoder::decode_unit(decode_job & job)
{
	// The frame is handed to the decoder HERE -- off the bounded queue and onto the
	// worker -- so this is what `sent_to_decoder` means. Stamping it at publish, as
	// this did, made the reported decode interval zero and moved its whole cost into
	// the queue segment ahead of it: the one number the dashboard had for nxwarp's
	// decode was the one number it could not be.
	job.fb.sent_to_decoder = host.now();
	const auto t_iter0 = std::chrono::steady_clock::now();
	size_t consumed = 0;
	VkSemaphore dec_sem = VK_NULL_HANDLE;
	uint64_t dec_val = 0;
	bool refused = false;
	// This driver advertises VK_KHR_timeline_semaphore and then refuses to create one,
	// so nxvc_vk_decoder_timeline() is null on the headset and there used to be no
	// object at all for the queue to wait on: the copy below had to be fenced against
	// the decode on the HOST, one full round trip in the middle of every frame.
	// NXVC_VKD_SUBMIT_SIGNAL_BINARY gives us a binary semaphore instead, which the
	// copy waits on where it would have waited on the timeline.
	//
	// A binary semaphore is single-use: signalled once, waited once. Every path that
	// leaves this function after a successful decode must therefore consume it, or the
	// next frame's decode submit blocks forever on a semaphore that is already
	// signalled. drain_binsem() below is that obligation, and the early returns call it.
	VkSemaphore bin_sem = VK_NULL_HANDLE;
	bool bin_pending = false;
	const bool use_bin_sem = nxvc_vk_decoder_timeline(nxvc) == VK_NULL_HANDLE and
	                         nxvc_vk_decoder_binary_semaphore(nxvc) != VK_NULL_HANDLE;
	const auto drain_bin_sem = [&]() {
		if (not bin_pending)
			return;
		bin_pending = false;
		// An empty submit whose only job is to consume the signal.
		host.with_queue(stream_index, [&](vk::Queue queue) {
			const vk::PipelineStageFlags stage = vk::PipelineStageFlagBits::eTopOfPipe;
			const vk::Semaphore sem{bin_sem};
			queue.submit(vk::SubmitInfo{
			        .waitSemaphoreCount = 1,
			        .pWaitSemaphores = &sem,
			        .pWaitDstStageMask = &stage,
			});
		});
	};
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
	host.with_queue(stream_index, [&](vk::Queue) {
		const uint32_t submit_flags =
		        NXVC_VKD_SUBMIT_ASYNC |
		        (use_bin_sem ? uint32_t(NXVC_VKD_SUBMIT_SIGNAL_BINARY) : 0u);
		auto st = nxvc_vk_decode_frame_ex(nxvc, job.unit.data(), job.unit.size(),
		                                  submit_flags, &consumed);
		if (st != NXVC_VKD_OK)
		{
			spdlog::warn("nxwarp[{}]: frame {} refused: {} ({})", stream_index, job.frame_id,
			             nxvc_vk_decoder_status_string(st), nxvc_vk_decoder_last_error(nxvc));
			++frames_dropped_codec;
			refused = true;
			return;
		}
		// The codec took the unit, so its reference ring slot is being filled by a
		// submit every later decode is ordered behind -- and that is the whole of
		// what the encoder needs to know before it may predict from this frame.
		// Confirmed HERE, not after the readback and the fence below: those are
		// about handing the picture to the compositor, and on a slow device they
		// are most of the wall time. Confirming after them would put the encoder's
		// answer a whole frame further behind, and the reference range it has to
		// fit inside is three.
		//
		// A frame that is decoded and then WITHHELD is confirmed too. Withholding
		// is about what the user is shown; the picture is in the ring either way,
		// and refusing to confirm it would cost an intra frame for nothing.
		note_frame_held(job.frame_id);
		dec_sem = nxvc_vk_decoder_timeline(nxvc);
		dec_val = nxvc_vk_decoder_timeline_value(nxvc);
		if (use_bin_sem)
		{
			bin_sem = nxvc_vk_decoder_binary_semaphore(nxvc);
			bin_pending = bin_sem != VK_NULL_HANDLE;
		}
	});
	if (refused)
	{
		host.report_frame_not_held(stream_index, job.frame_id, from_headset::nxwarp_frame_not_held::reason::refused);
		return;
	}
	// A slow device, on purpose (see sim_decode_ms). Inside the measured interval, so
	// everything downstream -- the stride, the queue bound, the decode figure the
	// server paces to -- sees it as the decode cost it is pretending to be.
	if (sim_decode_ms > 0)
		std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(sim_decode_ms));
	const auto t_submitted = std::chrono::steady_clock::now();

	nxvc_vkd_images img{};
	if (nxvc_vk_decoder_images(nxvc, &img) != NXVC_VKD_OK or img.count < 2)
	{
		// The decode was submitted, so the ring may well have advanced -- but a codec
		// that will not hand back its images is one whose state cannot be reasoned
		// about, and the two ways to be wrong here are not symmetric: an unnecessary
		// resynchronisation costs one intra frame, and a missed one corrupts every
		// frame until something else resets the stream.
		++frames_dropped_codec;
		host.report_frame_not_held(stream_index, job.frame_id, from_headset::nxwarp_frame_not_held::reason::refused);
		drain_bin_sem();
		return;
	}

	auto item = get_free();
	const auto t_got_free = std::chrono::steady_clock::now();
	if (not item)
	{
		// Deliberately NOT reported as not held. The codec decoded this frame and its
		// reference ring advanced; what failed is the copy out into the pool the
		// render thread picks from. The frame is not displayed and it IS in the ring,
		// so the encoder may still predict from it -- saying otherwise would cost an
		// intra refresh for a picture the decoder actually has. "Not held" is about
		// the reference ring, not about what reached the screen.
		spdlog::warn("nxwarp[{}]: no image available in pool, discard frame", stream_index);
		drain_bin_sem();
		return;
	}

	if (device.waitForFences(*fence, true, UINT64_MAX) != vk::Result::eSuccess)
		spdlog::warn("nxwarp: waitForFences failed");
	const auto t_fence_pre = std::chrono::steady_clock::now();

	cmd.reset();
	cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
	// Query 0 is written at the top of the pipe, which is AFTER the submit's semaphore
	// wait has been satisfied: it is the moment the copy actually starts on the device,
	// not the moment it was submitted. That is the whole point of measuring here.
	if (have_ts)
	{
		cmd.resetQueryPool(*ts_pool, 0, 2);
		cmd.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, *ts_pool, 0);
	}

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
	if (have_ts)
		cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *ts_pool, 1);
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
		frames_no_view_info.fetch_add(1, std::memory_order_relaxed);
		if (not warned_view_info)
		{
			warned_view_info = true;
			spdlog::warn("nxwarp[{}]: frame {} decoded with no view_info on its first datagram "
			             "(lost, and its tiles recovered by FEC); published with a default pose "
			             "and foveation (see docs/nxwarp.md)",
			             stream_index, job.frame_id);
		}
	}

	// `sent_to_decoder` is NOT stamped here. It used to be, and here is after the
	// decode, so the dashboard read nxwarp's decode as taking zero time and charged
	// the whole of it to the queue segment before it. It is stamped at the top of
	// decode_unit() instead, which is where the frame is actually handed over.
	//
	// `received_from_decoder` is the instant this frame's GPU work was known finished
	// -- the fence-post below -- and it is stamped there rather than here.
	// push_blit_handle() no longer overwrites a value the decoder already set, so what
	// the dashboard shows as "decode" is the decode and not the walk to the ring.
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

	// What the copy waits on, in order of preference: the timeline where the driver
	// gives one, the binary semaphore where it does not, and the host only where
	// neither exists. The host wait is the expensive one and is now the last resort
	// rather than the headset's normal path.
	VkSemaphore wait_sem = dec_sem != VK_NULL_HANDLE ? dec_sem : bin_sem;
	if (wait_sem == VK_NULL_HANDLE)
		nxvc_vk_decoder_wait(nxvc, UINT64_MAX);
	const bool signal_on_queue = *item->semaphore != VK_NULL_HANDLE;

	const auto t_recorded = std::chrono::steady_clock::now();
	device.resetFences(*fence);
	host.with_queue(stream_index, [&](vk::Queue queue) {
		const vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eTransfer;
		const uint64_t signal_val = ++item->semaphore_val;
		std::array<vk::Semaphore, 1> wait{wait_sem};
		std::array<vk::Semaphore, 1> signal{*item->semaphore};
		const uint32_t n_wait = wait_sem != VK_NULL_HANDLE ? 1 : 0;
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
	// The submit above is the one wait the binary semaphore gets. dec_val is passed
	// alongside it and is ignored: a binary semaphore has no value.
	bin_pending = false;
	const auto t_qsubmit = std::chrono::steady_clock::now();
	// No semaphore to hand the render thread: make the copy complete before it can see
	// the frame. One frame of pipelining lost, on the driver that gives no other choice.
	if (not signal_on_queue and device.waitForFences(*fence, true, UINT64_MAX) != vk::Result::eSuccess)
		spdlog::warn("nxwarp: waitForFences failed");
	const auto t_fence_post = std::chrono::steady_clock::now();
	// The decode is done: everything the codec was asked for has been signalled. This
	// is what `received_from_decoder` means for every other codec in this client, so
	// it is what it means here. See the note where sent_to_decoder is stamped.
	//
	// Stamped on the HANDLE and not on `job.fb`: the handle took its copy of the
	// feedback before the submit, so writing the job's copy here would leave the one
	// that travels at zero -- which reads downstream as a decode that finished before
	// it started.
	handle->feedback.received_from_decoder = host.now();
	// The copy's own device time. Only readable on the path that just waited on the
	// fence; where the render thread is given a semaphore instead, the copy may still
	// be running and the queries are not ready.
	double copy_gpu_ms = -1;
	double after_other_ms = 0;
	bool overlapped_other = false, saw_other = false;
	if (have_ts and not signal_on_queue)
	{
		std::array<uint64_t, 2> ts{};
		auto r = (*device).getQueryPoolResults(*ts_pool, 0, 2, sizeof(ts), ts.data(),
		                                       sizeof(uint64_t), vk::QueryResultFlagBits::e64,
		                                       *device.getDispatcher());
		if ((r == vk::Result::eSuccess or r == vk::Result::eNotReady) and ts[1] > ts[0])
		{
			copy_gpu_ms = double(ts[1] - ts[0]) * ts_period_ns / 1e6;
			const uint64_t prev_end = g_last_copy_end_ts.load(std::memory_order_relaxed);
			const uint32_t prev_stream = g_last_copy_stream.load(std::memory_order_relaxed);
			if (prev_end and prev_stream != 0xffffffffu and prev_stream != stream_index)
			{
				saw_other = true;
				if (ts[0] >= prev_end)
					after_other_ms = double(ts[0] - prev_end) * ts_period_ns / 1e6;
				else
					overlapped_other = true;
			}
			g_last_copy_end_ts.store(ts[1], std::memory_order_relaxed);
			g_last_copy_stream.store(stream_index, std::memory_order_relaxed);
		}
	}

	++frames_decoded;

	// May this frame be shown?
	//
	// It decoded either way -- the codec's ring has to keep moving, and the encoder is
	// about to reset it anyway -- but a frame warped from a reference this decoder
	// never built is not a picture, it is a few tiles of image over a field of grey,
	// and putting that on a headset is worse than showing the previous frame for
	// another 11 ms. So such a frame is decoded and withheld.
	//
	// Two ways to be safe, and they are both decided here, on the worker, from what
	// the worker itself knows:
	//
	//   * the frame continues the run: the previous frame id went through this codec,
	//     so the reference it warps from is in the ring;
	//   * or the encoder has said this frame needs no reference -- the resync notice
	//     it sends after every all-zero receipt map, which is the answer to the
	//     not-held report the drop sites above send.
	//
	// The first frame of a stream is safe by construction: the encoder has no
	// reference to predict from either, so it codes it intra.
	//
	// Sequence arithmetic throughout, so the 16-bit wrap costs nothing.
	const bool contiguous =
	        have_last_decoded and uint16_t(job.frame_id - last_decoded_id) == 1;
	bool at_resync = false;
	{
		std::lock_guard lock(resync_mutex);
		auto it = std::find(resync_ids.begin(), resync_ids.end(), job.frame_id);
		if (it != resync_ids.end())
		{
			at_resync = true;
			// Consume this one and everything older: a notice the encoder sent
			// before it has been overtaken by the frame in hand.
			resync_ids.erase(resync_ids.begin(), it + 1);
		}
	}
	const bool showable = not have_last_decoded or contiguous or at_resync;

	// The ring advanced for this frame whether or not it is shown, so it is the
	// reference the next one is measured against, and it is part of the list a
	// reference decoder has to be given to stay in step with this one.
	last_decoded_id = job.frame_id;
	have_last_decoded = true;
	host.on_frame_decoded(job.frame_id);

	if (not showable)
		frames_withheld.fetch_add(1, std::memory_order_relaxed);
	else
		host.publish(accumulator, std::move(handle));
	const auto t_published = std::chrono::steady_clock::now();

	// Where the time goes, once every two seconds per stream: the wait on the
	// previous frame, the wall time of this one, nxvc's own pass timings, and
	// the unit size. This is the number a live session on a headset is about.
	//
	// Accounted for EVERY frame the worker decoded, shown or withheld. It used to be
	// accounted only for published ones, and that is not a reporting detail: the
	// decode stride is derived here, so a stride that made frames non-contiguous --
	// which is what a stride greater than one does -- withheld every frame, which
	// stopped this block running, which stopped the stride ever decaying again.
	{
		const auto t_end = t_published;
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
		prof.gap_ms += have_last_iter ? ms(t_iter0 - last_iter_end) : 0.0;
		prof.sub_ms += ms(t_submitted - t_decode0);
		prof.free_ms += ms(t_got_free - t_submitted);
		prof.fpre_ms += ms(t_fence_pre - t_got_free);
		prof.rec_ms += ms(t_recorded - t_fence_pre);
		prof.qsub_ms += ms(t_qsubmit - t_recorded);
		prof.fpost_ms += ms(t_fence_post - t_qsubmit);
		prof.pub_ms += ms(t_published - t_fence_post);
		prof.withheld += showable ? 0 : 1;
		prof.wall_max_ms = std::max(prof.wall_max_ms, ms(t_end - t_decode0));
		if (copy_gpu_ms >= 0)
		{
			prof.ts_n++;
			prof.copy_gpu_ms += copy_gpu_ms;
			// What the host fence waited for that was neither this copy's GPU
			// time nor nxvc's own: the queue in front of both.
			prof.sched_ms += ms(t_fence_post - t_qsubmit) - copy_gpu_ms - st.gpu_ms;
			if (saw_other)
			{
				prof.after_other_n++;
				prof.after_other_ms += after_other_ms;
				prof.overlapped_n += overlapped_other ? 1 : 0;
			}
		}
		last_iter_end = t_end;
		have_last_iter = true;

		// What this frame cost, published for the server. The same wall time the
		// stride below is derived from, smoothed with a fifth-weight EWMA so a
		// single slow frame moves it a little and a sustained change moves it
		// within a handful of frames. Saturated at the 16-bit field's range.
		{
			double wall_us = ms(t_end - t_decode0) * 1000.0;
			const double prev = double(decode_us_report.load(std::memory_order_relaxed));
			// A sample four times the running figure is not this decoder getting
			// four times slower, it is something else having taken the GPU: a
			// 1.4-second "decode" was measured on this device with the compositor
			// holding the queue. Clip it rather than drop it -- the server paces to
			// this number, and a genuine sustained slowdown still walks the EWMA up
			// there in a handful of frames, while one stall moves it by a fifth of
			// three times itself and no further.
			if (prev > 0 and wall_us > 4 * prev)
			{
				wall_us = 4 * prev;
				prof.stalls++;
			}
			const double next = prev > 0 ? prev + 0.2 * (wall_us - prev) : wall_us;
			decode_us_report.store(uint16_t(std::clamp(next, 0.0, 65535.0)),
			                       std::memory_order_relaxed);
		}
		if (t_end - prof.since > std::chrono::seconds(2))
		{
			const double n = prof.n;
			spdlog::info("nxwarp[{}]: {} frames in {:.1f} s: {:.0f} B/frame, wait-prev {:.1f} ms, wall {:.1f} ms, nxvc passA {:.1f} passB {:.1f} gpu {:.1f} ms; holes {}, refused {}",
			             stream_index, prof.n, ms(t_end - prof.since) / 1000.0, prof.bytes / n,
			             prof.wait_ms / n, prof.wall_ms / n, prof.pass_a_ms / n, prof.pass_b_ms / n, prof.gpu_ms / n,
			             frames_dropped_holes.load(), frames_dropped_codec.load());
			spdlog::info("nxwarp[{}]: {} frames dropped late (queue kept to {})",
			             stream_index, frames_dropped_late.load(), kMaxQueuedFrames);
			spdlog::info("nxwarp[{}] stage: gap {:.1f} | wait-prev {:.1f} | submit {:.1f} | get_free {:.1f} | fence-pre {:.1f} | record {:.1f} | qsubmit {:.1f} | fence-post {:.1f} | publish {:.1f} ms; withheld {}, stride {}, arrival {:.1f} ms",
			             stream_index, prof.gap_ms / n, prof.wait_ms / n, prof.sub_ms / n,
			             prof.free_ms / n, prof.fpre_ms / n, prof.rec_ms / n, prof.qsub_ms / n,
			             prof.fpost_ms / n, prof.pub_ms / n, prof.withheld,
			             g_decode_stride.load(), arrival_period_ms.load(std::memory_order_relaxed));
			if (prof.ts_n)
				spdlog::info("nxwarp[{}] fence-post {:.1f} ms = nxvc gpu {:.1f} + copy gpu {:.2f} + queue {:.1f} (over {} frames with timestamps)",
				             stream_index, (prof.fpost_ms + prof.qsub_ms) / n, prof.gpu_ms / n,
				             prof.copy_gpu_ms / double(prof.ts_n), prof.sched_ms / double(prof.ts_n),
				             prof.ts_n);
			if (prof.after_other_n)
				spdlog::info("nxwarp[{}] eyes: this copy began {:.1f} ms after the other eye's copy ended, over {} frames; {} overlapped it",
				             stream_index, prof.after_other_ms / double(prof.after_other_n),
				             prof.after_other_n, prof.overlapped_n);
			if (prof.stalls)
				spdlog::info("nxwarp[{}]: {} of {} frames measured a stall not attributable to decoding (worst wall {:.1f} ms); clipped out of the reported decode cost and out of the stride",
				             stream_index, prof.stalls, prof.n, prof.wall_max_ms);

			// The same window, republished for stats(): the GUI shows these under the
			// latency figure instead of anybody reading the lines above out of the log.
			prof_wall_ms.store(float(prof.wall_ms / n), std::memory_order_relaxed);
			prof_gpu_ms.store(float(prof.gpu_ms / n), std::memory_order_relaxed);
			prof_bytes.store(float(prof.bytes / n), std::memory_order_relaxed);
			// The pass split, for the in-view overlay: pass B is the half that scales
			// with the pixel count, so it is the one the stream scale moves.
			prof_pass_a_ms.store(float(prof.pass_a_ms / n), std::memory_order_relaxed);
			prof_pass_b_ms.store(float(prof.pass_b_ms / n), std::memory_order_relaxed);
			// Feed the shared stride from this stream's measured cost: ceil(wall / period).
			// Streams only ever raise it quickly and lower it by one step per report, so
			// a momentary hiccup does not flip the selection back and forth.
			//
			// The period is the measured interval between ARRIVING frames, not the 90 Hz
			// the server composites at: the stride exists to drop the frames this device
			// cannot keep up with, and "keep up" is relative to the rate they turn up at.
			// A server pacing its sends to what this decoder reported it can manage
			// (from_headset::nxwarp_feedback::decode_us) would otherwise still have had
			// two frames in three thrown away here. Falls back to the frame period the
			// stream description gave when nothing has arrived yet.
			//
			// THREE THINGS THIS GETS RIGHT THAT THE CEILING DID NOT
			//
			// 1. The sample it is measured on excludes the window's single worst
			//    frame. A stall that is not this decoder's fault -- the compositor
			//    taking the GPU, a fence that came back a second late -- is one
			//    sample, and under a mean it set the stride for good. Anything
			//    sustained survives the trim by definition.
			// 2. It raises on evidence that stride N-1 is not enough, with a margin,
			//    rather than on ceil() of a ratio that sits exactly at 1.0. Measured
			//    here: wall 25 ms against a 25-33 ms arrival period, so ceil() flipped
			//    to 2 on ordinary jitter -- and a stride of 2 makes every decoded
			//    frame non-contiguous, which withheld every frame, which stopped this
			//    block running at all. That is how one jittery sample became a
			//    permanent 4.
			// 3. The measure is against the ARRIVAL period, so it can never ask for
			//    more than the rate frames actually turn up at justifies: once the
			//    server paces to what this device reports, the period grows to meet
			//    the decode cost and the stride is 1 by construction.
			//
			// The decay is not here at all; see decay_decode_stride().
			if (prof.n >= 4)
			{
				const double arrival = double(arrival_period_ms.load(std::memory_order_relaxed));
				const double period_ms = arrival > 0.1 ? arrival : double(frame_period_us) / 1000.0;
				// Mean of the window with its worst frame removed.
				const double trimmed = (prof.wall_ms - prof.wall_max_ms) / (n - 1);
				constexpr double kRaiseMargin = 1.15;
				uint32_t want = 1;
				while (want < kMaxDecodeStride and trimmed > double(want) * period_ms * kRaiseMargin)
					++want;
				uint32_t cur = g_decode_stride.load();
				// Raising only. A stride that is too high is undone by the timer.
				while (want > cur and not g_decode_stride.compare_exchange_weak(cur, want))
					;
			}
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
