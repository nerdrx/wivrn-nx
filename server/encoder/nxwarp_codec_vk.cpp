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

#include "nxwarp_codec.h"

#include <nxvc/nxvc_vk_enc.h>

#include <format>
#include <stdexcept>
#include <vector>

namespace
{

// The Vulkan compute encoder behind nxwarp_codec.
//
// Same seam, same vocabulary, a different machine underneath: nxvc_vk_encoder
// runs the E0..E5 compute passes of nx-warp's vk/encoder on the VkDevice this
// server already owns, so the frame never leaves the GPU the compositor drew
// it on. It is byte-identical to nxvc_ref at the same settings — nx-warp's
// tests/vk-encoder/acid.cmake pins that, and the settings are fixed inside its
// C ABI rather than chosen here — so swapping the backend changes the encode
// time and nothing about the stream.
//
// WHAT IT DOES NOT DO, and how it refuses:
//
//   * inter prediction, the pose warp and the reference ring. `inter = true`
//     is REFUSED at construction, because silently coding all-intra when the
//     caller asked for inter would be a performance mystery rather than an
//     error. Every frame is intra, so `intra_period` is moot and ignored.
//   * rate control. The QP is fixed, exactly as it is for the reference
//     backend; this is not a regression from it.
//   * set_view() and set_received_tiles() are ACCEPTED AND IGNORED, and that
//     is deliberate rather than lazy. An all-intra frame has no reference to
//     warp and no prediction a lost tile can corrupt, so there is nothing for
//     either to change. They stay wired so video_encoder_nxwarp does not have
//     to branch on the backend, and so the day inter lands the plumbing above
//     is already correct. The transport still gets real feedback and still
//     conceals; it is only the encoder's own shadow that has nothing to do.
//   * 10-bit, 4:4:4 and an alpha plane are refused by the library at create().
//
// The input path today is the same host planes the reference takes. The image
// path — E0 reading the compositor's VkImage directly — needs one change
// outside this file: the compositor creates its image with a
// VkImageFormatListCreateInfo naming R8_UNORM and R8G8_UNORM, and E0 binds its
// planes as UINT storage views, which that list does not permit. Until those
// two formats are added to the list this backend takes the readback
// video_encoder_nxwarp already does. It is the one remaining copy.
class nxwarp_codec_vk final : public wivrn::nxwarp_codec
{
	nxvc_vk_encoder * enc = nullptr;

	std::vector<uint8_t> header;
	std::vector<wivrn::nxwarp_tile_desc> tile_descs;
	uint32_t cols = 0, rows = 0;
	std::string device;

	// Wall time of the last encode(), milliseconds, as the library measured it
	// around its own submits. video_encoder_nxwarp times the whole call; this
	// is the GPU half of that, and the difference is the plane repack.
	double last_ms = 0, last_upload_ms = 0;

public:
	explicit nxwarp_codec_vk(const wivrn::nxwarp_codec_config & c,
	                         VkInstance instance,
	                         VkPhysicalDevice physical_device,
	                         VkDevice dev,
	                         VkQueue queue,
	                         uint32_t queue_family)
	{
		if (c.inter)
			throw std::runtime_error(
			        "the NX Warp Vulkan encoder is intra-only; "
			        "unset the \"inter\" option or use \"backend\": \"ref\"");

		nxvc_vke_create_info ci;
		nxvc_vk_encoder_create_info_default(&ci);
		// All five handles or none — the library then creates and destroys
		// nothing it did not allocate itself, exactly as the headset's
		// decoder does with the client's device.
		ci.instance = instance;
		ci.physical_device = physical_device;
		ci.device = dev;
		ci.queue = queue;
		ci.queue_family = queue_family;

		ci.width = c.width;
		ci.height = c.height;
		ci.eyes = 1; // one WiVRn stream is one eye
		ci.chroma = 0;
		ci.bit_depth = 8;
		ci.base_qp = c.base_qp;
		ci.quant_matrix = 1; // the reference's frame matrix

		nxvc_vke_status st = nxvc_vk_encoder_create(&ci, &enc);
		if (st != NXVC_VKE_OK or not enc)
			throw std::runtime_error(std::format("nxvc_vk_encoder_create failed: {}",
			                                     nxvc_vk_encoder_status_string(st)));

		size_t len = 0;
		nxvc_vk_encoder_stream_header(enc, nullptr, 0, &len);
		header.resize(len);
		st = nxvc_vk_encoder_stream_header(enc, header.data(), header.size(), &len);
		if (st != NXVC_VKE_OK)
		{
			nxvc_vk_encoder_destroy(enc);
			enc = nullptr;
			throw std::runtime_error(std::format("nxvc_vk_encoder_stream_header failed: {}",
			                                     nxvc_vk_encoder_status_string(st)));
		}
		header.resize(len);

		nxvc_vk_encoder_tile_grid(enc, &cols, &rows);
		device = nxvc_vk_encoder_device_name(enc);
	}

