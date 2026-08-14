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

#include "driver/configuration.h"
#include "driver/wivrn_session.h"
#include "util/u_logging.h"
#include "utils/wivrn_vk_bundle.h"
#include "video_encoder.h"
#include "wivrn_packets.h"

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

static void split_bitrate(std::array<wivrn::encoder_settings, num_streams> & encoders, uint64_t bitrate)
{
	assert(bitrate > 0);
	double total_weight = 0;
	for (auto [i, encoder]: std::ranges::enumerate_view(encoders))
	{
		if (not encoder.enabled)
		{
			encoder.bitrate = 0;
			encoder.bitrate_multiplier = 0;
			continue;
		}
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

	for (auto & encoder: encoders)
	{
		if (not encoder.enabled)
			continue;
		encoder.bitrate_multiplier = encoder.bitrate / total_weight;
		encoder.bitrate = encoder.bitrate_multiplier * bitrate;
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
		dst.foveation_foveal_qp = settings.foveation_foveal_qp;
		// Both switches, like the encoder failover. The encoders that have a refresh
		// mechanism can only configure it when their encode session is created, which
		// is here, so this is the half that cannot be changed live.
		dst.intra_refresh = config.intra_refresh and settings.intra_refresh;

		std::tie(dst.encoder_name, dst.codec) = prober.select_encoder(src);
	}

	// Reduced resolution streaming: the headset can ask for the eye images to be encoded at
	// a fraction of the stream eye size to save bitrate, encode and decode cost. The
	// foveation target and therefore the decoded image shrink by the same factor (the
	// foveation object below is built from this encode extent), while the headset keeps its
	// full defoveated/display resolution and reconstructs the difference when it samples the
	// decoded image (bilinear on its own, sharp with FSR). Clamped so a stray value can
	// never ask for a degenerate encode size. Only read here, so it applies on connection.
	const float render_scale = std::clamp(settings.render_scale, 0.5f, 1.0f);
	auto width = align(uint16_t(info.stream_eye_width * render_scale), 64);
	auto height = align(uint16_t(info.stream_eye_height * render_scale), 64);
	// Ensure we don't try to encode too large images (only for left/right, ignore alpha)
	for (size_t i = 0; i < 2; ++i)
		check_video_size(res[i].encoder_name, res[i].codec, width, height);

	for (auto [i, dst]: std::ranges::enumerate_view(res))
	{
		dst.width = width;
		dst.height = height;
		dst.src_layer = i;
		if (i == 2) // alpha channel
			dst.height /= 2;
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
	    std::ranges::contains(enabled_encoders, video_codec::raw, &encoder_settings::codec))
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
	return res;
}
} // namespace wivrn
