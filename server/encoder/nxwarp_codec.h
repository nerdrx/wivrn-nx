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

#include <cstdint>
#include <memory>
#include <span>
#include <string>

// Only for the five handles make_vulkan() takes. Nothing else in this header
// is Vulkan, and the CPU implementation never touches them.
#include <vulkan/vulkan.h>

namespace wivrn
{

// The seam between video_encoder_nxwarp and whatever actually produces NX Warp
// bytes.
//
// Today the only implementation is the CPU reference codec (nxvc_ref), which is
// the executable form of nx-warp/docs/SYNTAX.md and is far too slow to be the
// shipping encoder. The Vulkan compute encoder is being built in parallel and
// will implement this same interface, so the encoder above it — the image copy,
// the transport, the feedback, the WiVRn plumbing — is written once and does not
// move when the codec is swapped.
//
// Everything here is deliberately in WiVRn's own vocabulary and carries no nxvc
// type: video_encoder_nxwarp.cpp does not include nxvc.h, and a build that
// selects a different backend does not have to have the reference codec's
// headers on the include path.
struct nxwarp_codec_config
{
	uint32_t width = 0;  // luma samples, per eye
	uint32_t height = 0;
	// The quantiser, 0..63, every tile of a frame is coded at, until
	// nxwarp_codec::set_qp says otherwise. With "rc": "auto" this is only the
	// starting point the controller in video_encoder_nxwarp moves from.
	uint32_t base_qp = 28;
	// Inter prediction: the pose warp, per-tile motion vectors and the reference
	// ring. Off makes every frame all-intra, which is the safe default for a
	// first end-to-end bring-up because it needs no client reference state.
	bool inter = false;
	// Rolling intra refresh period in frames. 1 forces every tile every frame.
	uint32_t intra_period = 180;
	// Which coded-vector mode the inter decision may choose on top of
	// WARP_SKIP and INTRA. Only the Vulkan backend reads it; the reference
	// codec runs its own mode search and has every mode.
	//
	// The default is STATIC, because on the Vulkan encoder it is free: at
	// 1088x1088 QP 30 it takes the stream from 13446 to 9303 bytes a frame
	// AND the encode from 4.79 ms to 4.69, since a frame with fewer coded
	// tiles is cheaper in the passes that scale with coded tiles than the
	// search costs before them. `none` exists to pin the older stream shape.
	enum class coded_vectors_t
	{
		def = 0,  // STATIC
		none = 1, // WARP_SKIP and INTRA only
		statik = 2,
	};
	coded_vectors_t coded_vectors = coded_vectors_t::def;
	// Encoder-side speed knobs; none of them changes how a stream decodes.
	// Directional intra (tool 17): costs the CPU encoder most of its time at
	// this resolution; off codes the DC-plane predictor only.
	bool intra_dir = true;
	// nxvc effort preset: 0 medium, 1 fast, 2 slow.
	uint32_t preset = 1;
	// Encoder worker threads: 0 = all cores (capped at 16), 1 = serial.
	uint32_t threads = 0;
};

// One eye's view for one frame, OpenXR conventions: a unit quaternion and the
// four field-of-view half angles in radians, left and down negative. The only
// floating point the codec takes, and only on the encoder side — the predictor
// quantises it into the bitstream's integer warp matrix.
struct nxwarp_codec_view
{
	double qx = 0, qy = 0, qz = 0, qw = 1;
	double fov_left = 0, fov_right = 0, fov_up = 0, fov_down = 0;
};

// What the transport needs to know about a tile that is not its bytes.
struct nxwarp_tile_desc
{
	uint32_t index = 0; // raster order within the frame
	uint8_t qp = 0;
	uint8_t mode = 0;       // nxvc_tile_mode / nxt::TileMode, same numbering
	uint8_t res_level = 0;  // per-tile resolution level, 0..2
	uint8_t ref_delta = 3;  // 0..2 temporal distance, 3 = no temporal reference
};

class nxwarp_codec
{
public:
	virtual ~nxwarp_codec() = default;

	// The stream header (magic, geometry, tool mask, TLV area). Constant for the
	// life of the object; the client must parse it before the first frame.
	virtual std::span<const uint8_t> stream_header() const = 0;

