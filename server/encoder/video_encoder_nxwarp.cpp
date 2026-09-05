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

vk::raii::CommandPool make_cmd_pool(wivrn::vk_bundle & vk, uint8_t stream_idx)
{
	auto res = vk.device.createCommandPool(vk::CommandPoolCreateInfo{
	        .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer | vk::CommandPoolCreateFlagBits::eTransient,
	        .queueFamilyIndex = vk.transfer_queue ? vk.transfer_queue.family_index : vk.queue.family_index,
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
                      vk.transfer_queue ? vk.transfer_queue.family_index : vk.queue.family_index,
                      settings,
                      std::make_unique<dummy_idr_handler>(),
                      // Synchronous, like x264: the transport does its own framing
                      // and pacing, so there is nothing for the sender thread to do.
                      false),
        vk{vk},
        cmd_pool{make_cmd_pool(vk, stream_idx)},
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

	nxwarp_codec_config codec_cfg{
	        .width = extent.width,
	        .height = extent.height,
	        .base_qp = base_qp,
	        .inter = option_bool(settings.options, "inter", false),
	        .intra_period = option_u32(settings.options, "intra-period", 180),
	        .intra_dir = option_bool(settings.options, "intra-dir", true),
	        .preset = option_u32(settings.options, "preset", 1),
	        .threads = option_u32(settings.options, "threads", 0),
	};
	codec = nxwarp_codec::make_reference(codec_cfg);

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
	nxt::Key key{}, salt{};
	for (size_t i = 0; i < key.size(); ++i)
	{
		key[i] = uint8_t(i);
		salt[i] = uint8_t(0xA0 + i);
	}
	sender = std::make_unique<nxt::Sender>(stream_cfg, aead.get(), key, salt);
	// A chunk that will not fit an MTU cannot be carried without fragmentation.
	// Dropping it loses part of a frame, which the codec's concealment is built
	// for; rejecting the band would lose all of it.
	sender->packetizer().set_policy(nxt::Packetizer::OversizePolicy::kDropTile);
	sender->set_auto_fec(false);
	sender->packetizer().set_fec(nxwarp_fec_policy());
	sender->striper().configure_path(0, double(settings.bitrate ? settings.bitrate : 100'000'000), 8000);

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

	cb_plane.resize(size_t(extent.width / 2) * (extent.height / 2));
	cr_plane.resize(cb_plane.size());

	U_LOG_I("nxwarp: stream %d %ux%u, %s, base QP %u, %ux%u tiles in %u band(s), %zu bytes per transport tile",
	        int(stream_idx),
	        unsigned(extent.width),
	        unsigned(extent.height),
	        codec->description().c_str(),
	        unsigned(base_qp),
	        unsigned(stream_cfg.cols),
	        unsigned(stream_cfg.rows),
	        unsigned(stream_cfg.bands()),
	        chunk_bytes);
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
	const uint8_t * base = (const uint8_t *)in[slot].buffer.map();
	const uint8_t * y = base;
	const uint8_t * cbcr = base + size_t(extent.width) * extent.height;
	const size_t cw = extent.width / 2;
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
	if (have_previous_frame)
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

	if (not logged_bitrate_note and pending_bitrate.load())
	{
		logged_bitrate_note = true;
		// Rate control is nx-warp's rc/ component and is not wired yet: the
		// controller's number is accepted and ignored rather than silently
		// mapped to a QP nobody measured. Fixed QP until then.
		U_LOG_I("nxwarp: stream %d runs at fixed QP %u; the bitrate controller's %u bit/s is not applied yet",
		        int(stream_idx),
		        unsigned(base_qp),
		        unsigned(pending_bitrate.load()));
	}

	const auto t_enc0 = std::chrono::steady_clock::now();
	auto bitstream = codec->encode(y, extent.width, cb_plane.data(), cr_plane.data(), cw);
	const auto t_enc1 = std::chrono::steady_clock::now();
	if (bitstream.empty())
	{
		U_LOG_W("nxwarp: stream %d frame %llu did not encode", int(stream_idx), (unsigned long long)frame_id);
		return {};
	}
	// Encode wall time, once every two seconds per stream: on a CPU reference
	// encoder this is the number that decides the frame rate a headset sees.
	{
		const double ms = std::chrono::duration<double, std::milli>(t_enc1 - t_enc0).count();
		prof_n++;
		prof_ms += ms;
		prof_max_ms = std::max(prof_max_ms, ms);
		prof_bytes += bitstream.size();
		if (t_enc1 - prof_since > std::chrono::seconds(2))
		{
			U_LOG_I("nxwarp: stream %d encoded %llu frames in %.1f s: %.1f ms/frame (max %.1f), %.0f B/frame",
			        int(stream_idx), (unsigned long long)prof_n,
			        std::chrono::duration<double>(t_enc1 - prof_since).count(),
			        prof_ms / prof_n, prof_max_ms, double(prof_bytes) / prof_n);
			prof_n = 0;
			prof_ms = 0;
			prof_max_ms = 0;
			prof_bytes = 0;
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
		                              base_qp,
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
