/*
 * WiVRn VR streaming
 * Copyright (C) 2025  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "decoder.h"

#include <stdexcept>

#ifdef __ANDROID__
#include "decoder/android/android_decoder.h"
#else
#include "decoder/ffmpeg/ffmpeg_decoder.h"
#endif
#include "decoder/nxwarp/nxwarp_decoder.h"
#include "decoder/raw_decoder.h"

wivrn::decoder::~decoder() = default;

std::shared_ptr<wivrn::decoder> wivrn::decoder::make(
        vk::raii::Device & device,
        vk::raii::PhysicalDevice & phys_dev,
        uint32_t vk_queue_family_index,
        const wivrn::to_headset::video_stream_description & description,
        uint8_t stream_index,
        std::weak_ptr<scenes::stream> scene,
        shard_accumulator * acc)
{
	switch (description.codec[stream_index])
	{
		case h264:
		case h265:
		case av1:
#ifdef __ANDROID__
			return std::make_shared<wivrn::android::decoder>(
			        device,
			        phys_dev,
			        description,
			        stream_index,
			        scene,
			        acc);
#else
			return std::make_shared<wivrn::ffmpeg::decoder>(
			        device,
			        phys_dev,
			        description,
			        stream_index,
			        scene,
			        acc);
#endif
		case nxwarp:
#include "wivrn_config.h"

#if WIVRN_USE_NXWARP
			return std::make_shared<wivrn::nxwarp_decoder>(
			        device,
			        phys_dev,
			        vk_queue_family_index,
			        description,
			        stream_index,
			        scene,
			        acc);
#else
			throw std::runtime_error("this client was built without NX Warp support");
#endif
		case raw:
			return std::make_shared<wivrn::raw_decoder>(
			        device,
			        phys_dev,
			        vk_queue_family_index,
			        description,
			        stream_index,
			        scene,
			        acc);
	}
	__builtin_unreachable();
}

static std::vector<wivrn::video_codec> supported_codecs_()
{
	std::vector<wivrn::video_codec> res;
#ifdef __ANDROID__
	wivrn::android::decoder::supported_codecs(res);
#else
	wivrn::ffmpeg::decoder::supported_codecs(res);
#endif
#if WIVRN_USE_NXWARP
	// NX Warp is a pure-compute decoder: there is no hardware capability to probe, only a
	// Vulkan device that either runs it or does not, and that device does not exist yet
	// when this list is built. It is advertised last so that a server which supports it
	// still prefers the hardware codecs unless the headset setting moves it up; see
	// configuration::nxwarp and scenes::stream's headset_info.
	res.push_back(wivrn::video_codec::nxwarp);
#endif
	res.push_back(wivrn::video_codec::raw);
	return res;
}

const std::vector<wivrn::video_codec> & wivrn::decoder::supported_codecs()
{
	static std::vector<wivrn::video_codec> res = supported_codecs_();
	return res;
}
