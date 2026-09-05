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

#include "video_encoder_nxwarp.h"
#include <chrono>

#include "nxwarp_packetize.h"

#include "encoder/encoder_settings.h"
#include "os/os_time.h"
#include "util/u_logging.h"
#include "utils/wivrn_vk_bundle.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <optional>
#include <stdexcept>

namespace
{
class dummy_idr_handler : public wivrn::idr_handler
{
public:
	void on_feedback(const wivrn::from_headset::feedback &) override {};
	void reset() override {};
	// INTEGRATION.md 1.5: the IDR ladder is retired for this codec. Recovery is
	// per-tile reference tracking driven by the transport's feedback, so there is
	// never a frame to skip and never a keyframe to ask for.
	bool should_skip(uint64_t) override
	{
		return false;
	};
};

// Which queue family the compositor should hand this encoder's image to.
//
// The GPU backend reads the image with compute: E0 binds its planes as storage
// images and nxvc_vk_encoder submits its passes on the queue it adopted, which
// is WiVRn's graphics/compute queue. Releasing the image to the transfer family
// would mean acquiring it back for a copy that no longer happens, so the "vk"
// backend keeps the image where it was drawn. The CPU backend really does copy
// it, and the transfer queue is the right place for that.
//
// This is read out of the options before the base class is constructed —
// target_queue is const and set in its initialiser list — which is why it is a
// free function over settings rather than a member.
bool nxwarp_backend_is_vk(const wivrn::encoder_settings & settings)
{
	auto it = settings.options.find("backend");
	return it != settings.options.end() and it->second == "vk";
}

uint32_t nxwarp_target_queue(wivrn::vk_bundle & vk, const wivrn::encoder_settings & settings)
{
	if (nxwarp_backend_is_vk(settings))
		return vk.queue.family_index;
	return vk.transfer_queue ? vk.transfer_queue.family_index : vk.queue.family_index;
}

vk::raii::CommandPool make_cmd_pool(wivrn::vk_bundle & vk, uint8_t stream_idx, uint32_t family)
{
	auto res = vk.device.createCommandPool(vk::CommandPoolCreateInfo{
	        .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer | vk::CommandPoolCreateFlagBits::eTransient,
	        .queueFamilyIndex = family,
	});
	vk.name(res, std::format("nxwarp encoder {} command pool", stream_idx));
	return res;
}

uint32_t option_u32(const std::map<std::string, std::string> & options,
                    const char * key,
                    uint32_t fallback)
{
	auto it = options.find(key);
	if (it == options.end())
		return fallback;
	uint32_t out = fallback;
	const auto & v = it->second;
	auto r = std::from_chars(v.data(), v.data() + v.size(), out);
	if (r.ec != std::errc{})
	{
		U_LOG_W("nxwarp: ignoring option %s=\"%s\"", key, v.c_str());
		return fallback;
	}
	return out;
}

bool option_bool(const std::map<std::string, std::string> & options,
                 const char * key,
                 bool fallback)
{
	auto it = options.find(key);
	if (it == options.end())
		return fallback;
	const auto & v = it->second;
	if (v == "1" or v == "on" or v == "true" or v == "yes")
		return true;
	if (v == "0" or v == "off" or v == "false" or v == "no")
		return false;
	U_LOG_W("nxwarp: ignoring option %s=\"%s\"", key, v.c_str());
	return fallback;
}

std::string option_string(const std::map<std::string, std::string> & options,
                          const char * key,
                          const char * fallback)
{
	auto it = options.find(key);
	return it == options.end() ? std::string(fallback) : it->second;
}

int16_t q15(float v)
{
	return int16_t(std::clamp(v, -1.f, 1.f) * 32767.f);
}

int32_t mm_q8(float metres)
{
	return int32_t(std::lround(double(metres) * 1000.0 * 256.0));
}
} // namespace

