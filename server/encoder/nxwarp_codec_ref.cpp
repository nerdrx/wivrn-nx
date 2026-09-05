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

#include <nxvc/nxvc.h>

#include <format>
#include <stdexcept>
#include <vector>

namespace
{

// The CPU reference encoder behind nxwarp_codec.
//
// nxvc_ref is normative, not fast: it is docs/SYNTAX.md in executable form and
// the thing the Vulkan encoder has to be equal to. It is here so that an
// end-to-end NX Warp session can exist at all before the compute encoder lands,
// which is why every knob it exposes for speed is turned the cheap way (no RDO
// trellis, no per-tile QP search, one directional-intra candidate).
class nxwarp_codec_ref final : public wivrn::nxwarp_codec
{
	nxvc_encoder * enc = nullptr;
	nxvc_tile_layout layout{};

	std::vector<uint8_t> header;
	std::vector<uint8_t> bitstream;
	size_t bitstream_len = 0;
	std::vector<wivrn::nxwarp_tile_desc> tile_descs;

public:
	explicit nxwarp_codec_ref(const wivrn::nxwarp_codec_config & c)
	{
		nxvc_config cfg;
		nxvc_config_default(&cfg);
		cfg.width = c.width;
		cfg.height = c.height;
		cfg.chroma = NXVC_CHROMA_420;
		cfg.bit_depth = 8;
		// INTEGRATION-DECISIONS 1: WiVRn's Linux compositor hands the encoder a
		// two-plane 4:2:0 YCbCr image that is already foveated, so the planes are
		// coded exactly as delivered and there is no colour conversion stage at
		// all. NXVC_CT_YCOCGR is for the RGB sources (the Windows helper).
		cfg.color_transform = NXVC_CT_NONE;
		cfg.color_space = NXVC_CS_YCBCR_709_LIMITED;
		cfg.alpha = 0;
		cfg.base_qp = c.base_qp;
		cfg.eyes = 1; // one WiVRn stream is one eye; stereo pairing is a later item
		cfg.inter = c.inter ? 1 : 0;
		cfg.stereo = 0;
		cfg.intra_period = c.intra_period;

		// Encoder-side speed levers. None of them changes how a stream decodes.
		// Eight rANS lanes, fixed. The library's "auto" lane count drops to one
		// lane per tile at high QP, which costs the Adreno decoder 4x of Pass A
		// for a 30% rate saving it cannot afford (vk/decoder README, live shape).
		cfg.nsub_log2 = 3;
		cfg.rdo = 0;         // dead-zone quantiser instead of the RD trellis (~2.7x)
		cfg.qp_search = 0;   // no per-tile QP offset search
		cfg.intra_dir = c.intra_dir ? 1 : 0;
		cfg.intra_dir_cand = 1;
		cfg.preset = c.preset;
		cfg.threads = c.threads; // the tile pool (ref/README.md "Encoder threading")
		cfg.collect_stats = 0;

		nxvc_status st = NXVC_OK;
		enc = nxvc_encoder_create(&cfg, &st);
		if (not enc or st != NXVC_OK)
			throw std::runtime_error(std::format("nxvc_encoder_create failed: {}",
			                                     nxvc_status_string(st)));

		nxvc_tile_layout_get_ex(c.width, c.height, cfg.eyes, &layout);

		header.resize(4096);
		size_t len = 0;
		st = nxvc_encoder_stream_header(enc, header.data(), header.size(), &len);
		if (st != NXVC_OK)
		{
			nxvc_encoder_destroy(enc);
			enc = nullptr;
			throw std::runtime_error(std::format("nxvc_encoder_stream_header failed: {}",
			                                     nxvc_status_string(st)));
		}
		header.resize(len);

		// Worst case for a frame is bounded by the source: a stream that spent more
		// bytes than the raw picture would have been better off transform-skipped.
		// The margin covers the headers, the per-lane rANS initialisers and a
		// pathological all-intra frame at QP 0.
		bitstream.resize(size_t(c.width) * c.height * 3 + (1u << 20));
	}

	~nxwarp_codec_ref() override
	{
		if (enc)
			nxvc_encoder_destroy(enc);
	}

	std::span<const uint8_t> stream_header() const override
	{
		return header;
	}

	void tile_grid(uint32_t & cols, uint32_t & rows) const override
	{
		cols = layout.tiles_x;
		rows = layout.tile_count / (layout.tiles_x ? layout.tiles_x : 1);
	}

	void set_view(const wivrn::nxwarp_codec_view & v) override
	{
		nxvc_view view{
		        .qx = v.qx,
		        .qy = v.qy,
		        .qz = v.qz,
		        .qw = v.qw,
		        .fov_left = v.fov_left,
		        .fov_right = v.fov_right,
		        .fov_up = v.fov_up,
		        .fov_down = v.fov_down,
		};
		nxvc_encoder_set_views(enc, &view, 1);
	}

	std::span<const uint8_t> encode(const uint8_t * y,
	                                size_t y_stride,
	                                const uint8_t * cb,
	                                const uint8_t * cr,
	                                size_t chroma_stride) override
	{
		nxvc_image img{};
		img.plane[0] = const_cast<uint8_t *>(y);
		img.plane[1] = const_cast<uint8_t *>(cb);
		img.plane[2] = const_cast<uint8_t *>(cr);
		img.stride[0] = int32_t(y_stride);
		img.stride[1] = int32_t(chroma_stride);
		img.stride[2] = int32_t(chroma_stride);

		bitstream_len = 0;
		nxvc_status st = nxvc_encoder_encode_frame(enc, &img, nullptr, nullptr,
		                                          bitstream.data(), bitstream.size(),
		                                          &bitstream_len);
		if (st != NXVC_OK)
		{
			bitstream_len = 0;
			tile_descs.clear();
			return {};
		}

		uint32_t count = 0;
		const nxvc_tile_info * ti = nxvc_encoder_tiles(enc, &count);
		tile_descs.resize(count);
		for (uint32_t i = 0; i < count; ++i)
		{
			tile_descs[i] = {
			        .index = ti[i].tile_index,
			        .qp = ti[i].qp,
			        .mode = ti[i].mode,
			        .res_level = ti[i].res_level,
			        .ref_delta = ti[i].ref_delta,
			};
		}
		return std::span<const uint8_t>(bitstream.data(), bitstream_len);
	}

	std::span<const wivrn::nxwarp_tile_desc> tiles() const override
	{
		return tile_descs;
	}

	void set_received_tiles(std::span<const uint8_t> received) override
	{
		if (received.empty())
			return;
		nxvc_encoder_set_received_tiles(enc, received.data(), uint32_t(received.size()));
	}

	std::string description() const override
	{
		return std::format("nxvc_ref CPU reference encoder, {} ({}x{} tiles)",
		                   nxvc_version_string(),
		                   layout.tiles_x,
		                   layout.tiles_x ? layout.tile_count / layout.tiles_x : 0);
	}
};

} // namespace

std::unique_ptr<wivrn::nxwarp_codec> wivrn::nxwarp_codec::make_reference(const nxwarp_codec_config & cfg)
{
	return std::make_unique<nxwarp_codec_ref>(cfg);
}
