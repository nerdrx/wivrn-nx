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

#include "mirror_view.h"

#include "wivrn_config.h"

#include <KLocalizedString>
#include <QDebug>
#include <QSize>
#include <QVideoFrameFormat>
#include <QVideoSink>

#if WIVRN_USE_PIPEWIRE
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/utils/string.h>
#endif

namespace
{
//! Node published by the server, see server/compositor/pipewire_mirror.cpp
const char * const node_name = "wivrn-headset-view";

#if WIVRN_USE_PIPEWIRE
//! Frames the GUI thread has not consumed yet before the PipeWire thread drops new ones
const int max_frames_in_flight = 2;

/*!
 * The server publishes SPA_VIDEO_FORMAT_RGBx out of a VK_FORMAT_R8G8B8A8_UNORM
 * image, so the bytes are R, G, B, unused, in that order in memory.
 *
 * QVideoFrameFormat names are byte order too, not word order: filling a frame
 * with the byte sequence 10 20 30 ff and converting it to a QImage gives
 * #102030 for Format_RGBX8888 (and for Format_RGBA8888), while Format_XBGR8888
 * gives #ff3020. Format_RGBX8888 is therefore the memcpy-compatible one.
 */
constexpr QVideoFrameFormat::PixelFormat qt_pixel_format = QVideoFrameFormat::Format_RGBX8888;
constexpr uint32_t bytes_per_pixel = 4;
#endif
} // namespace

#if WIVRN_USE_PIPEWIRE

struct MirrorView::impl
{
	MirrorView & self;

	pw_thread_loop * loop = nullptr;
	pw_context * context = nullptr;
	pw_core * core = nullptr;
	pw_registry * registry = nullptr;
	spa_hook registry_listener{};

	// Owned by the GUI thread: only created and destroyed there, under the loop lock
	pw_stream * stream = nullptr;
	spa_hook stream_listener{};

	// PipeWire thread only
	uint32_t node_id = SPA_ID_INVALID;
	uint32_t width = 0;
	uint32_t height = 0;
	bool warned_short_buffer = false;

	std::atomic<int> frames_in_flight{0};

	static const pw_registry_events registry_events;
	static const pw_stream_events stream_events;

	explicit impl(MirrorView & self) :
	        self(self) {}