wivrn::video_encoder_nxwarp::video_encoder_nxwarp(
        wivrn::vk_bundle & vk,
        const encoder_settings & settings,
        uint8_t stream_idx) :
        video_encoder(vk,
                      stream_idx,
                      nxwarp_target_queue(vk, settings),
                      settings,
                      std::make_unique<dummy_idr_handler>(),
                      // Synchronous, like x264: the transport does its own framing
                      // and pacing, so there is nothing for the sender thread to do.
                      false),
        vk{vk},
        cmd_pool{make_cmd_pool(vk, stream_idx, nxwarp_target_queue(vk, settings))},
        eye{stream_idx < 2 ? stream_idx : 0u}
{
	if (settings.bit_depth != 8)
		throw std::runtime_error("NX Warp v1 is an 8-bit bitstream");
	if (extent.width % 2 or extent.height % 2)
		throw std::runtime_error("NX Warp needs even dimensions");

	// INTEGRATION-DECISIONS 6. Must be set before the first frame: the watchdog is
	// polled by the compositor's present path, which is running already.
	watchdog.set_eligible(false);

	base_qp = std::min(63u, option_u32(settings.options, "qp", 28));
	current_qp = base_qp;

	// "rc": "auto" (the default) maps the bitrate the session gives this stream
	// to a quantiser, frame by frame. "fixed" is the behaviour NX Warp shipped
	// with: this stream's `qp`, whatever the link is doing. An unknown value is
	// an error rather than a fallback, for the reason "backend" gives below.
	{
		const std::string rc = option_string(settings.options, "rc", "auto");
		if (rc == "auto")
			rc_auto = true;
		else if (rc == "fixed")
			rc_auto = false;
		else
			throw std::runtime_error(
			        std::format("unknown NX Warp \"rc\" mode \"{}\"; expected \"auto\" or \"fixed\"", rc));
	}
	rc_min_qp = std::min(63u, option_u32(settings.options, "min-qp", 20));
	rc_max_qp = std::min(63u, option_u32(settings.options, "max-qp", 44));
	if (rc_min_qp > rc_max_qp)
		throw std::runtime_error(std::format("NX Warp \"min-qp\" {} is above \"max-qp\" {}",
		                                     rc_min_qp,
		                                     rc_max_qp));
	// The starting QP has to be inside the band the controller may move in, or
	// the first move is a jump rather than a step.
	if (rc_auto)
		current_qp = std::clamp(base_qp, rc_min_qp, rc_max_qp);
	rc_fps = settings.fps;

	nxwarp_codec_config codec_cfg{
	        .width = extent.width,
	        .height = extent.height,
	        .base_qp = current_qp,
	        .inter = option_bool(settings.options, "inter", false),
	        .intra_period = option_u32(settings.options, "intra-period", 180),
	        .intra_dir = option_bool(settings.options, "intra-dir", true),
	        .preset = option_u32(settings.options, "preset", 1),
	        .threads = option_u32(settings.options, "threads", 0),
	};
	// "backend": "ref" (the default) is the CPU reference codec; "vk" is the
	// Vulkan compute encoder, running on this server's own VkDevice. The
	// default stays "ref" because it is the one that has been run end to end
	// on every configuration, and because a build against an nxvc without the
	// Vulkan encoder has no "vk" to offer.
	//
	// An unknown value is an error rather than a fallback: a typo that
	// silently gives you the 200 ms CPU encoder looks exactly like the GPU
	// encoder being slow.
	const std::string backend = option_string(settings.options, "backend", "ref");
	if (backend == "ref")
	{
		codec = nxwarp_codec::make_reference(codec_cfg);
	}
	else if (backend == "vk")
	{
#ifdef WIVRN_NXWARP_VK_ENCODER
		codec_uses_vk_queue = true;
		codec = nxwarp_codec::make_vulkan(codec_cfg,
		                                  *vk.instance,
		                                  *vk.physical_device,
		                                  *vk.device,
		                                  *vk.queue.queue,
		                                  vk.queue.family_index);
#else
		throw std::runtime_error(
		        "\"backend\": \"vk\" needs an nxvc built with the Vulkan encoder "
		        "(-DNXWARP_BUILD_VK=ON with \"encoder\" in NXWARP_VK_SUBDIRS); "
		        "this server was built without it");
#endif
	}
	else
	{
		throw std::runtime_error(
		        std::format("unknown NX Warp backend \"{}\"; expected \"ref\" or \"vk\"",
		                    backend));
	}
	codec_reads_image = codec->accepts_image();
	U_LOG_I("nxwarp: stream %d backend: %s%s",
	        int(stream_idx),
	        codec->description().c_str(),
	        codec_reads_image ? ", reading the compositor image directly" : "");

	uint32_t cols = 0, rows = 0;
	codec->tile_grid(cols, rows);
	if (not cols or not rows)
		throw std::runtime_error("NX Warp codec reported an empty tile grid");

	stream_cfg.stream_id = stream_idx;
	stream_cfg.cols = uint16_t(cols);
	stream_cfg.rows = uint16_t(rows);
	stream_cfg.band_rows = uint16_t(std::min<uint32_t>(rows, option_u32(settings.options, "band-rows", 6)));
	stream_cfg.layers = 1;
	// One nxt datagram travels inside one to_headset::nxwarp_datagram inside one
	// WiVRn UDP datagram, so the transport's MTU has to leave room for WiVRn's own
	// serialisation and its stream encryption. 1280 against a 1400-byte wire
	// budget is ample and keeps the tile runs well clear of fragmentation.
	stream_cfg.mtu = option_u32(settings.options, "mtu", 1280);
	stream_cfg.caps = nxt::kCapFec | nxt::kCapPoseHdr | nxt::kCapRleFeedback;
	stream_cfg.frame_period_us = settings.fps > 0 ? uint32_t(1'000'000.f / settings.fps) : 11111;

	// nxvc_transport never generates or exchanges keys: the integration supplies
	// them. WiVRn's own stream socket is already authenticated and encrypted end
	// to end (crypto_handshake, per-datagram in-place decryption on the client),
	// and this transport rides inside it, so a second AEAD layer would encrypt
	// ciphertext. The NullAead is keyed and detects corruption but is NOT
	// cryptography; it is correct here only because of that outer layer, and
	// wiring nxt's AEAD to the session key is the right follow-up if the datagrams
	// ever leave WiVRn's socket.
	aead = nxt::make_null_aead();
	for (size_t i = 0; i < session_key.size(); ++i)
	{
		session_key[i] = uint8_t(i);
		session_salt[i] = uint8_t(0xA0 + i);
	}
	path_bps = double(settings.bitrate ? settings.bitrate : 100'000'000);
	rebuild_sender();

	tiles_per_frame = stream_cfg.tiles_per_frame();
	chunk_bytes = nxwarp_chunk_bytes(stream_cfg);
	received_tiles.assign(tiles_per_frame, 1);

	vk::DeviceSize buffer_size = vk::DeviceSize(extent.width) * extent.height * 3 / 2;

	auto command_buffers = vk.device.allocateCommandBuffers({
	        .commandPool = *cmd_pool,
	        .commandBufferCount = num_slots,
	});

	for (size_t i = 0; i < num_slots; ++i)
	{
		in[i].cmd = std::move(command_buffers[i]);
		// The readback buffer exists only for a codec that takes host planes.
		// The image path never reads a pixel on the CPU, so allocating 1.7 MB
		// of host-visible memory per slot for it would be 1.7 MB nothing ever
		// touches.
		if (not codec_reads_image)
			in[i].buffer = buffer_allocation(
			        vk.device,
			        {
			                .size = buffer_size,
			                .usage = vk::BufferUsageFlagBits::eTransferDst,
			        },
			        {
			                .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
			                .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
			        },
			        std::format("nxwarp stream {} buffer", stream_idx));
		in[i].fence = vk::raii::Fence(vk.device, vk::FenceCreateInfo{.flags = vk::FenceCreateFlagBits::eSignaled});
	}

	if (not codec_reads_image)
	{
		cb_plane.resize(size_t(extent.width / 2) * (extent.height / 2));
		cr_plane.resize(cb_plane.size());
	}

	const std::string rc_desc =
	        rc_auto ? std::format("rate control from QP {} in {}..{} at {:.0f} Hz",
	                              current_qp, rc_min_qp, rc_max_qp, rc_fps)
	                : std::format("fixed QP {}", base_qp);

	U_LOG_I("nxwarp: stream %d %ux%u, %s, %s, %ux%u tiles in %u band(s), %zu bytes per transport tile",
	        int(stream_idx),
	        unsigned(extent.width),
	        unsigned(extent.height),
	        codec->description().c_str(),
	        rc_desc.c_str(),
	        unsigned(stream_cfg.cols),
	        unsigned(stream_cfg.rows),
	        unsigned(stream_cfg.bands()),
	        chunk_bytes);
}