	// Tile grid the bitstream is divided into. The transport's StreamConfig is
	// built from this, so its tile indices and the codec's are the same numbers.
	virtual void tile_grid(uint32_t & cols, uint32_t & rows) const = 0;

	// Pose and projection of the frame that is about to be encoded. Must be
	// called before every encode(); without it the codec emits an identity warp.
	virtual void set_view(const nxwarp_codec_view &) = 0;

	// The quantiser, 0..63, for the frames that follow. This is the whole of the
	// rate-control surface a backend exposes: no NX Warp codec moves the QP on
	// its own, so the controller lives in video_encoder_nxwarp and this is how
	// it speaks.
	//
	// Cheap by contract — nothing is recreated and nothing is allocated — so
	// calling it before every frame is the intended use, and setting the QP the
	// codec already has is a no-op. Returns false if the backend refused it,
	// which for a rate controller means "keep the QP you thought you had"
	// rather than "give up": a QP that did not take must not be reported as the
	// one the frame was coded at.
	virtual bool set_qp(uint32_t qp) = 0;

	// Encode one frame from planar 8-bit 4:2:0. Returns the frame's bytes, valid
	// until the next call, or an empty span on failure (which is logged by the
	// implementation). `cb` and `cr` are half size in both axes.
	virtual std::span<const uint8_t> encode(const uint8_t * y,
	                                        size_t y_stride,
	                                        const uint8_t * cb,
	                                        const uint8_t * cr,
	                                        size_t chroma_stride) = 0;

	// True when this codec can encode straight out of the compositor's image,
	// on the GPU, with no host copy of the picture: encode_image() below is
	// then the entry point and encode() is never called.
	//
	// It is a property of the backend and of the build, not of a frame, so the
	// encoder above asks once and shapes present_image around the answer — the
	// image path needs no readback buffer, no de-interleave scratch and no
	// ownership transfer to the transfer queue, and allocating them anyway
	// would be dead weight per slot.
	virtual bool accepts_image() const
	{
		return false;
	}

	// Encode one frame from a two-plane 4:2:0 image on the server's VkDevice —
	// the compositor's own render target, in VK_IMAGE_LAYOUT_GENERAL and owned
	// by the queue the codec was created with. `array_layer` is the eye's layer
	// of the compositor's array image.
	//
	// Returns the frame's bytes, valid until the next call, or an empty span on
	// failure. Same contract as encode() in every other respect, including that
	// tiles() describes the frame it just produced. The call submits and waits,
	// so the image is free again when it returns — and must not be written
	// before that.
	virtual std::span<const uint8_t> encode_image(VkImage, uint32_t array_layer)
	{
		return {};
	}

	// Per-tile records of the frame encode() just produced.
	virtual std::span<const nxwarp_tile_desc> tiles() const = 0;

	// Which tiles of the frame just encoded the client actually holds, from the
	// transport's feedback. The codec replays the client's concealment on its own
	// shadow copy so that the next frame is predicted from what the client has,
	// not from what was sent. `received[t] == 0` means lost.
	virtual void set_received_tiles(std::span<const uint8_t> received) = 0;

	// Human readable identification of the backend, logged once at stream start.
	virtual std::string description() const = 0;

	// The CPU reference codec. Throws std::runtime_error if the configuration is
	// outside what it accepts.
	static std::unique_ptr<nxwarp_codec> make_reference(const nxwarp_codec_config &);

	// The Vulkan compute encoder (nxvc_vk_encoder), running on the VkDevice the
	// server already owns — the one the compositor's image lives on — the same
	// way the headset's decoder adopts the client's. Throws std::runtime_error
	// if the configuration is outside what it accepts, which is a strictly
	// smaller set than the reference's: intra only, no rate control, 8-bit
	// 4:2:0. See nxwarp_codec_vk.cpp for what it ignores and what it refuses.
	//
	// Declared unconditionally, defined only when the build found an nxvc with
	// the Vulkan encoder in it; video_encoder_nxwarp guards the call site on
	// WIVRN_NXWARP_VK_ENCODER so a server built against a reference-only nxvc
	// still links.
	static std::unique_ptr<nxwarp_codec> make_vulkan(const nxwarp_codec_config &,
	                                                 VkInstance,
	                                                 VkPhysicalDevice,
	                                                 VkDevice,
	                                                 VkQueue,
	                                                 uint32_t queue_family);
};

} // namespace wivrn
