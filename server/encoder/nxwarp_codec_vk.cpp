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

#include "util/u_logging.h"

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
// INTER PREDICTION is on when the `inter` option is, and then `intra-period`
// sets the rolling refresh -- 1/T of the tiles are coded fresh every frame and
// each tile position exactly once every T frames, which is the loss-recovery
// bound the format states. Two calls carry it, and both are made
// unconditionally by video_encoder_nxwarp: set_view() every frame, and
// set_received_tiles() whenever the transport has feedback.
//
// WHAT IT DOES NOT DO, and how it refuses:
//
//   * WARP_MV and QUAD_MV. `inter = true` gets WARP_SKIP, STATIC_MV and
//     INTRA: a tile predicts from the pose-warped reference and carries no
//     payload, or carries a translation vector and a residual, or is coded
//     fresh. That is 3.95x fewer bytes at 1088x1088. WARP_MV is measured and
//     deliberately absent -- 6.2% fewer bytes than STATIC_MV alone and 0.12 dB
//     WORSE, for a predictor the encoder's search cannot evaluate cheaply.
//     "coded-vectors": "none" pins the skip-only shape, which is 2.73x.
//   * a rate controller of its own, and per-tile quantisers. One QP codes
//     every tile of a frame — but that QP is settable between frames
//     (set_qp), so the controller in video_encoder_nxwarp drives this backend
//     exactly as it drives the reference one.
//   * with `inter = false` -- the default -- set_view() and
//     set_received_tiles() are still accepted and ignored by the library, for
//     the reason they always were: an all-intra frame has no reference to warp
//     and no prediction a lost tile can corrupt. The calls stay unconditional
//     here so this file has no mode to get wrong.
//   * 10-bit, 4:4:4 and an alpha plane are refused by the library at create().
//
// THE INPUT PATH is the compositor's image itself. E0 reads the two planes of
// the VkImage the compositor drew, through R8_UINT / R8G8_UINT storage views,
// and writes the tile-major planes E3 consumes — on the device, with no host
// copy of the picture anywhere. What that needs of the caller, and where each
// piece is:
//
//   * the image's VkImageFormatListCreateInfo must name R8_UINT and R8G8_UINT
//     as well as the _UNORM plane formats, or the plane views are invalid;
//     image_formats() in server/compositor/compositor.cpp and in
//     quad_converter.cpp names all four;
//   * VK_IMAGE_USAGE_STORAGE_BIT with EXTENDED_USAGE, which the compositor
//     already sets for its own writes;
//   * VK_IMAGE_LAYOUT_GENERAL, owned by the queue family this codec was
//     created with — which is why video_encoder_nxwarp points its target_queue
//     at the graphics/compute queue for this backend rather than at the
//     transfer queue. nxvc_vk_encoder adopts a compute-capable queue and
//     submits its passes there; a picture sitting on the transfer family would
//     have to be acquired first, for a copy nobody makes any more.
//
// encode() — the host-plane entry point — is still implemented, and is what a
// caller with pixels rather than an image gets. Both produce the same bytes;
// nx-warp's tests/vk-encoder acid tests pin each of them against nxv-enc
// separately.
class nxwarp_codec_vk final : public wivrn::nxwarp_codec
{
	nxvc_vk_encoder * enc = nullptr;

	std::vector<uint8_t> header;
	std::vector<wivrn::nxwarp_tile_desc> tile_descs;
	uint32_t cols = 0, rows = 0;
	// One warning each, not one per frame: a refused view or a wrong-sized
	// receipt map is a wiring mistake that would otherwise fill the log at
	// 90 Hz and hide whatever came next.
	bool warned_view = false;
	bool warned_recv = false;
	uint32_t width = 0, height = 0;
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

		// Inter prediction, the pose warp and the reference ring.  The
		// library refuses `intra_period` without `inter`, so it is only
		// passed with it; 0 there would mean "the library's default", and
		// the option's own default is already 180.
		ci.inter = c.inter ? 1u : 0u;
		ci.intra_period = c.inter ? c.intra_period : 0u;
		// The library refuses a non-default value without `inter`, the same
		// way it refuses a period without it, so this is only passed with it.
		// The two enumerations are deliberately the same three values in the
		// same order; the mapping is written out rather than cast, because a
		// cast would keep compiling the day either side gains a fourth.
		ci.coded_vectors = NXVC_VKE_CV_DEFAULT;
		if (c.inter)
		{
			using cv = wivrn::nxwarp_codec_config::coded_vectors_t;
			switch (c.coded_vectors)
			{
				case cv::none:
					ci.coded_vectors = NXVC_VKE_CV_NONE;
					break;
				case cv::statik:
					ci.coded_vectors = NXVC_VKE_CV_STATIC;
					break;
				case cv::def:
					ci.coded_vectors = NXVC_VKE_CV_DEFAULT;
					break;
			}
		}

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

		width = c.width;
		height = c.height;
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