// The rate controller.
//
// WHAT IT CONTROLS. Not a bit budget in the abstract: bytes per frame. The
// codec has one quantiser per frame and no rate control of its own, so the only
// output variable is a frame's size, and on this headset that size is two things
// at once. It is the link load, and it is the frame rate — NX Warp decodes at
// roughly a millisecond per kilobyte on the Pico 4, so a 12 KB frame decodes
// inside a 90 Hz budget and a 52 KB frame arrives at ten. A controller that
// holds bytes per frame therefore holds both, and there is nothing else in this
// encoder that can.
//
// THE TARGET. `pending_bitrate` is already this stream's share: video_encoder's
// apply_bitrate() takes the whole-link number the session's controller decided,
// multiplies by this encoder's `bitrate_multiplier` (encoder_settings.cpp's
// split_bitrate weighs the eyes, the passthrough alpha stream at 5% and the
// quad layer) and then takes the FEC parity overhead out of it. So the split
// across streams is not re-done here — doing it again would double-apply it —
// and the number simply divides by eight and by the frame rate.
//
// It is read fresh every frame on purpose. WiVRn's automatic bitrate mode moves
// the target at runtime, and an adaptive FEC ratio moves this encoder's share
// without the target moving at all; both arrive through pending_bitrate, so a
// controller that recomputed its budget once at startup would spend the session
// chasing a number nobody is at any more.
//
// THE LAW. One QP step per frame toward the target, two when the frame is more
// than a factor of two out, with a 5% dead band in the middle.
//
//   * a step, rather than a jump to the QP some rate model says fits, because
//     there is no such model that survives a scene change: bytes at a given QP
//     move by an order of magnitude between a dark corridor and a bright
//     detailed room, and a controller that trusts a model overshoots on every
//     cut and then oscillates. The step needs no model at all.
//   * damped to one QP, because a step is roughly a 12% move in bytes and the
//     loop runs at frame rate: 90 corrections a second converge in well under a
//     second from anywhere in the band, which is faster than the bitrate
//     controller above it moves the target.
//   * two when more than 2x out, because that case is not a drift, it is a cut
//     or a fresh target, and single-stepping across a factor of eight would
//     take half a second of frames nobody can decode in time.
//   * the dead band, because without it the QP dithers by one every frame
//     forever — the target is never hit exactly — and every dither is a visible
//     step in a flat gradient.
//
// The band is [min_qp, max_qp] and it is a real limit, not a formality: below
// min_qp the frames cost frame rate on the headset however much link there is,
// and above max_qp there is no point sending the picture at all. A session that
// wants the frames the ceiling implies and cannot have them is a session whose
// ceiling is wrong, and clamping says so in the two-second report rather than
// silently obeying.
void wivrn::video_encoder_nxwarp::run_rate_control(size_t last_frame_bytes)
{
	if (not rc_auto)
		return;

	rc_bitrate = pending_bitrate.load();
	if (not rc_bitrate)
		return; // no ceiling has been decided yet; stay where we are

	// The live compositor rate when the session has set one, else the rate the
	// stream was configured at. A zero here would make the budget infinite.
	float fps = pending_framerate.load();
	if (not(fps > 0))
		fps = rc_fps;
	if (not(fps > 0))
		return;

	rc_target_bytes = double(rc_bitrate) / 8.0 / double(fps);
	const double actual = double(last_frame_bytes);

	int step = 0;
	if (actual > rc_target_bytes * 1.05)
		step = actual > rc_target_bytes * 2.0 ? +2 : +1;
	else if (actual < rc_target_bytes * 0.95)
		step = actual * 2.0 < rc_target_bytes ? -2 : -1;
	if (not step)
		return;

	const uint32_t want = uint32_t(std::clamp(int(current_qp) + step, int(rc_min_qp), int(rc_max_qp)));
	if (want == current_qp)
		return;

	// A QP the codec refused is a QP this frame was not coded at, so it must not
	// become the one the next report claims. Leave current_qp where it is and
	// try again next frame.
	if (codec->set_qp(want))
		current_qp = want;
}

