/*
 * WiVRn VR streaming
 * Copyright (C) 2026  Patrick Nicolas <patricknicolas@laposte.net>
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
#include <vulkan/vulkan_raii.hpp>

namespace wivrn
{
struct vk_bundle;

/*!
 * Desktop mirror of the headset view.
 *
 * Publishes one eye of the composited view as a PipeWire video source node, so
 * that the frames the headset is about to receive can be displayed or recorded
 * by any PipeWire consumer.
 *
 * The frames are taken in the compositor, after layer composition but before
 * foveation, so what is published is the clean undistorted view.
 */
class pipewire_mirror
{
public:
	//! One eye, as it exists in the compositor command buffer before foveation.
	struct source
	{
		//! Sampler-compatible view of the composited eye, sRGB, in
		//! eShaderReadOnlyOptimal on the compositor queue family, and
		//! valid until the current submission completes.
		vk::ImageView view;
		//! Region of the view holding the eye, in texels. Extents may be
		//! negative, in which case the axis is mirrored.
		int32_t x = 0;
		int32_t y = 0;
		int32_t width = 0;
		int32_t height = 0;
		//! Source is rendered upside down (GL clients)
		bool flip_y = false;
	};

	virtual ~pipewire_mirror();

	/*!
	 * Record the capture of @p src into @p cmd.
	 *
	 * Does nothing and returns false when no consumer is connected, when the
	 * frame is not due yet for the configured frame rate, or when the previous
	 * capture has not been consumed. When it returns true, submitted() must be
	 * called with the timeline semaphore value that the submission of @p cmd
	 * signals.
	 */
	virtual bool capture(vk::raii::CommandBuffer & cmd, const source & src) = 0;

	//! Hand the capture recorded by the last successful capture() to the reader thread.
	virtual void submitted(vk::Semaphore, uint64_t value) = 0;

	//! Returns nullptr if the mirror could not be set up.
	static std::unique_ptr<pipewire_mirror> create(vk_bundle &, vk::Extent2D size, int fps);
};

} // namespace wivrn