	~impl()
	{
		// Stop the thread first: after this no callback can run, so the
		// remaining teardown does not race with one.
		if (loop)
			pw_thread_loop_stop(loop);

		destroy_stream();

		if (registry)
		{
			spa_hook_remove(&registry_listener);
			pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry));
			registry = nullptr;
		}
		if (core)
		{
			pw_core_disconnect(core);
			core = nullptr;
		}
		if (context)
		{
			pw_context_destroy(context);
			context = nullptr;
		}
		if (loop)
		{
			pw_thread_loop_destroy(loop);
			loop = nullptr;
		}
	}

	//! Post a call back to the thread MirrorView lives in.
	template <typename F>
	void to_gui(F && f)
	{
		// The context object is &self, so Qt drops the pending calls when it
		// is destroyed and runs them in its own thread.
		QMetaObject::invokeMethod(&self, std::forward<F>(f), Qt::QueuedConnection);
	}

	//! GUI thread, with the loop locked
	void create_stream()
	{
		if (stream)
			return;

		pw_properties * props = pw_properties_new(
		        PW_KEY_MEDIA_TYPE,
		        "Video",
		        PW_KEY_MEDIA_CATEGORY,
		        "Capture",
		        PW_KEY_MEDIA_ROLE,
		        "Camera",
		        // Link to the server's node by name rather than by id: the id
		        // changes every time the session restarts.
		        PW_KEY_TARGET_OBJECT,
		        node_name,
		        PW_KEY_NODE_NAME,
		        "wivrn-dashboard-mirror",
		        NULL);

		stream = pw_stream_new(core, "WiVRn dashboard mirror", props);
		if (not stream)
			return;

		width = 0;
		height = 0;
		warned_short_buffer = false;

		pw_stream_add_listener(stream, &stream_listener, &stream_events, this);

		uint8_t pod_buffer[1024];
		spa_pod_builder b = SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));

		// The server publishes a single fixed size, accept anything sane
		spa_rectangle size_def{1280, 720};
		spa_rectangle size_min{1, 1};
		spa_rectangle size_max{16384, 16384};
		spa_fraction rate_def{30, 1};
		spa_fraction rate_min{0, 1};
		spa_fraction rate_max{1000, 1};

		const spa_pod * params[1];
		params[0] = reinterpret_cast<const spa_pod *>(spa_pod_builder_add_object(
		        &b,
		        SPA_TYPE_OBJECT_Format,
		        SPA_PARAM_EnumFormat,
		        SPA_FORMAT_mediaType,
		        SPA_POD_Id(SPA_MEDIA_TYPE_video),
		        SPA_FORMAT_mediaSubtype,
		        SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		        SPA_FORMAT_VIDEO_format,
		        SPA_POD_Id(SPA_VIDEO_FORMAT_RGBx),
		        SPA_FORMAT_VIDEO_size,
		        SPA_POD_CHOICE_RANGE_Rectangle(&size_def, &size_min, &size_max),
		        SPA_FORMAT_VIDEO_framerate,
		        SPA_POD_CHOICE_RANGE_Fraction(&rate_def, &rate_min, &rate_max)));

		if (pw_stream_connect(
		            stream,
		            PW_DIRECTION_INPUT,
		            PW_ID_ANY,
		            pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
		            params,
		            1) < 0)
		{
			qWarning() << "Mirror: failed to connect the PipeWire stream";
			destroy_stream();
		}
	}

	//! GUI thread with the loop locked, or the destructor once the loop is stopped
	void destroy_stream()
	{
		if (not stream)
			return;
		spa_hook_remove(&stream_listener);
		pw_stream_destroy(stream);
		stream = nullptr;
	}

	static void on_registry_global(void * data, uint32_t id, uint32_t, const char * type, uint32_t, const spa_dict * props)
	{
		auto self = static_cast<impl *>(data);
		if (not props or not spa_streq(type, PW_TYPE_INTERFACE_Node))
			return;
		const char * name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
		if (not name or not spa_streq(name, node_name))
			return;

		self->node_id = id;
		self->to_gui([&mirror = self->self]() { mirror.set_available(true); });
	}

	static void on_registry_global_remove(void * data, uint32_t id)
	{
		auto self = static_cast<impl *>(data);
		if (id != self->node_id)
			return;

		self->node_id = SPA_ID_INVALID;
		self->to_gui([&mirror = self->self]() { mirror.set_available(false); });
	}

	static void on_state_changed(void * data, pw_stream_state, pw_stream_state state, const char * error)
	{
		auto self = static_cast<impl *>(data);
		if (error)
			qWarning() << "Mirror: PipeWire stream error:" << error;

		bool streaming = state == PW_STREAM_STATE_STREAMING;
		self->to_gui([&mirror = self->self, streaming]() { mirror.set_active(streaming); });
	}

	static void on_param_changed(void * data, uint32_t id, const spa_pod * param)
	{
		auto self = static_cast<impl *>(data);
		if (id != SPA_PARAM_Format or param == nullptr)
		{
			if (id == SPA_PARAM_Format)
			{
				self->width = 0;
				self->height = 0;
			}
			return;
		}

		spa_video_info info{};
		if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0)
			return;
		if (info.media_type != SPA_MEDIA_TYPE_video or info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
			return;
		if (spa_format_video_raw_parse(param, &info.info.raw) < 0)
			return;
		if (info.info.raw.format != SPA_VIDEO_FORMAT_RGBx)
		{
			qWarning() << "Mirror: unexpected negotiated format" << int(info.info.raw.format);
			return;
		}

		self->width = info.info.raw.size.width;
		self->height = info.info.raw.size.height;
		self->warned_short_buffer = false;

		uint8_t pod_buffer[512];
		spa_pod_builder b = SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
		const spa_pod * params[1];
		// The server sizes the buffers, only say which memory types can be mapped
		params[0] = reinterpret_cast<const spa_pod *>(spa_pod_builder_add_object(
		        &b,
		        SPA_TYPE_OBJECT_ParamBuffers,
		        SPA_PARAM_Buffers,
		        SPA_PARAM_BUFFERS_dataType,
		        SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr))));
		pw_stream_update_params(self->stream, params, 1);
	}

	static void on_process(void * data)
	{
		auto self = static_cast<impl *>(data);
		pw_buffer * b = pw_stream_dequeue_buffer(self->stream);
		if (not b)
			return;
		self->handle_buffer(b);
		pw_stream_queue_buffer(self->stream, b);
	}

	//! PipeWire thread
	void handle_buffer(pw_buffer * b)
	{
		if (width == 0 or height == 0)
			return;

		spa_buffer * buf = b->buffer;
		if (buf->n_datas < 1)
			return;

		spa_data & data = buf->datas[0];
		if (not data.data or not data.chunk or data.chunk->size == 0)
			return;

		const uint32_t row_bytes = width * bytes_per_pixel;
		const uint32_t src_stride = data.chunk->stride > 0 ? uint32_t(data.chunk->stride) : row_bytes;
		if (src_stride < row_bytes)
			return;

		if (uint64_t(data.chunk->offset) + uint64_t(data.chunk->size) > data.maxsize or
		    uint64_t(data.chunk->size) < uint64_t(src_stride) * (height - 1) + row_bytes)
		{
			if (not warned_short_buffer)
			{
				warned_short_buffer = true;
				qWarning() << "Mirror: buffer too small for" << width << "x" << height;
			}
			return;
		}

		// Do not let the GUI thread fall behind: it is cheaper to drop here than
		// to queue frames it will never draw.
		if (frames_in_flight.load(std::memory_order_relaxed) >= max_frames_in_flight)
			return;

		// The PipeWire buffer is recycled as soon as this returns, so the frame
		// gets its own storage.
		QVideoFrame frame{QVideoFrameFormat{QSize(int(width), int(height)), qt_pixel_format}};
		if (not frame.map(QVideoFrame::WriteOnly))
			return;

		auto src = static_cast<const uint8_t *>(data.data) + data.chunk->offset;
		uint8_t * dst = frame.bits(0);
		const int dst_stride = frame.bytesPerLine(0);
		const bool copied = dst and dst_stride >= int(row_bytes);
		if (copied)
		{
			for (uint32_t y = 0; y < height; ++y)
				memcpy(dst + size_t(y) * dst_stride, src + size_t(y) * src_stride, row_bytes);
		}
		frame.unmap();

		if (not copied)
			return;

		frames_in_flight.fetch_add(1, std::memory_order_relaxed);
		to_gui([this, frame]() {
			frames_in_flight.fetch_sub(1, std::memory_order_relaxed);
			self.present_frame(frame);
		});
	}
};