	~nxwarp_codec_vk() override
	{
		if (enc)
			nxvc_vk_encoder_destroy(enc);
	}

	std::span<const uint8_t> stream_header() const override
	{
		return header;
	}

	void tile_grid(uint32_t & c, uint32_t & r) const override
	{
		c = cols;
		r = rows;
	}

	// Accepted and ignored; see the class comment.
	void set_view(const wivrn::nxwarp_codec_view &) override {}
	void set_received_tiles(std::span<const uint8_t>) override {}

	std::span<const uint8_t> encode(const uint8_t * y,
	                                size_t y_stride,
	                                const uint8_t * cb,
	                                const uint8_t * cr,
	                                size_t chroma_stride) override
	{
		const uint8_t * bytes = nullptr;
		size_t len = 0;
		nxvc_vke_status st = nxvc_vk_encoder_encode_planes(
		        enc, y, y_stride, cb, cr, chroma_stride, &bytes, &len);
		if (st != NXVC_VKE_OK or not bytes)
		{
			tile_descs.clear();
			return {};
		}
		last_ms = nxvc_vk_encoder_last_encode_ms(enc);
		last_upload_ms = nxvc_vk_encoder_last_upload_ms(enc);

		uint32_t count = 0;
		const nxvc_vke_tile * ti = nxvc_vk_encoder_tiles(enc, &count);
		tile_descs.resize(count);
		for (uint32_t i = 0; i < count; ++i)
		{
			// nxvc_vke_tile also carries the tile's byte offset and length,
			// which the reference's ABI cannot report. nxwarp_tile_desc has
			// nowhere to put them yet, so the transport still uses the chunk
			// mapping; making it the identity mapping is a change to
			// nxwarp_packetize and to the client's reassembly, not to this
			// file. See docs/NXWARP-E2E.md section 5.
			tile_descs[i] = {
			        .index = ti[i].index,
			        .qp = ti[i].qp,
			        .mode = ti[i].mode,
			        .res_level = ti[i].res_level,
			        .ref_delta = ti[i].ref_delta,
			};
		}
		return std::span<const uint8_t>(bytes, len);
	}

	std::span<const wivrn::nxwarp_tile_desc> tiles() const override
	{
		return tile_descs;
	}

	std::string description() const override
	{
		return std::format("nxvc_vk_encoder GPU compute encoder, {} ({}x{} tiles, "
		                   "intra only, fixed QP)",
		                   device,
		                   cols,
		                   rows);
	}
};

} // namespace

std::unique_ptr<wivrn::nxwarp_codec> wivrn::nxwarp_codec::make_vulkan(
        const nxwarp_codec_config & cfg,
        VkInstance instance,
        VkPhysicalDevice physical_device,
        VkDevice device,
        VkQueue queue,
        uint32_t queue_family)
{
	return std::make_unique<nxwarp_codec_vk>(cfg, instance, physical_device,
	                                         device, queue, queue_family);
}
