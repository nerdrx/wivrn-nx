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
	bool warned_held = false;
	// Whether the caller wants the held reports acted on at all; see
	// nxwarp_codec_config::frame_held.
	bool frame_held = true;
	uint32_t width = 0, height = 0;
	// Eye pairing. `width`/`height` are PER EYE, as everywhere in nxvc; the
	// picture the encoder is configured for is `pair_width` = eyes * width, and
	// that is the number nxvc_vke_image::width must carry.
	uint32_t eyes = 1;
	uint32_t pair_width = 0;
	std::string device;

	// --- the stereo compose. See encode_image_pair().
	VkPhysicalDevice vk_phys = VK_NULL_HANDLE;
	VkDevice vk_dev = VK_NULL_HANDLE;
	VkQueue vk_queue = VK_NULL_HANDLE;
	uint32_t vk_family = 0;
	VkImage compose_image = VK_NULL_HANDLE;
	VkDeviceMemory compose_mem = VK_NULL_HANDLE;
	VkCommandPool compose_pool = VK_NULL_HANDLE;
	VkCommandBuffer compose_cmd = VK_NULL_HANDLE;
	VkFence compose_fence = VK_NULL_HANDLE;
	VkQueryPool compose_qpool = VK_NULL_HANDLE;
	bool compose_ready = false;
	bool compose_undefined = true;
	bool compose_warned = false;
	double timestamp_period_ns = 0; // 0 = the device cannot time this
	double last_compose_ms = 0;

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

		vk_phys = physical_device;
		vk_dev = dev;
		vk_queue = queue;
		vk_family = queue_family;

		ci.width = c.width;
		ci.height = c.height;
		// 1, or 2 for a side-by-side stereo frame. nxvc_vk_encoder_tile_grid()
		// then reports `cols` over the pair, which is what the transport wants,
		// so tile_grid() below needs no adjustment of its own.
		ci.eyes = c.eyes ? c.eyes : 1;
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
		// The headset confirms the frames it reconstructs, so require a confirmation
		// from the first frame rather than waiting for one to arrive: the frames
		// between the initial INTRA and the first confirmation are exactly the ones a
		// headset that is already behind refuses, and the chain-derived record cannot
		// help there because it is optimistic for one round trip.
		//
		// Safe to assume unconditionally: protocol_version is a hash of the packet
		// types, from_headset::nxwarp_feedback carries the confirmation, so a client
		// that does not send one cannot complete the handshake with this server.
		ci.ref_confirm = (c.inter and c.frame_held) ? 1u : 0u;
		frame_held = c.frame_held;
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

		// The entropy tool (stream bit 30).  Written out rather than cast for
		// the same reason `coded_vectors` is, and passed unconditionally
		// because -- unlike the inter fields -- it is legal on every
		// configuration.  Selecting Lite also turns SIGN_HIDE, CUSTOM_TABLES
		// and TAB_V2 off inside the library, because the syntax forbids the
		// combination; the stream header the client parses is the authority
		// on what a stream actually carries.
		switch (c.entropy)
		{
			case wivrn::nxwarp_codec_config::entropy_t::lite:
				ci.entropy = NXVC_VKE_ENTROPY_LITE;
				break;
			case wivrn::nxwarp_codec_config::entropy_t::rans:
				ci.entropy = NXVC_VKE_ENTROPY_RANS;
				break;
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
		eyes = ci.eyes;
		pair_width = c.width * eyes;
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

	// The frame-level report, which is what lets a dropped frame cost one
	// ref_sel step instead of a whole intra frame.  nxvc keeps the chain --
	// a frame predicted from an unheld frame is itself unheld -- so this
	// forwards the report and nothing more.
	bool supports_frame_held() const override
	{
		return frame_held;
	}

	void set_frame_held(uint32_t frame_number, bool held) override
	{
		const nxvc_vke_status st =
		        nxvc_vk_encoder_set_frame_held(enc, frame_number, held ? 1 : 0);
		if (st != NXVC_VKE_OK and not warned_held)
		{
			warned_held = true;
			U_LOG_W("nxwarp: the GPU encoder refused a frame-held report: %s (%s)",
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

	// The stereo compose.
	//
	// WiVRn keeps the two eyes in separate ARRAY LAYERS of one image; nxvc's image
	// entry point wants ONE side-by-side picture, `eyes * width` wide, and its
	// `array_layer` selects a layer of an array image rather than an eye. So the
	// pair has to be brought together somewhere, and this is the cheapest place:
	// a GPU-side copy of each layer into the two halves of a scratch image, then
	// the ordinary encode_image() on that.
	//
	// It costs one full-frame copy per frame that the mono path does not pay, and
	// it does undo, for the stereo case only, the "no plane ever touches host
	// memory" property the image entry point exists for -- but only in the sense
	// that the picture is copied ON the device. Nothing crosses the bus and no
	// plane is laid out on the host. encode_image_pair() times the copy with a
	// timestamp pair so the cost is measured rather than asserted; the number
	// comes back through last_compose_ms().
	//
	// The alternative -- widening the compositor's image so the eyes are already
	// side by side -- would touch every other encoder's src_layer and make
	// video_stream_description::width the pair's, which the client's alpha offset
	// reads as one eye's. The right long-term fix is neither: it is teaching
	// nxvc's E0 to read eye 1 from a second array layer, which it could do almost
	// for free because its tile index is already pair-wide. That is a change in
	// nx-warp, so it is a follow-up and not this.
	bool ensure_compose()
	{
		if (compose_ready)
			return true;

		VkPhysicalDeviceProperties props{};
		vkGetPhysicalDeviceProperties(vk_phys, &props);
		// A timestamp period of 0 means the device does not support them; the
		// compose still runs, it just is not timed.
		timestamp_period_ns = props.limits.timestampPeriod;

		// Exactly the image nxvc_vk_enc.h asks for: the two-plane 4:2:0 format,
		// MUTABLE_FORMAT with a format list that names the UINT plane views (a
		// list that omits them makes those views invalid and a driver may refuse
		// them), and STORAGE reached through EXTENDED_USAGE because the planar
		// format has no storage feature of its own. TRANSFER_DST is ours.
		static const VkFormat view_formats[] = {
		        VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
		        VK_FORMAT_R8_UNORM,
		        VK_FORMAT_R8G8_UNORM,
		        VK_FORMAT_R8_UINT,
		        VK_FORMAT_R8G8_UINT,
		};
		VkImageFormatListCreateInfo fmt_list{
		        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
		        .viewFormatCount = uint32_t(std::size(view_formats)),
		        .pViewFormats = view_formats,
		};
		VkImageCreateInfo ici{
		        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		        .pNext = &fmt_list,
		        .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
		                 VK_IMAGE_CREATE_EXTENDED_USAGE_BIT,
		        .imageType = VK_IMAGE_TYPE_2D,
		        .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
		        .extent = {pair_width, height, 1},
		        .mipLevels = 1,
		        .arrayLayers = 1,
		        .samples = VK_SAMPLE_COUNT_1_BIT,
		        .tiling = VK_IMAGE_TILING_OPTIMAL,
		        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};
		if (vkCreateImage(vk_dev, &ici, nullptr, &compose_image) != VK_SUCCESS)
			return compose_fail("could not create the compose image");

		VkMemoryRequirements req{};
		vkGetImageMemoryRequirements(vk_dev, compose_image, &req);
		VkPhysicalDeviceMemoryProperties mem{};
		vkGetPhysicalDeviceMemoryProperties(vk_phys, &mem);
		uint32_t type = UINT32_MAX;
		for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
			if ((req.memoryTypeBits & (1u << i)) and
			    (mem.memoryTypes[i].propertyFlags &
			     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
			{
				type = i;
				break;
			}
		if (type == UINT32_MAX)
			return compose_fail("no device-local memory type for the compose image");

		VkMemoryAllocateInfo mai{
		        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		        .allocationSize = req.size,
		        .memoryTypeIndex = type,
		};
		if (vkAllocateMemory(vk_dev, &mai, nullptr, &compose_mem) != VK_SUCCESS)
			return compose_fail("could not allocate the compose image");
		if (vkBindImageMemory(vk_dev, compose_image, compose_mem, 0) != VK_SUCCESS)
			return compose_fail("could not bind the compose image");

		VkCommandPoolCreateInfo pci{
		        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		        .queueFamilyIndex = vk_family,
		};
		if (vkCreateCommandPool(vk_dev, &pci, nullptr, &compose_pool) != VK_SUCCESS)
			return compose_fail("could not create the compose command pool");

		VkCommandBufferAllocateInfo cbi{
		        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		        .commandPool = compose_pool,
		        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		        .commandBufferCount = 1,
		};
		if (vkAllocateCommandBuffers(vk_dev, &cbi, &compose_cmd) != VK_SUCCESS)
			return compose_fail("could not allocate the compose command buffer");

		VkFenceCreateInfo fci{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		if (vkCreateFence(vk_dev, &fci, nullptr, &compose_fence) != VK_SUCCESS)
			return compose_fail("could not create the compose fence");

		if (timestamp_period_ns > 0)
		{
			VkQueryPoolCreateInfo qci{
			        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
			        .queryType = VK_QUERY_TYPE_TIMESTAMP,
			        .queryCount = 2,
			};
			if (vkCreateQueryPool(vk_dev, &qci, nullptr, &compose_qpool) != VK_SUCCESS)
				compose_qpool = VK_NULL_HANDLE; // not fatal, just untimed
		}

		compose_ready = true;
		return true;
	}

	bool compose_fail(const char * why)
	{
		if (not compose_warned)
		{
			U_LOG_W("nxwarp: stereo compose unavailable: %s", why);
			compose_warned = true;
		}
		return false;
	}

	// Copy one array layer of `src` into the half of the compose image at
	// `dst_x`. A two-plane 4:2:0 image is copied a plane at a time: the chroma
	// plane is half the luma in both axes, so its offset and extent are halved
	// too. The per-eye width is a multiple of 64 (nxvc refuses eyes == 2
	// otherwise), so `dst_x / 2` is exact and the seam falls on a chroma sample.
	static void copy_layer(VkCommandBuffer cmd,
	                       VkImage src,
	                       uint32_t layer,
	                       VkImage dst,
	                       uint32_t dst_x,
	                       uint32_t w,
	                       uint32_t h)
	{
		const VkImageCopy regions[] = {
		        {
		                .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT,
		                                   .mipLevel = 0,
		                                   .baseArrayLayer = layer,
		                                   .layerCount = 1},
		                .srcOffset = {0, 0, 0},
		                .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT,
		                                   .mipLevel = 0,
		                                   .baseArrayLayer = 0,
		                                   .layerCount = 1},
		                .dstOffset = {int32_t(dst_x), 0, 0},
		                .extent = {w, h, 1},
		        },
		        {
		                .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT,
		                                   .mipLevel = 0,
		                                   .baseArrayLayer = layer,
		                                   .layerCount = 1},
		                .srcOffset = {0, 0, 0},
		                .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT,
		                                   .mipLevel = 0,
		                                   .baseArrayLayer = 0,
		                                   .layerCount = 1},
		                .dstOffset = {int32_t(dst_x / 2), 0, 0},
		                .extent = {w / 2, h / 2, 1},
		        },
		};
		vkCmdCopyImage(cmd,
		               src,
		               VK_IMAGE_LAYOUT_GENERAL,
		               dst,
		               VK_IMAGE_LAYOUT_GENERAL,
		               uint32_t(std::size(regions)),
		               regions);
	}

	std::span<const uint8_t> encode_image_pair(VkImage image,
	                                           uint32_t layer_left,
	                                           uint32_t layer_right) override
	{
		if (eyes != 2)
			return encode_image(image, layer_left);
		if (not ensure_compose())
			return {};

		vkResetCommandBuffer(compose_cmd, 0);
		VkCommandBufferBeginInfo bi{
		        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		vkBeginCommandBuffer(compose_cmd, &bi);

		// UNDEFINED only once: after the first frame the image already holds the
		// previous pair, and discarding it would be a lie the driver is entitled
		// to act on. Every later frame transitions GENERAL -> GENERAL, which is
		// just the execution and memory dependency against the encoder's reads
		// of the frame before.
		VkImageMemoryBarrier to_dst{
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask = compose_undefined ? VkAccessFlags(0)
		                                           : VkAccessFlags(VK_ACCESS_SHADER_READ_BIT),
		        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		        .oldLayout = compose_undefined ? VK_IMAGE_LAYOUT_UNDEFINED
		                                       : VK_IMAGE_LAYOUT_GENERAL,
		        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image = compose_image,
		        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
		vkCmdPipelineBarrier(compose_cmd,
		                     compose_undefined ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
		                                       : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		                     VK_PIPELINE_STAGE_TRANSFER_BIT,
		                     0, 0, nullptr, 0, nullptr, 1, &to_dst);
		compose_undefined = false;

		if (compose_qpool)
		{
			vkCmdResetQueryPool(compose_cmd, compose_qpool, 0, 2);
			vkCmdWriteTimestamp(compose_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			                    compose_qpool, 0);
		}

		copy_layer(compose_cmd, image, layer_left, compose_image, 0, width, height);
		copy_layer(compose_cmd, image, layer_right, compose_image, width, width, height);

		if (compose_qpool)
			vkCmdWriteTimestamp(compose_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			                    compose_qpool, 1);

		// The encoder's E0 reads this as a storage image, so the copy has to be
		// visible to a shader read before its submit runs. nxvc submits and waits
		// on its own queue -- the same queue this went on -- so the barrier plus
		// the fence below is the whole ordering.
		VkImageMemoryBarrier to_read{
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image = compose_image,
		        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
		vkCmdPipelineBarrier(compose_cmd,
		                     VK_PIPELINE_STAGE_TRANSFER_BIT,
		                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		                     0, 0, nullptr, 0, nullptr, 1, &to_read);
		vkEndCommandBuffer(compose_cmd);

		vkResetFences(vk_dev, 1, &compose_fence);
		VkSubmitInfo si{
		        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		        .commandBufferCount = 1,
		        .pCommandBuffers = &compose_cmd,
		};
		if (vkQueueSubmit(vk_queue, 1, &si, compose_fence) != VK_SUCCESS)
			return compose_fail("the compose submit failed"), std::span<const uint8_t>{};
		vkWaitForFences(vk_dev, 1, &compose_fence, VK_TRUE, UINT64_MAX);

		if (compose_qpool)
		{
			uint64_t ts[2] = {0, 0};
			if (vkGetQueryPoolResults(vk_dev, compose_qpool, 0, 2, sizeof ts, ts,
			                          sizeof(uint64_t),
			                          VK_QUERY_RESULT_64_BIT |
			                                  VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS and
			    ts[1] >= ts[0])
				last_compose_ms = double(ts[1] - ts[0]) * timestamp_period_ns * 1e-6;
		}

		return encode_image(compose_image, 0);
	}

	double compose_ms() const override
	{
		return last_compose_ms;
	}

	std::span<const uint8_t> encode_image(VkImage image, uint32_t array_layer) override
	{
		nxvc_vke_image img{
		        .image = image,
		        .layout = VK_IMAGE_LAYOUT_GENERAL,
		        .array_layer = array_layer,
		        .width = pair_width,
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
			// The byte span too, which the reference's ABI cannot report:
			// E5 computes the frame's layout, so this is a read of it. It is
			// what lets the transport put a tile's own bytes at its own tile
			// index instead of cutting the frame into fixed chunks.
			tile_descs[i] = {
			        .index = ti[i].index,
			        .offset = ti[i].offset,
			        .length = ti[i].length,
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

	bool reports_tile_spans() const override
	{
		return true;
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
