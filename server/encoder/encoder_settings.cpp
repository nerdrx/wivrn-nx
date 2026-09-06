/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
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
#include "encoder_settings.h"

#include "stream_scale.h"

#include "driver/configuration.h"
#include "driver/wivrn_session.h"
#include "util/u_logging.h"
#include "utils/wivrn_vk_bundle.h"
#include "video_encoder.h"
#include "wivrn_packets.h"

#include <format>
#include <ranges>
#include <algorithm>
#include <magic_enum.hpp>
#include <string>
#include <vulkan/vulkan.hpp>

#include "wivrn_config.h"

#if WIVRN_USE_NVENC
#include "video_encoder_nvenc.h"
#endif
#if WIVRN_USE_VAAPI
#include "ffmpeg/video_encoder_va.h"
#include <libavutil/ffversion.h>
#endif

namespace wivrn
{

static const double passthrough_bitrate_factor = 0.05;
// The promoted quad layer is a text panel: the whole point of pulling it out of the
// eye images is that it stays legible, so its pixels are weighted like eye pixels
// rather than discounted the way the passthrough alpha plane is. At the default cap
// of 1024x1024 against two 2000x2000 eyes that lands around 11% of the ceiling. The
// stream only exists when the headset asked for it, and it is only fed on frames
// that actually promote a layer, so nothing is spent when the feature is unused —
// but the share is reserved for the whole session, which is why the toggle is read
// when the encoders are created and not only per frame.
static const double quad_bitrate_factor = 1.0;

// The hybrid base layer's share of the ceiling, as a fraction.
//
// This is a POLICY number and deliberately not an area weight. The base is
// pair-wide and so is the enhancement stream it serves, so weighting by
// width*height would split the ceiling down the middle, and the gate measured
// that the base does not want anything like half:
// NXWARP-HYBRID-GATE.md §2 has it saturating at 3-5 Mbit/s per eye for
// head-rotation content and 10-15 Mbit/s for dense static UI, against WiVRn
// streams that run at 50-100 Mbit/s today. At the 100 Mbit/s default this
// fraction is 12 Mbit/s for the pair, which is inside the measured saturation
// band for both content classes; the remainder stays with the enhancement layer,
// whose job under the gate's result is loss recovery, latency cover and foveal
// detail rather than quality correction.
//
// It is a fraction of the ceiling rather than a fixed Mbit/s so that a session
// configured well below the default does not spend most of its budget on the
// base.
static const double base_bitrate_fraction = 0.12;

static void split_bitrate(std::array<wivrn::encoder_settings, num_streams> & encoders, uint64_t bitrate)
{
	assert(bitrate > 0);

	// The base layer is taken off the top, before the area weighting, and the
	// remainder is split among the others exactly as it was. Doing it this way
	// rather than as another weight is what keeps the base's share a number
	// somebody chose from a measurement.
	uint64_t base_bitrate = 0;
	for (auto & e: encoders)
	{
		if (not e.enabled or e.role != stream_role::base)
			continue;
		base_bitrate = uint64_t(bitrate * base_bitrate_fraction);
		e.bitrate = base_bitrate;
		e.bitrate_multiplier = base_bitrate_fraction;
	}
	if (base_bitrate >= bitrate)
		base_bitrate = 0; // a degenerate ceiling; fall back to the plain split
	const uint64_t ceiling = bitrate;
	bitrate -= base_bitrate;
	// What is left for everyone else, as a fraction of the ceiling. The
	// multipliers below are relative to the CEILING and not to the remainder,
	// because that is what the live rate control multiplies when the ceiling
	// moves -- without this the shares would sum to 1 + base_bitrate_fraction.
	const double rest_fraction = double(bitrate) / double(ceiling);

	double total_weight = 0;
	for (auto [i, encoder]: std::ranges::enumerate_view(encoders))
	{
		if (not encoder.enabled)
		{
			encoder.bitrate = 0;
			encoder.bitrate_multiplier = 0;
			continue;
		}
		if (encoder.role == stream_role::base)
			continue; // already served, off the top
		double w = encoder.width * encoder.height;
		if (i == 2)
			w *= passthrough_bitrate_factor;
		if (i == quad_stream_idx)
			w *= quad_bitrate_factor;
		switch (encoder.codec)
		{
			case wivrn::h264:
				w *= 2;
				break;
			case wivrn::h265:
			case wivrn::av1:
			case wivrn::raw:
				break;
		}
		encoder.bitrate = w;
		total_weight += w;
	}

	if (total_weight <= 0)
		return;
	for (auto & encoder: encoders)
	{
		if (not encoder.enabled or encoder.role == stream_role::base)
			continue;
		encoder.bitrate_multiplier = (encoder.bitrate / total_weight) * rest_fraction;
		encoder.bitrate = encoder.bitrate_multiplier * ceiling;
	}
}

void print_encoders(const std::array<wivrn::encoder_settings, num_streams> & encoders)
{
	std::stringstream str;
	str << "Encoder configuration:";
	for (auto & encoder: encoders)
	{
		if (not encoder.enabled)
			continue;
		str << "\n\t* " << encoder.encoder_name << " (" << magic_enum::enum_name(encoder.codec) << " " << encoder.bit_depth << "-bit)"
		    << "\n\t  size: " << encoder.width << "x" << encoder.height
		    << "\n\t  bitrate: " << int(encoder.bitrate / 100'000) / 10. << "Mbit/s";
	}
	U_LOG_I("%s", str.str().c_str());
}

static void check_video_size(std::string_view encoder_name, video_codec codec, uint16_t & width, uint16_t & height)
{
#if WIVRN_USE_NVENC
	if (encoder_name == encoder_nvenc)
	{
		auto max = video_encoder_nvenc::get_max_size(codec);
		width = std::min<uint16_t>(max[0], width);
		height = std::min<uint16_t>(max[1], height);
	}
#endif
}

namespace
{
class prober
{
	wivrn::vk_bundle & vk;
	const from_headset::headset_info_packet & info;
	const bool nvidia;

#if WIVRN_USE_VAAPI
	std::unordered_map<video_codec, bool> vaapi_support;

	bool check_vaapi(video_codec codec)
	{
		if (auto it = vaapi_support.find(codec); it != vaapi_support.end())
			return it->second;
		try
		{
			video_encoder_va test(
			        vk,
			        encoder_settings{
			                .width = 800,
			                .height = 800,
			                .codec = codec,
			                .fps = 60,
			                .bitrate = 50'000'000,
			                .bit_depth = 8,
			        },
			        0);
			vaapi_support[codec] = true;
			return true;
		}
		catch (std::exception & e)
		{
			vaapi_support[codec] = false;
			U_LOG_I("vaapi not supported for %s", std::string(magic_enum::enum_name(codec)).c_str());
			return false;
		}
	}
#endif

#if WIVRN_USE_NVENC
	std::unordered_map<video_codec, bool> nvenc_support;

	bool check_nvenc(video_codec codec)
	{
		if (auto it = nvenc_support.find(codec); it != nvenc_support.end())
			return it->second;
		try
		{
			video_encoder_nvenc test(
			        vk,
			        encoder_settings{
			                .width = 800,
			                .height = 800,
			                .codec = codec,
			                .fps = 60,
			                .bitrate = 50'000'000,
			                .bit_depth = 8,
			        },
			        0);
			nvenc_support[codec] = true;
			return true;
		}
		catch (std::exception & e)
		{
			nvenc_support[codec] = false;
			U_LOG_I("nvenc not supported for %s", std::string(magic_enum::enum_name(codec)).c_str());
			return false;
		}
	}
#endif

	static bool is_nvidia(vk::raii::PhysicalDevice & physical_device)
	{
		auto props = physical_device.getProperties();
		return props.vendorID == 0x10DE;
	}

#if WIVRN_USE_VULKAN_ENCODE

	bool has_vk(video_codec codec)
	{
		if (vk.encode_queues.empty())
			return false;
		if (not std::get<vk::PhysicalDeviceVideoMaintenance1FeaturesKHR>(vk.feat).videoMaintenance1)
		{
			U_LOG_I("Cannot use vulkan video encode without VideoMaintenance1 feature");
			return false;
		}
		auto prop = vk.physical_device.getQueueFamilyProperties2<vk::StructureChain<vk::QueueFamilyProperties2, vk::QueueFamilyVideoPropertiesKHR>>();
		const auto flags = prop.at(vk.encode_queues[0].family_index).get<vk::QueueFamilyVideoPropertiesKHR>().videoCodecOperations;
		switch (codec)
		{
			case h264: {
				auto res = vk.has_device_ext(VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME) and flags & vk::VideoCodecOperationFlagBitsKHR::eEncodeH264;
				if (not res)
					U_LOG_I("GPU does not support H.264 Vulkan video encode");
				return res;
			}
			case h265: {
				auto res = vk.has_device_ext(VK_KHR_VIDEO_ENCODE_H265_EXTENSION_NAME) and flags & vk::VideoCodecOperationFlagBitsKHR::eEncodeH265;
				if (not res)
					U_LOG_I("GPU does not support H.265 Vulkan video encode");
				return res;
			}
			case av1:
				U_LOG_D("Vulkan video encode for AV1 is not implemented in WiVRn");
			case raw:
			case nxwarp:
				return false;
		}
		U_LOG_E("Invalid codec %d", int(codec));
		return false;
	}
#endif

public:
	prober(wivrn::vk_bundle & vk, const from_headset::headset_info_packet & info) :
	        vk(vk), info(info), nvidia(is_nvidia(vk.physical_device)) {}

	std::pair<std::string, video_codec> select_encoder(const configuration::encoder & config)
	{
		if (config.codec == video_codec::raw or config.name == encoder_raw)
			return {encoder_raw, video_codec::raw};

		// NX Warp is never auto-selected: it is picked only when the configuration
		// names it, because it is a CPU reference codec for now and would lose to
		// every hardware encoder on the machine. The headset still has to be able
		// to decode it — a stream the client cannot build a decoder for is a black
		// screen, and unlike a hardware codec there is no fallback the watchdog can
		// swap in (INTEGRATION-DECISIONS 6).
		if (config.codec == video_codec::nxwarp or config.name == encoder_nxwarp)
		{
#if WIVRN_USE_NXWARP
			if (not std::ranges::contains(info.supported_codecs, video_codec::nxwarp))
				U_LOG_W("nxwarp: the headset did not list this codec as supported; the stream will only work with a matching client");
			return {encoder_nxwarp, video_codec::nxwarp};
#else
			throw std::runtime_error("nxwarp encoder was requested but this server was built without it");
#endif
		}

#if WIVRN_USE_NVENC
		if ((nvidia and config.name.empty()) or config.name == encoder_nvenc)
		{
			for (auto codec: config.codec ? std::vector{*config.codec} : info.supported_codecs)
			{
				if (check_nvenc(codec))
					return {encoder_nvenc, codec};
			}
		}
#endif

#if WIVRN_USE_VULKAN_ENCODE
		if (config.name.empty() or config.name == encoder_vulkan)
		{
			for (auto codec: config.codec ? std::vector{*config.codec} : info.supported_codecs)
			{
				if (has_vk(codec))
					return {encoder_vulkan, codec};
			}
		}
#endif

#if WIVRN_USE_VAAPI
		if (config.name.empty() or config.name == encoder_vaapi)
		{
			for (auto codec: config.codec ? std::vector{*config.codec} : info.supported_codecs)
			{
				if (check_vaapi(codec))
					return {encoder_vaapi, codec};
			}
		}
#endif
		U_LOG_W("No suitable hardware accelerated codec found");
#if WIVRN_USE_X264
		if (config.name.empty() or config.name == encoder_x264)
			return {encoder_x264, video_codec::h264};
#endif

		throw std::runtime_error("Failed to find a suitable video encoder");
	}
};
} // namespace

static uint16_t align(uint16_t value, uint16_t alignment)
{
	return ((value + alignment - 1) / alignment) * alignment;
}

std::array<encoder_settings, num_streams> get_encoder_settings(wivrn::vk_bundle & bundle, wivrn_session & session)
{
	configuration config;

	std::array<wivrn::encoder_settings, num_streams> res{};
	// What each slot IS, before anything decides whether it is enabled. This is the
	// assignment that was missing: encoder_settings::role used to default to `view`
	// and nothing here ever set it, so the alpha plane went on the wire labelled as
	// an eye and the client waited for a picture it would never be sent.
	for (auto [i, dst]: std::ranges::enumerate_view(res))
		dst.role = default_stream_role(size_t(i));

	const auto & info = session.get_info();
	const auto settings = *session.get_settings();

	// The quad layer stream costs an encoder and a share of the bitrate for the
	// whole session, so it is only set up when the headset asked for it.
	res[quad_stream_idx].enabled = settings.quad_layers and config.quad_layers.max_size > 0;

	prober prober{bundle, info};

	for (auto [src, dst]: std::ranges::zip_view(config.encoders, res))
	{
		if (not dst.enabled)
			continue;
		dst.fps = session.default_fps();
		dst.options = src.options;
		dst.device = src.device;
		dst.sharp_text = settings.sharp_text;
		// The headset's nxvc decoder tool mask, for the NX Warp negotiation. It is a
		// property of the headset and not of the stream, so every stream gets the same
		// one; the encoder that does not code nxwarp ignores it.
		dst.nxvc_tools = info.nxvc_tools;
		dst.foveation_foveal_qp = settings.foveation_foveal_qp;
		// Both switches, like the encoder failover. The encoders that have a refresh
		// mechanism can only configure it when their encode session is created, which
		// is here, so this is the half that cannot be changed live.
		dst.intra_refresh = config.intra_refresh and settings.intra_refresh;
		// Same pair again, one rung down the ladder. Also fixed at creation: the encoders
		// that can invalidate have to be told at session setup to keep more than one
		// reference, or there is nothing older to fall back on.
		dst.ref_invalidation = config.ref_invalidation and settings.ref_invalidation;

		std::tie(dst.encoder_name, dst.codec) = prober.select_encoder(src);
	}

	// Reduced resolution streaming: the headset can ask for the eye images to be encoded at
	// a fraction of the stream eye size to save bitrate, encode and decode cost. The
	// foveation target and therefore the decoded image shrink by the same factor (the
	// foveation object below is built from this encode extent), while the headset keeps its
	// full defoveated/display resolution and reconstructs the difference when it samples the
	// decoded image (bilinear on its own, sharp with FSR). Clamped so a stray value can
	// never ask for a degenerate encode size. Only read here, so it applies on connection.
	//
	// The server has a ceiling of its own on the same quantity, "stream_scale": the NX Warp
	// decoder's per-tile cost scales with the pixel count and the headset serialises the two
	// eyes on its GPU, so trading a little sharpness for a smaller encode is what buys the
	// decoded frame rate back. Both are linear fractions of the same stream eye size, so they
	// compose as the smaller of the two, not as a product: the server value is a cap and a
	// headset that already asked for less keeps what it asked for. Both are read here only,
	// so both apply on connection.
	// stream_encode_size() is the whole derivation, kept pure and dependency free in
	// stream_scale.h so it can be unit tested without a Vulkan device or a session.
	const auto encode = stream_encode_size(info.stream_eye_width,
	                                       info.stream_eye_height,
	                                       settings.render_scale,
	                                       config.stream_scale);
	const float render_scale = encode.scale;
	auto width = encode.width;
	auto height = encode.height;
	if (config.stream_scale < 1.0f)
		U_LOG_I("nxwarp: stream 0 encodes %ux%u per eye (stream_scale %.3g, headset asked %ux%u)",
		        unsigned(width),
		        unsigned(height),
		        double(config.stream_scale),
		        unsigned(info.stream_eye_width),
		        unsigned(info.stream_eye_height));
	// Ensure we don't try to encode too large images (only for left/right, ignore alpha)
	for (size_t i = 0; i < 2; ++i)
		check_video_size(res[i].encoder_name, res[i].codec, width, height);

	for (auto [i, dst]: std::ranges::enumerate_view(res))
	{
		dst.width = width;
		dst.height = height;
		dst.encode_scale = render_scale;
		dst.src_layer = i;
		if (i == 2) // alpha channel
			dst.height /= 2;
	}

	// --- NX Warp: code both eyes as ONE stereo frame on stream 0.
	//
	// "stereo-frame": auto (the default) pairs the eyes when the whole pair is NX
	// Warp; on forces it; off keeps one stream and one encoder per eye. auto
	// declines a mixed pair because the win is a property of the pair -- the
	// headset's two decoders serialise, so pairing is what removes the second
	// dispatch, and pairing one eye with something that is not nxvc removes
	// nothing.
	//
	// The right-eye stream keeps its entry in the description, so the headset's
	// view-to-stream mapping is untouched and this needs no protocol change; what
	// it loses is its encoder. The client learns the pair is on from the .nxv
	// stream header it already parses (nxvc_vkd_stream_info::eyes == 2) and serves
	// view 1 out of stream 0's second eye.
	{
		const auto & opts = res[0].options;
		const auto it = opts.find("stereo-frame");
		const std::string mode = it == opts.end() ? "auto" : it->second;
		const bool both_nxwarp = res[0].enabled and res[1].enabled and
		                         res[0].codec == video_codec::nxwarp and
		                         res[1].codec == video_codec::nxwarp;
		bool pair = false;
		if (mode == "off" or mode == "0" or mode == "false")
			pair = false;
		else if (mode == "on" or mode == "1" or mode == "true")
			pair = both_nxwarp;
		else
			pair = both_nxwarp; // auto

		if (pair and mode != "off")
		{
			// nxvc refuses eyes == 2 unless the per-eye width is a multiple of
			// 64, so that the seam falls on a tile boundary and each eye's
			// sub-picture is addressable. stream_encode_size can land anywhere,
			// so this is a real gate and not an assertion.
			if (width % 64 != 0)
			{
				U_LOG_W("nxwarp: not pairing the eyes: the per-eye width %u is not a multiple of 64",
				        unsigned(width));
			}
			else
			{
				res[0].eyes = 2;
				res[0].src_layer = 0;
				res[0].src_layer_right = 1;
				res[1].enabled = false;
				U_LOG_I("nxwarp: both eyes on stream 0 as one %ux%u stereo frame (%u tiles), stream 1 has no encoder",
				        unsigned(width * 2),
				        unsigned(height),
				        unsigned(2 * ((width + 63) / 64) * ((height + 63) / 64)));
			}
		}
	}

	// --- The hybrid base layer, on the stream-1 slot the pairing just vacated.
	//
	// A hardware HEVC picture of the SAME side-by-side eye pair, decoded on the
	// headset's idle video ASIC and imported into the NX Warp atlas as
	// base-sourced patches (ADR-0029 Cheat 7, flags bit 2). It is not a second
	// view: it is never composited on its own, which is what `stream_role::base`
	// says on the wire.
	//
	// Every precondition here is the pairing's precondition, which is why this
	// slot is the right one and `num_streams = 6` buys nothing:
	//   * the pair must have paired, or stream 1 still holds the right eye's
	//     encoder and there is no slot;
	//   * the per-eye width is already a multiple of 64, so with CTB 64 every
	//     HEVC quantisation and deblocking boundary coincides with an nxvc tile
	//     boundary in BOTH eyes, with no per-eye offset correction;
	//   * both encoders want the identical side-by-side picture, so the compose
	//     is produced once and consumed twice.
	//
	// Off by default. It is a protocol-visible feature whose client half is an
	// atlas import that is still being built, so it is opted into explicitly and
	// nothing changes for anyone who does not.
	{
		const auto & opts = res[0].options;
		const auto it = opts.find("base-layer");
		const std::string mode = it == opts.end() ? "off" : it->second;
		const bool want = mode == "on" or mode == "1" or mode == "true";
		const bool paired = res[0].eyes == 2 and not res[1].enabled;

		if (want and not paired)
			U_LOG_W("hybrid: base layer requested but the eyes are not paired; stream 1 still carries the right eye");
		else if (want)
		{
			// Ask the prober for HEVC specifically. The base's whole reason to
			// exist is that the headset has an idle HEVC ASIC, so falling back to
			// H.264 or to x264 would be answering a different question -- an
			// unavailable HEVC encoder means no base layer, not a worse one.
			configuration::encoder want_h265;
			want_h265.codec = video_codec::h265;
			auto [name, codec] = prober.select_encoder(want_h265);
			if (codec != video_codec::h265)
			{
				U_LOG_W("hybrid: no HEVC encoder available for the base layer; not enabling it");
			}
			else
			{
				auto & base = res[1];
				base.enabled = true;
				base.role = stream_role::base;
				base.serves_stream = 0;
				base.encoder_name = name;
				base.codec = codec;
				base.fps = res[0].fps;
				base.device = res[0].device;
				// The base reads the SAME pair the nxwarp encoder does. `width`
				// and `height` stay per-eye here, exactly as they do on stream 0,
				// and `eyes == 2` is what makes the coded picture pair-wide -- so
				// there is one convention for a paired stream and not two.
				base.eyes = 2;
				base.src_layer = 0;
				base.src_layer_right = 1;
				base.encode_scale = res[0].encode_scale;
				// Loss recovery is the enhancement layer's job here, not the
				// base's: a lost base picture leaves the atlas stale and nxvc
				// refreshes the affected tiles per-tile with no IDR
				// (NXWARP-HYBRID.md §4.6). Keeping the base's own intra refresh
				// on top of that would spend base bitrate on a recovery that has
				// already happened.
				base.intra_refresh = false;
				base.ref_invalidation = res[0].ref_invalidation;
				// bit_depth is settled below, for every stream at once. An nxwarp
				// stream forces the whole session to 8 bits, and the base is on a
				// paired nxwarp stream by construction, so it gets the 8 bits §9
				// requires without asking for them here.
				// CTB 64, low latency, and FULL-RANGE BT.709 -- the last is not a
				// preference. The atlas's coded sample domain is BT.709 full-range
				// YCbCr because the nxwarp encoder leaves nxvc at CT_NONE, so a
				// limited-range base is a fixed offset on every base-sourced
				// patch: a failure that looks like a gamma bug and is not one.
				// NXWARP-HYBRID.md §9 is the derivation and hybrid-proto's
				// test_base_shadow is the guard.
				base.options = {
				        {"ctu", "64"},
				        {"bframes", "0"},
				        {"color_range", "pc"},
				        {"colorprim", "bt709"},
				        {"colormatrix", "bt709"},
				};
				U_LOG_I("hybrid: base layer on stream 1, %s HEVC, one %ux%u side-by-side pair, serving stream %u",
				        name.c_str(),
				        unsigned(width * 2),
				        unsigned(height),
				        unsigned(base.serves_stream));
			}
		}
	}

	if (res[quad_stream_idx].enabled)
	{
		// The quad has an image of its own, so its size is not tied to the eyes:
		// a square capped at the configured maximum, of which only the part
		// matching the panel's aspect ratio is filled on any given frame.
		auto & quad = res[quad_stream_idx];
		quad.width = align(config.quad_layers.max_size, 64);
		quad.height = quad.width;
		check_video_size(quad.encoder_name, quad.codec, quad.width, quad.height);
		quad.src_layer = 0;
	}

	auto bit_depth = config.bit_depth ? config.bit_depth : info.bit_depth;

	if (bit_depth and bit_depth != 8 and bit_depth != 10)
		throw std::runtime_error("invalid bit-depth setting. supported values: 8, 10");

	auto enabled_encoders = res | std::views::filter(&encoder_settings::enabled);
	if (std::ranges::contains(enabled_encoders, video_codec::h264, &encoder_settings::codec) or
	    std::ranges::contains(enabled_encoders, video_codec::raw, &encoder_settings::codec) or
	    // NX Warp v1 is an 8-bit bitstream: NXVC_TOOL_BITDEPTH10 is defined but not
	    // implemented by the reference codec, which rejects bit_depth 10 outright.
	    std::ranges::contains(enabled_encoders, video_codec::nxwarp, &encoder_settings::codec))
		bit_depth = 8;
	else if (not bit_depth)
		bit_depth = 10;

	auto check_format = [&](vk::Format format) {
		try
		{
			auto props = bundle.physical_device.getImageFormatProperties(
			        format,
			        vk::ImageType::e2D,
			        vk::ImageTiling::eOptimal,
			        vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc);
			return props.maxArrayLayers >= 3 and
			       props.maxExtent.depth >= 1 and
			       props.maxExtent.width >= width and
			       props.maxExtent.height >= height;
		}
		catch (vk::FormatNotSupportedError &)
		{
			return false;
		}
	};

#if WIVRN_USE_VAAPI
	auto check_vaapi = [&](int bit_depth) {
		for (const auto & encoder: res)
		{
			if (encoder.encoder_name == encoder_vaapi)
			{
				try
				{
					video_encoder_va{
					        bundle,
					        encoder_settings{
					                .width = 800,
					                .height = 800,
					                .codec = encoder.codec,
					                .fps = 60,
					                .bitrate = 50'000'000,
					                .bit_depth = bit_depth,
					        },
					        0};
				}
				catch (std::exception & e)
				{
					U_LOG_I("vaapi not supported for %s %d bits", std::string(magic_enum::enum_name(encoder.codec)).c_str(), bit_depth);
					return false;
				}
			}
		}
		return true;
	};
#else
	auto check_vaapi = [&](int) {
		return true;
	};
#endif

	if (bit_depth == 10 and not(check_format(vk::Format::eR16Unorm) and check_vaapi(*bit_depth)))
	{
		U_LOG_W("GPU does not have sufficient support for 10-bit images, reverting to 8");
		bit_depth = 8;
	}
	if (bit_depth == 8 and not check_format(vk::Format::eR8Unorm))
	{
		U_LOG_W("GPU does not have sufficient support for 8-bit images");
	}

	for (auto & i: res)
		i.bit_depth = bit_depth.value_or(10);

	split_bitrate(res, settings.bitrate_bps);

	// Last gate before these become the wire description. A wrong role does not fail
	// here, on the server, where it would be obvious: it fails on the headset, as a
	// stream scene that never starts, which reads as a broken client or a bad link.
	// So say it here, loudly, with the stream that is wrong.
	if (const auto bad = check_stream_roles(res); not bad.empty())
		U_LOG_E("stream roles are inconsistent: %s -- the headset will route this stream wrongly", bad.c_str());

	return res;
}
} // namespace wivrn