const pw_registry_events MirrorView::impl::registry_events = {
        .version = PW_VERSION_REGISTRY_EVENTS,
        .global = &MirrorView::impl::on_registry_global,
        .global_remove = &MirrorView::impl::on_registry_global_remove,
};

const pw_stream_events MirrorView::impl::stream_events = {
        .version = PW_VERSION_STREAM_EVENTS,
        .state_changed = &MirrorView::impl::on_state_changed,
        .param_changed = &MirrorView::impl::on_param_changed,
        .process = &MirrorView::impl::on_process,
};

#else // WIVRN_USE_PIPEWIRE

struct MirrorView::impl
{};

#endif

MirrorView::MirrorView(QObject * parent) :
        QObject(parent)
{
#if WIVRN_USE_PIPEWIRE
	static std::once_flag pw_initialized;
	std::call_once(pw_initialized, []() {
		int argc = 0;
		pw_init(&argc, nullptr);
	});

	auto d2 = std::make_unique<impl>(*this);

	d2->loop = pw_thread_loop_new("wivrn-dashboard-mirror", nullptr);
	if (d2->loop)
		d2->context = pw_context_new(pw_thread_loop_get_loop(d2->loop), nullptr, 0);
	if (d2->context)
		d2->core = pw_context_connect(d2->context, nullptr, 0);

	if (not d2->core)
	{
		m_error = i18n("Cannot connect to PipeWire");
		qWarning() << "Mirror: cannot connect to PipeWire";
		return;
	}

	// Created before the loop is started, so no locking is needed yet
	d2->registry = pw_core_get_registry(d2->core, PW_VERSION_REGISTRY, 0);
	if (d2->registry)
		pw_registry_add_listener(d2->registry, &d2->registry_listener, &impl::registry_events, d2.get());

	if (pw_thread_loop_start(d2->loop) < 0)
	{
		m_error = i18n("Cannot start the PipeWire thread");
		qWarning() << "Mirror: cannot start the PipeWire thread";
		return;
	}

	d = std::move(d2);
	m_supported = true;
#else
	m_error = i18n("This build has no PipeWire support");
#endif
}

MirrorView::~MirrorView()
{
	// ~impl stops the PipeWire thread before anything else is torn down
	d.reset();
}

void MirrorView::setVideoSink(QVideoSink * sink)
{
	if (m_sink == sink)
		return;

	if (m_sink)
		m_sink->setVideoFrame(QVideoFrame());

	m_sink = sink;

	if (m_sink and m_last_frame.isValid())
		m_sink->setVideoFrame(m_last_frame);

	Q_EMIT videoSinkChanged();
}

void MirrorView::start()
{
	if (m_wanted)
		return;
	m_wanted = true;
	reconcile();
}

void MirrorView::stop()
{
	if (not m_wanted)
		return;
	m_wanted = false;
	reconcile();

	m_last_frame = QVideoFrame();
	if (m_sink)
		m_sink->setVideoFrame(QVideoFrame());
	set_active(false);
}

void MirrorView::reconcile()
{
#if WIVRN_USE_PIPEWIRE
	if (not d)
		return;

	const bool want_stream = m_wanted and m_available;

	pw_thread_loop_lock(d->loop);
	if (want_stream)
		d->create_stream();
	else
		d->destroy_stream();
	pw_thread_loop_unlock(d->loop);

	if (not want_stream)
		set_active(false);
#endif
}

void MirrorView::set_active(bool value)
{
	if (m_active == value)
		return;
	m_active = value;
	Q_EMIT activeChanged();
}

void MirrorView::set_available(bool value)
{
	if (m_available == value)
		return;
	m_available = value;
	reconcile();
	Q_EMIT availableChanged();
}

void MirrorView::present_frame(const QVideoFrame & frame)
{
	if (not m_wanted)
		return;
	m_last_frame = frame;
	if (m_sink)
		m_sink->setVideoFrame(frame);
}

#include "moc_mirror_view.cpp"