	// The frame's pose and projection, for the frame the next encode codes.
	//
	// `nxwarp_codec_view` and `nxvc_vke_view` are the same eight doubles in
	// the same order and the same conventions -- an OpenXR quaternion and an
	// XrFovf with left and down negative -- so this is a copy and not a
	// conversion.  They are kept as separate types so that server/encoder does
	// not include nxvc's headers in its own interface, not because they
	// differ.
	//
	// It MUST be called for every frame, including the first.  The library
	// keeps the view that went with each reference-ring slot and derives
	// warp_ext() from that slot's view and this frame's, so a skipped call
	// does not degrade gracefully: it makes the encoder predict confidently
	// from the wrong pose, which looks like a bad codec rather than a missing
	// call.  On an intra stream the library accepts and ignores it.
	void set_view(const wivrn::nxwarp_codec_view & v) override
	{
		nxvc_vke_view nv{};
		nv.qx = v.qx;
		nv.qy = v.qy;
		nv.qz = v.qz;
		nv.qw = v.qw;
		nv.fov_left = v.fov_left;
		nv.fov_right = v.fov_right;
		nv.fov_up = v.fov_up;
		nv.fov_down = v.fov_down;

		const nxvc_vke_status st = nxvc_vk_encoder_set_view(enc, &nv);
		if (st != NXVC_VKE_OK and not warned_view)
		{
			warned_view = true;
			U_LOG_W("nxwarp: the GPU encoder refused a view: %s (%s)",
			        nxvc_vk_encoder_status_string(st),
			        nxvc_vk_encoder_last_error(enc));
		}
	}

	// Which tiles the client actually holds, one byte per tile.
	//
	// The library's rule is the blunt one: a tile the client does not hold is
	// coded INTRA on the next frame, and it holds for that one frame.  An
	// ALL-ZERO map is therefore a full reset, and that is how a resumed
	// session says so -- there is no separate reset entry point and there does
	// not need to be.
	//
	// The size has to be the tile count exactly; the library refuses anything
	// else rather than guessing which tiles a short map describes.  An empty
	// span is the caller saying "no feedback this frame", which is not the
	// same statement as "the client holds nothing", so it is dropped here
	// rather than forwarded as a reset.
	void set_received_tiles(std::span<const uint8_t> received) override
	{
		if (received.empty())
			return;

		const uint32_t want = cols * rows;
		if (received.size() != want)
		{
			if (not warned_recv)
			{
				warned_recv = true;
				U_LOG_W("nxwarp: receipt map is %zu bytes, expected %u (one per tile); ignoring",
				        received.size(),
				        unsigned(want));
			}
			return;
		}

		const nxvc_vke_status st =
		        nxvc_vk_encoder_set_received_tiles(enc, received.data(), want);
		if (st != NXVC_VKE_OK and not warned_recv)
		{
			warned_recv = true;
			U_LOG_W("nxwarp: the GPU encoder refused a receipt map: %s (%s)",
			        nxvc_vk_encoder_status_string(st),
			        nxvc_vk_encoder_last_error(enc));
		}
	}

	// The quantiser, which this backend DOES honour: nxvc_vk_encoder_set_qp
	// rewrites the frame parameter record and the job list, both of which the
	// library re-uploads on every encode, so nothing is rebuilt and the next
	// frame simply carries the new QP. nx-warp's tests/vk-encoder/qp_switch
	// pins byte identity with nxv-enc across the change, frame by frame, which
	// is what makes it safe to move every frame.
	bool set_qp(uint32_t qp) override
	{
		const nxvc_vke_status st = nxvc_vk_encoder_set_qp(enc, qp);
		if (st != NXVC_VKE_OK)
		{
			U_LOG_W("nxwarp: the GPU encoder refused QP %u: %s (%s)",
			        unsigned(qp),
			        nxvc_vk_encoder_status_string(st),
			        nxvc_vk_encoder_last_error(enc));
			return false;
		}
		return true;
	}

	// The image path. See the class comment for what the image must be; the
	// library checks what it can (geometry, layout) and refuses the rest at the
	// only place it could be checked, which is the caller.
	bool accepts_image() const override
	{
		return true;
	}

	std::span<const uint8_t> encode_image(VkImage image, uint32_t array_layer) override
	{
		nxvc_vke_image img{
		        .image = image,
		        .layout = VK_IMAGE_LAYOUT_GENERAL,
		        .array_layer = array_layer,
		        .width = width,
		        .height = height,
		        .flags = 0,
		};
		const uint8_t * bytes = nullptr;
		size_t len = 0;
		nxvc_vke_status st = nxvc_vk_encoder_encode_image(enc, &img, &bytes, &len);
		if (st != NXVC_VKE_OK or not bytes)
		{
			U_LOG_W("nxwarp: the GPU encoder refused the compositor image: %s (%s)",
			        nxvc_vk_encoder_status_string(st),
			        nxvc_vk_encoder_last_error(enc));
			tile_descs.clear();
			return {};
		}
		last_ms = nxvc_vk_encoder_last_encode_ms(enc);
		last_upload_ms = nxvc_vk_encoder_last_upload_ms(enc);
		fill_tiles();
		return std::span<const uint8_t>(bytes, len);
	}

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

		fill_tiles();
		return std::span<const uint8_t>(bytes, len);
	}

	void fill_tiles()
	{
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
	}

	std::span<const wivrn::nxwarp_tile_desc> tiles() const override
	{
		return tile_descs;
	}

	std::string description() const override
	{
		return std::format("nxvc_vk_encoder GPU compute encoder, {} ({}x{} tiles, "
		                   "intra only, one QP per frame)",
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
