// base_shadow.h -- the encoder's conforming HEVC decode of its own base layer.
//
// ADR-0029 Cheat 7 Option B: "the encoder decodes its own base stream in
// hardware on the PC and its shadow atlas is exact". This is that decode.
//
// It is sound because the decode is normative: measured on the Pico 4,
// OMX.qcom.video.decoder.hevc and FFmpeg's HEVC decoder agree byte for byte on
// 180 of 180 frames across three bitrates (docs/NXWARP-HYBRID.md 3.3). So the
// frames this produces ARE the headset's frames, and a base-sourced patch the
// encoder models against them is modelled exactly.
//
// Deliberately software (AV_CODEC_ID_HEVC, no hwaccel): correctness here is the
// whole point and a hardware decoder buys 1-2 ms on a path that is not on the
// frame deadline -- the shadow is consumed by the NEXT frame's mode decision,
// not by this one. If that ever changes, the hwaccel is a drop-in because the
// output is normatively identical either way.
//
// libavcodec is already a server dependency (server/encoder/ffmpeg/), so this
// adds none.

#ifndef NXWARP_BASE_SHADOW_H
#define NXWARP_BASE_SHADOW_H

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

namespace wivrn {

// One decoded base picture, as three 8-bit planes at the CODED geometry --
// which for a paired stream is the eye pair side by side, eye 0 first.
struct base_shadow_frame
{
	int width = 0, height = 0;   // luma, the whole coded picture
	int cw = 0, ch = 0;          // chroma
	std::vector<uint8_t> y, cb, cr;
	int64_t pts = 0;
	bool valid = false;
};

class base_shadow_decoder
{
public:
	base_shadow_decoder()
	{
		const AVCodec * c = avcodec_find_decoder(AV_CODEC_ID_HEVC);
		if (not c)
			throw std::runtime_error("base_shadow: no HEVC decoder in libavcodec");
		ctx = avcodec_alloc_context3(c);
		if (not ctx)
			throw std::runtime_error("base_shadow: avcodec_alloc_context3 failed");
		// One thread, no delay: the shadow must come out in the order it went
		// in, and a frame-threaded decoder buffers several pictures before it
		// emits the first. That reordering would put the shadow behind the
		// encoder's own reference tracking, which is the one thing it must not
		// do.
		ctx->thread_count = 1;
		ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
		if (avcodec_open2(ctx, c, nullptr) < 0)
			throw std::runtime_error("base_shadow: avcodec_open2 failed");
		pkt = av_packet_alloc();
		frm = av_frame_alloc();
	}

	~base_shadow_decoder()
	{
		av_frame_free(&frm);
		av_packet_free(&pkt);
		avcodec_free_context(&ctx);
	}

	base_shadow_decoder(const base_shadow_decoder &) = delete;
	base_shadow_decoder & operator=(const base_shadow_decoder &) = delete;

	// Feed one Annex-B access unit; returns the picture it produced, if any.
	// With LOW_DELAY and a P-only stream this is one in, one out.
	base_shadow_frame decode(const uint8_t * au, size_t len, int64_t pts)
	{
		base_shadow_frame out;
		pkt->data = const_cast<uint8_t *>(au);
		pkt->size = int(len);
		pkt->pts = pts;
		int r = avcodec_send_packet(ctx, pkt);
		if (r < 0)
			return out;
		r = avcodec_receive_frame(ctx, frm);
		if (r < 0)
			return out;
		copy_out(out);
		av_frame_unref(frm);
		return out;
	}

	// Drain at end of stream.
	base_shadow_frame flush()
	{
		base_shadow_frame out;
		avcodec_send_packet(ctx, nullptr);
		if (avcodec_receive_frame(ctx, frm) < 0)
			return out;
		copy_out(out);
		av_frame_unref(frm);
		return out;
	}

	// What the base layer must be encoded as for its samples to BE the atlas's
	// numbers, with no conversion (docs/NXWARP-HYBRID.md 9). Checked rather
	// than assumed, because getting it wrong looks like a gamma bug: a limited
	// -range base makes every patch washed out by a fixed offset.
	std::string check_colour() const
	{
		std::string bad;
		if (ctx->color_range != AVCOL_RANGE_JPEG)
			bad += "range is not full (pc/JPEG); ";
		if (ctx->colorspace != AVCOL_SPC_UNSPECIFIED and ctx->colorspace != AVCOL_SPC_BT709)
			bad += "matrix is not BT.709; ";
		// A full-range stream comes back as the deprecated YUVJ420P rather than
		// YUV420P; both are 8-bit planar 4:2:0 and both are correct here.
		if (ctx->pix_fmt != AV_PIX_FMT_YUV420P and
		    ctx->pix_fmt != AV_PIX_FMT_YUVJ420P and
		    ctx->pix_fmt != AV_PIX_FMT_NONE)
			bad += "pixel format is not 8-bit planar 4:2:0; ";
		return bad;
	}

private:
	void copy_out(base_shadow_frame & out)
	{
		out.width = frm->width;
		out.height = frm->height;
		out.cw = (frm->width + 1) / 2;
		out.ch = (frm->height + 1) / 2;
		out.pts = frm->pts;
		out.y.resize(size_t(out.width) * out.height);
		out.cb.resize(size_t(out.cw) * out.ch);
		out.cr.resize(size_t(out.cw) * out.ch);
		for (int r = 0; r < out.height; r++)
			memcpy(out.y.data() + size_t(r) * out.width,
			       frm->data[0] + size_t(r) * frm->linesize[0], out.width);
		for (int r = 0; r < out.ch; r++)
		{
			memcpy(out.cb.data() + size_t(r) * out.cw,
			       frm->data[1] + size_t(r) * frm->linesize[1], out.cw);
			memcpy(out.cr.data() + size_t(r) * out.cw,
			       frm->data[2] + size_t(r) * frm->linesize[2], out.cw);
		}
		out.valid = true;
	}

	AVCodecContext * ctx = nullptr;
	AVPacket * pkt = nullptr;
	AVFrame * frm = nullptr;
};

} // namespace wivrn

#endif