wivrn::video_encoder_nxwarp::~video_encoder_nxwarp()
{
	for (auto & slot: in)
	{
		if (*slot.fence)
			(void)vk.device.waitForFences(*slot.fence, true, 1'000'000'000);
	}
}

void wivrn::video_encoder_nxwarp::present_image(
        vk::Image y_cbcr,
        vk::SemaphoreSubmitInfo compositor_sem,
        uint8_t slot,
        uint64_t,
        const to_headset::video_stream_data_shard::view_info_t & view_info)
{
	if (vk.device.waitForFences(*in[slot].fence, true, 1'000'000'000) == vk::Result::eTimeout)
	{
		U_LOG_E("nxwarp: timeout on stream %d", int(stream_idx));
		return;
	}

	// The pose at present time, which is the whole point of the signature change:
	// the predictor derives the frame's warp matrix from this view and the
	// previous frame's, and encode() is one call too late for that.
	in[slot].view_info = view_info;
	in[slot].have_view_info = true;

	auto & cmd = in[slot].cmd;
	cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

	// --- the image path ----------------------------------------------------
	//
	// Nothing is copied. The codec reads the compositor's image itself, so all
	// this submission exists for is the compositor semaphore: the encode has to
	// wait for the frame to be finished, and encode() waits on this fence. The
	// image stays where it was drawn — target_queue is the graphics/compute
	// family for this backend, so the compositor's release barrier is already a
	// plain memory barrier and there is no ownership to acquire.
	//
	// It stays untouched until encode() returns, which is what the slot state
	// machine in video_encoder::present_image guarantees: the compositor cannot
	// present into this slot again until the encode that reads it is done, and
	// the codec's own submit is waited on inside encode().
	if (codec_reads_image)
	{
		in[slot].image = y_cbcr;
		cmd.end();

		std::unique_lock lock(vk.queue.mutex);
		vk::CommandBufferSubmitInfo cmd_info{.commandBuffer = *cmd};
		compositor_sem.stageMask = vk::PipelineStageFlagBits2::eComputeShader;

		vk.device.resetFences(*in[slot].fence);
		vk.queue.queue.submit2(vk::SubmitInfo2{
		                               .waitSemaphoreInfoCount = 1,
		                               .pWaitSemaphoreInfos = &compositor_sem,
		                               .commandBufferInfoCount = 1,
		                               .pCommandBufferInfos = &cmd_info,
		                       },
		                       *in[slot].fence);
		return;
	}

	if (need_transfer)
	{
		vk::ImageMemoryBarrier2 barrier{
		        .dstStageMask = vk::PipelineStageFlagBits2KHR::eTransfer,
		        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
		        .srcQueueFamilyIndex = vk.queue.family_index,
		        .dstQueueFamilyIndex = target_queue,
		        .image = y_cbcr,
		        .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
		                             .baseMipLevel = 0,
		                             .levelCount = 1,
		                             .baseArrayLayer = src_layer,
		                             .layerCount = 1},
		};
		cmd.pipelineBarrier2({
		        .imageMemoryBarrierCount = 1,
		        .pImageMemoryBarriers = &barrier,
		});
	}

	const std::array regions{
	        vk::BufferImageCopy{
	                .imageSubresource = {
	                        .aspectMask = vk::ImageAspectFlagBits::ePlane0,
	                        .baseArrayLayer = src_layer,
	                        .layerCount = 1,
	                },
	                .imageExtent = {.width = extent.width, .height = extent.height, .depth = 1},
	        },
	        vk::BufferImageCopy{
	                .bufferOffset = vk::DeviceSize(extent.width) * extent.height,
	                .imageSubresource = {
	                        .aspectMask = vk::ImageAspectFlagBits::ePlane1,
	                        .baseArrayLayer = src_layer,
	                        .layerCount = 1,
	                },
	                .imageExtent = {.width = extent.width / 2, .height = extent.height / 2, .depth = 1},
	        },
	};
	cmd.copyImageToBuffer(y_cbcr, vk::ImageLayout::eGeneral, in[slot].buffer, regions);
	cmd.end();

	std::unique_lock lock(vk.transfer_queue ? vk.transfer_queue.mutex : vk.queue.mutex);
	vk::CommandBufferSubmitInfo cmd_info{.commandBuffer = *cmd};
	compositor_sem.stageMask = vk::PipelineStageFlagBits2::eTransfer;

	vk.device.resetFences(*in[slot].fence);
	(vk.transfer_queue ? vk.transfer_queue : vk.queue)
	        .queue.submit2(vk::SubmitInfo2{
	                               .waitSemaphoreInfoCount = 1,
	                               .pWaitSemaphoreInfos = &compositor_sem,
	                               .commandBufferInfoCount = 1,
	                               .pCommandBufferInfos = &cmd_info,
	                       },
	                       *in[slot].fence);
}

void wivrn::video_encoder_nxwarp::rebuild_sender()
{
	sender = std::make_unique<nxt::Sender>(stream_cfg, aead.get(), session_key, session_salt);
	// A chunk that will not fit an MTU cannot be carried without fragmentation.
	// Dropping it loses part of a frame, which the codec's concealment is built
	// for; rejecting the band would lose all of it.
	sender->packetizer().set_policy(nxt::Packetizer::OversizePolicy::kDropTile);
	sender->set_auto_fec(false);
	sender->packetizer().set_fec(nxwarp_fec_policy());
	sender->striper().configure_path(0, path_bps, 8000);
}

// The client lost its references. Nothing about the transport changed, so the
// sender keeps its sequence numbers, its shadow and its path state: the receiver
// on the other end is the same object and is still counting.
void wivrn::video_encoder_nxwarp::reset()
{
	video_encoder::reset();

	// Acted on by encode(), not here: reset() is called from the session's network
	// thread and every call into the codec belongs to the thread that encodes.
	// encode() then hands the codec an all-zero receipt map -- the codec's own lever
	// for "the client does not have these tiles" -- so it has no temporal reference
	// to predict from and codes the frame intra. That is what makes a resumed stream
	// decodable by a client that has never seen a frame once `inter` is on; it is off
	// by default today (nxwarp_codec_config::inter), which is why an all-intra stream
	// survives a reconnect the moment the transport stops rejecting it.
	client_holds_nothing = true;
}

// A new client, with a new nxt::Receiver that starts from nothing.
//
// This is the fix for the reconnect that used to kill the stream until the server
// was restarted. What a fresh Receiver cannot infer, and why each piece matters:
//
//   * the per-path sequence number. nxt::Sender counts datagrams per path in 64
//     bits and puts the low 14 on the wire; the receiver rebuilds the other 50 by
//     extending against what it has already seen (Receiver::process,
//     extend_seq14). A receiver that has seen nothing takes the wire value AS the
//     full sequence -- it has nothing to extend against -- so once the sender has
//     sent more than 16384 datagrams on a path (seconds, at video rates) the
//     nonce the two sides derive from it disagree, and EVERY datagram fails
//     authentication: ReceiverStats::auth_fail climbs with the datagram count,
//     tiles_placed stays at zero, and every frame closes "with a hole" because
//     not one chunk was ever delivered. A new Sender starts its counters at zero,
//     which is exactly what the new Receiver expects.
//
//     The epoch field exists for precisely this and would be the tidier lever,
//     but nothing carries it to the client: the receiver's epoch is zero and the
//     codec's stream header has no room for one. Restarting the counters is safe
//     here only because this AEAD is nxt's NullAead riding inside WiVRn's own
//     encrypted stream socket (see the constructor); a real key on this transport
//     would need the epoch, and the client, to move together.
//
//   * the client shadow, the striper's path state and the band scheduler. All of
//     them describe a headset that is gone.
//
//   * the stream header. The new decoder cannot create its receiver, or its
//     codec, until it has one; it is resent every header_period_frames, so
//     without this the stream would recover by itself after up to a second of
//     black -- but only if the transport were not rejecting the datagrams anyway.
void wivrn::video_encoder_nxwarp::reset_stream()
{
	reset();
	{
		std::lock_guard lock(sender_mutex);
		rebuild_sender();
	}
	// Send it now rather than at the next period boundary: the new client decodes
	// nothing at all until it arrives.
	header_sent = false;
	U_LOG_I("nxwarp: stream %d resumed for a new client: transport sequence restarted, "
	        "stream header resent, next frame coded without a reference",
	        int(stream_idx));
}

void wivrn::video_encoder_nxwarp::send_stream_header()
{
	auto header = codec->stream_header();
	to_headset::nxwarp_datagram pkt{
	        .stream_item_idx = stream_idx,
	        // 0xFF marks the codec's stream header rather than a transport
	        // datagram: it is not an nxt datagram at all, it has no header the
	        // receiver could parse, and the client must hand it straight to
	        // nxvc_decoder_parse_stream_header.
	        .path_id = 0xFF,
	        .payload = std::vector<uint8_t>(header.begin(), header.end()),
	};
	// Control socket: a client that misses this decodes nothing at all, so it is
	// the one part of the stream that must not be lost.
	SendControlPacket(std::move(pkt));
}

std::optional<wivrn::video_encoder::data> wivrn::video_encoder_nxwarp::encode(uint8_t slot, uint64_t frame_id)
{
	if (vk.device.waitForFences(*in[slot].fence, true, 1'000'000'000) == vk::Result::eTimeout)
	{
		U_LOG_W("nxwarp: timeout on stream %d", int(stream_idx));
		return {};
	}
	if (not in[slot].have_view_info)
		return {};

	const auto & view_info = in[slot].view_info;

	if (not header_sent or frame_id - last_header_frame >= header_period_frames)
	{
		send_stream_header();
		header_sent = true;
		last_header_frame = frame_id;
	}

	// --- host side of the image -------------------------------------------
	//
	// Only for a codec that takes host planes. The image path does none of this:
	// no readback, no de-interleave, no upload — E0 reads the compositor's two
	// planes on the device. That readback and this loop were most of what
	// encode() cost at 1088x1088.
	const uint8_t * y = nullptr;
	const size_t cw = extent.width / 2;
	if (not codec_reads_image)
	{
		const uint8_t * base = (const uint8_t *)in[slot].buffer.map();
		y = base;
		const uint8_t * cbcr = base + size_t(extent.width) * extent.height;
		const size_t ch = extent.height / 2;

		// NV12 out of the compositor, planar into the codec. A W/2 x H/2 pass, and
		// the only pixel work this class does: the picture is already YCbCr 4:2:0 and
		// already foveated (INTEGRATION-DECISIONS 1), so there is no conversion.
		for (size_t row = 0; row < ch; ++row)
		{
			const uint8_t * src = cbcr + row * cw * 2;
			uint8_t * dcb = cb_plane.data() + row * cw;
			uint8_t * dcr = cr_plane.data() + row * cw;
			for (size_t x = 0; x < cw; ++x)
			{
				dcb[x] = src[2 * x];
				dcr[x] = src[2 * x + 1];
			}
		}
	}

	// --- what the client actually holds of the previous frame -------------
	//
	// INTEGRATION-DECISIONS 4 plus nxvc's shadow contract: the encoder replays
	// the client's concealment on its own copy of the frame it just encoded, so
	// the next frame is predicted from the client's reconstruction rather than
	// from a reference the client never received. The shadow is rebuilt from the
	// per-band feedback bitmaps by nxt::Sender::on_feedback on the network
	// thread; this is the point at which the codec is told about it.
	//
	// UNKNOWN counts as received: no feedback for a band yet is not evidence of
	// loss, and treating it as loss would force a needless refresh every frame on
	// a link whose feedback is merely late.
	// A reset() since the last frame: the client holds nothing of the previous one,
	// either because it asked for a keyframe or because it is a different client
	// altogether. An all-zero receipt map is how the codec is told so.
	if (client_holds_nothing.exchange(false))
	{
		std::fill(received_tiles.begin(), received_tiles.end(), uint8_t(0));
		codec->set_received_tiles(received_tiles);
		have_previous_frame = false;
	}
	else if (have_previous_frame)
	{
		std::lock_guard lock(sender_mutex);
		auto & shadow = sender->shadow();
		for (uint16_t row = 0; row < stream_cfg.rows; ++row)
		{
			for (uint16_t col = 0; col < stream_cfg.cols; ++col)
			{
				received_tiles[stream_cfg.tile_index(row, col)] =
				        shadow.state(previous_frame_id, row, col) == nxt::ShadowState::kConcealed ? 0 : 1;
			}
		}
		codec->set_received_tiles(received_tiles);
	}

	// --- the pose the predictor warps by ----------------------------------
	const XrPosef & pose = view_info.pose[std::min<size_t>(eye, view_info.pose.size() - 1)];
	const XrFovf & fov = view_info.fov[std::min<size_t>(eye, view_info.fov.size() - 1)];
	codec->set_view(nxwarp_codec_view{
	        .qx = pose.orientation.x,
	        .qy = pose.orientation.y,
	        .qz = pose.orientation.z,
	        .qw = pose.orientation.w,
	        .fov_left = fov.angleLeft,
	        .fov_right = fov.angleRight,
	        .fov_up = fov.angleUp,
	        .fov_down = fov.angleDown,
	});

	if (not logged_rc_mode and pending_bitrate.load())
	{
		logged_rc_mode = true;
		const uint32_t bps = pending_bitrate.load();
		if (rc_auto)
			U_LOG_I("nxwarp: stream %d rate control on: %u bit/s is %.0f B/frame at %.0f Hz, QP band %u..%u",
			        int(stream_idx),
			        unsigned(bps),
			        double(bps) / 8.0 / double(pending_framerate.load() > 0 ? pending_framerate.load() : rc_fps),
			        double(pending_framerate.load() > 0 ? pending_framerate.load() : rc_fps),
			        unsigned(rc_min_qp),
			        unsigned(rc_max_qp));
		else
			// "rc": "fixed" was asked for explicitly, so the ceiling being
			// ignored is the configuration working. Say which knob undoes it.
			U_LOG_I("nxwarp: stream %d is at fixed QP %u by configuration; the bitrate controller's "
			        "%u bit/s is ignored (unset \"rc\": \"fixed\" to honour it)",
			        int(stream_idx),
			        unsigned(base_qp),
			        unsigned(bps));
	}

	// The GPU backend submits its own command buffers on vk.queue, and WiVRn
	// guards that queue with a mutex the compositor also takes — vkQueueSubmit
	// is externally synchronised, so two threads submitting at once is
	// undefined behaviour, not a race to lose. nxvc_vk_encoder is documented
	// as leaving that serialisation to its caller, so this is where it
	// happens. The CPU backend touches no queue and takes no lock.
	const auto t_enc0 = std::chrono::steady_clock::now();
	std::span<const uint8_t> bitstream;
	if (codec_uses_vk_queue)
	{
		std::unique_lock lock(vk.queue.mutex);
		bitstream = codec_reads_image
		                    ? codec->encode_image(in[slot].image, src_layer)
		                    : codec->encode(y, extent.width, cb_plane.data(), cr_plane.data(), cw);
	}
	else
	{
		bitstream = codec->encode(y, extent.width, cb_plane.data(), cr_plane.data(), cw);
	}
	const auto t_enc1 = std::chrono::steady_clock::now();
	if (bitstream.empty())
	{
		U_LOG_W("nxwarp: stream %d frame %llu did not encode", int(stream_idx), (unsigned long long)frame_id);
		return {};
	}
	// Encode wall time, once every two seconds per stream: on a CPU reference
	// encoder this is the number that decides the frame rate a headset sees.
	// The QP this frame was coded at. Everything below reports on the frame that
	// was just encoded — the profile, and the tile records the transport puts on
	// the wire — so none of it may see the quantiser the controller picks at the
	// end of this function for the next frame.
	const uint32_t coded_qp = current_qp;

	{
		const double ms = std::chrono::duration<double, std::milli>(t_enc1 - t_enc0).count();
		prof_qp_sum += coded_qp;
		prof_qp_lo = std::min(prof_qp_lo, coded_qp);
		prof_qp_hi = std::max(prof_qp_hi, coded_qp);
		prof_total_n++;
		prof_total_ms += ms;
		prof_total_max_ms = std::max(prof_total_max_ms, ms);
		prof_total_bytes += bitstream.size();
		prof_n++;
		prof_ms += ms;
		prof_max_ms = std::max(prof_max_ms, ms);
		prof_bytes += bitstream.size();
		if (t_enc1 - prof_since > std::chrono::seconds(2))
		{
			// The applied QP and the bytes it bought, next to the budget they
			// were aimed at: those three numbers together are the whole of what
			// a session needs to see to know whether the ceiling is being met,
			// and whether it is being met at a quantiser anyone wants. A band
			// that is one value wide is a settled controller; a QP pinned at
			// min_qp or max_qp with the bytes still off target is a ceiling this
			// stream cannot reach from inside its band.
			const double achieved = double(prof_bytes) / double(prof_n);
			const double mean_qp = double(prof_qp_sum) / double(prof_n);
			if (rc_auto and rc_target_bytes > 0)
				U_LOG_I("nxwarp: stream %d encoded %llu frames in %.1f s: %.1f ms/frame (max %.1f), "
				        "%.0f B/frame vs %.0f target (%+.0f%%), QP %.1f [%u..%u]%s",
				        int(stream_idx), (unsigned long long)prof_n,
				        std::chrono::duration<double>(t_enc1 - prof_since).count(),
				        prof_ms / prof_n, prof_max_ms,
				        achieved, rc_target_bytes,
				        100.0 * (achieved - rc_target_bytes) / rc_target_bytes,
				        mean_qp, unsigned(prof_qp_lo), unsigned(prof_qp_hi),
				        current_qp == rc_min_qp ? " (at min QP)"
				                                : (current_qp == rc_max_qp ? " (at max QP)" : ""));
			else
				U_LOG_I("nxwarp: stream %d encoded %llu frames in %.1f s: %.1f ms/frame (max %.1f), "
				        "%.0f B/frame at fixed QP %u",
				        int(stream_idx), (unsigned long long)prof_n,
				        std::chrono::duration<double>(t_enc1 - prof_since).count(),
				        prof_ms / prof_n, prof_max_ms, achieved, unsigned(current_qp));
			prof_n = 0;
			prof_ms = 0;
			prof_max_ms = 0;
			prof_bytes = 0;
			prof_qp_sum = 0;
			prof_qp_lo = 63;
			prof_qp_hi = 0;
			prof_since = t_enc1;
		}
	}

	// --- frame bytes onto the tile grid -----------------------------------
	//
	// The transport carries tiles, each an opaque blob with its own directory
	// entry, and it is the codec that decides where a tile ends. The CPU
	// reference codec's C ABI reports a tile's payload *length* but not its
	// offset in the frame (nxvc_tile_info has no offset field, and nx-warp's own
	// transport_loopback example fills synthetic bytes for exactly this reason),
	// so there is no way to hand nxt the real per-tile spans from this backend.
	//
	// What happens instead: the frame is cut into MTU-sized chunks and chunk i is
	// placed at tile index i of the grid. The packetizer, the class-A FEC, the
	// pose header, the band deadlines, the feedback and the client shadow all run
	// on their real paths and the bytes round-trip exactly; what is lost is
	// per-tile independence, so a chunk that does not arrive costs the frame
	// rather than one tile. That is a Phase 3 limitation of this codec backend,
	// not of the transport or of the wire format, and it disappears when the
	// Vulkan encoder — which produces its segments already datagram-sized and
	// knows where each tile starts — is dropped in behind nxwarp_codec.
	//
	// The per-tile descriptors the codec does report (mode, qp, res_level,
	// ref_delta) are still attached, in tile order, so the directory the client
	// sees is honest about the frame even while the byte split is not.
	// kFrameLenBytes for the length prefix nxwarp_send_frame puts in front of
	// chunk 0, so this warning agrees with the check that actually drops the frame.
	const size_t chunks = (bitstream.size() + kFrameLenBytes + chunk_bytes - 1) / chunk_bytes;
	if (chunks > tiles_per_frame)
	{
		if (not logged_oversize)
		{
			logged_oversize = true;
			U_LOG_W("nxwarp: stream %d frame is %zu bytes, more than the %u tile slots x %zu bytes the grid can carry; raise QP",
			        int(stream_idx),
			        bitstream.size(),
			        unsigned(tiles_per_frame),
			        chunk_bytes);
		}
		return {};
	}

	auto descs = codec->tiles();

	nxt::PoseHeader pose_hdr{};
	pose_hdr.pose_seq = uint16_t(frame_id);
	pose_hdr.quat[0] = q15(pose.orientation.x);
	pose_hdr.quat[1] = q15(pose.orientation.y);
	pose_hdr.quat[2] = q15(pose.orientation.z);
	pose_hdr.quat[3] = q15(pose.orientation.w);
	pose_hdr.pos_mm_q8[0] = mm_q8(pose.position.x);
	pose_hdr.pos_mm_q8[1] = mm_q8(pose.position.y);
	pose_hdr.pos_mm_q8[2] = mm_q8(pose.position.z);

	const uint16_t frame_id16 = uint16_t(frame_id);
	const uint64_t encode_end_ns = uint64_t(os_monotonic_get_ns());
	const uint32_t now_us = uint32_t(encode_end_ns / 1000);

	std::vector<nxt::Datagram> datagrams;
	{
		std::lock_guard lock(sender_mutex);
		datagrams = nxwarp_send_frame(*sender,
		                              stream_cfg,
		                              frame_id16,
		                              pose_hdr,
		                              bitstream,
		                              descs,
		                              chunk_bytes,
		                              coded_qp,
		                              now_us,
		                              uint16_t(std::min<uint64_t>(65535, (uint64_t(os_monotonic_get_ns()) - encode_end_ns) / 1000)));
	}

	for (size_t i = 0; i < datagrams.size(); ++i)
	{
		SendPacket(to_headset::nxwarp_datagram{
		                   .stream_item_idx = stream_idx,
		                   .path_id = datagrams[i].path_id,
		                   // First datagram of the frame carries the pose and
		                   // projection it was rendered for, and no other does: the
		                   // same rule the shard path follows, and the reason the
		                   // headset can reproject an NX Warp frame at all. The
		                   // transport's own pose header is quantised and has no fov.
		                   .view_info = i == 0 ? std::optional(view_info) : std::nullopt,
		                   .payload = std::move(datagrams[i].bytes),
		           },
		           i + 1 == datagrams.size());
	}

	in[slot].have_view_info = false;
	previous_frame_id = frame_id16;
	have_previous_frame = true;

	// The quantiser for the NEXT frame, from the size of this one. Last, because
	// everything above describes the frame that was just sent and the controller
	// is about to invalidate `current_qp` for it.
	run_rate_control(bitstream.size());

	// Nothing for the sender thread: everything is already on the wire. The
	// watchdog knows this encoder sends synchronously (async_send == false in the
	// base constructor), so an empty return is success here, not a stall.
	return {};
}

void wivrn::video_encoder_nxwarp::on_nxwarp_feedback(uint8_t path_id, std::span<const uint8_t> payload)
{
	std::lock_guard lock(sender_mutex);
	// Decrypts, applies the per-band bitmaps to the client shadow and updates the
	// path statistics. Everything the encoder does with the result happens at the
	// top of the next encode(), which is where nxvc's shadow contract wants it:
	// "call set_received_tiles after encode_frame and before the next one".
	if (not sender->on_feedback(payload, path_id, uint64_t(os_monotonic_get_ns()) / 1000))
		U_LOG_D("nxwarp: stream %d rejected a feedback packet", int(stream_idx));
}
