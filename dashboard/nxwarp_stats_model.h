/*
 * WiVRn VR streaming
 * Copyright (C) 2025  WiVRn contributors
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

// One NX Warp stream's two-second report, as QML sees it.
//
// A Q_GADGET value type over wivrn::nxwarp_stream_stats, in the same shape as field_of_view: the
// server publishes the numbers as JSON on the NxwarpStats property, wivrn_server parses them, and
// the status page binds to these properties.
//
// The derived fields (fpsSent, tiles, the two "..Text" strings) are computed here rather than in
// the QML because they are statements about the data, not about the layout, and because a status
// page should not be doing arithmetic in a binding.

#include "nxwarp_stats.h"

#include <QObject>
#include <QQmlEngine>
#include <QString>

struct nxwarp_stream_stat
{
	Q_GADGET

	Q_PROPERTY(int streamIndex READ streamIndex CONSTANT)
	Q_PROPERTY(double windowSeconds READ windowSeconds CONSTANT)

	// How many frames a second actually left the encoder, and how many it was aiming to
	// send. The gap between them is the pacing.
	Q_PROPERTY(double fpsSent READ fpsSent CONSTANT)
	Q_PROPERTY(double pacedFps READ pacedFps CONSTANT)
	Q_PROPERTY(QString paceModeText READ paceModeText CONSTANT)
	// Zero until the headset has reported one.
	Q_PROPERTY(double clientDecodeMs READ clientDecodeMs CONSTANT)
	Q_PROPERTY(bool clientDecodeKnown READ clientDecodeKnown CONSTANT)
	Q_PROPERTY(double framesNotSent READ framesNotSent CONSTANT)

	Q_PROPERTY(double encodeMsMean READ encodeMsMean CONSTANT)
	Q_PROPERTY(double encodeMsMax READ encodeMsMax CONSTANT)

	Q_PROPERTY(double bytesPerFrame READ bytesPerFrame CONSTANT)
	Q_PROPERTY(double targetBytesPerFrame READ targetBytesPerFrame CONSTANT)
	// Signed percentage the achieved size misses its target by. Meaningless with a fixed
	// quantiser, where there is no target.
	Q_PROPERTY(double bytesOffTargetPercent READ bytesOffTargetPercent CONSTANT)
	Q_PROPERTY(bool hasTarget READ hasTarget CONSTANT)

	Q_PROPERTY(double qpMean READ qpMean CONSTANT)
	Q_PROPERTY(int qpMin READ qpMin CONSTANT)
	Q_PROPERTY(int qpMax READ qpMax CONSTANT)
	Q_PROPERTY(bool rcAuto READ rcAuto CONSTANT)
	// The configured quantiser band cannot reach the bitrate the controller was given: the
	// one state on this page that is a fault rather than a measurement.
	Q_PROPERTY(bool rcUnreachable READ rcUnreachable CONSTANT)
	Q_PROPERTY(double controllerMbps READ controllerMbps CONSTANT)

	Q_PROPERTY(double notReconstructed READ notReconstructed CONSTANT)
	Q_PROPERTY(double notReconstructedCostly READ notReconstructedCostly CONSTANT)
	Q_PROPERTY(QString dominantReasonText READ dominantReasonText CONSTANT)
	Q_PROPERTY(double dominantReasonCount READ dominantReasonCount CONSTANT)

	Q_PROPERTY(QString entropy READ entropy CONSTANT)
	Q_PROPERTY(bool entropyWasAuto READ entropyWasAuto CONSTANT)
	Q_PROPERTY(QString toolsText READ toolsText CONSTANT)

	// Whether this stream carries both eyes as one stereo frame. The page turns this into
	// the card's title; it stays a boolean here so the wording goes through i18n in the
	// QML rather than being an untranslated string baked into C++.
	Q_PROPERTY(bool paired READ paired CONSTANT)
	// The frame actually coded and its tile count: the pair side by side when paired,
	// one eye otherwise. This is what one decode dispatch on the headset costs.
	Q_PROPERTY(int codedFrameWidth READ codedFrameWidth CONSTANT)
	Q_PROPERTY(int codedFrameTiles READ codedFrameTiles CONSTANT)
	Q_PROPERTY(int encodedWidth READ encodedWidth CONSTANT)
	Q_PROPERTY(int encodedHeight READ encodedHeight CONSTANT)
	Q_PROPERTY(int tiles READ tiles CONSTANT)
	Q_PROPERTY(double encodeScale READ encodeScale CONSTANT)
	// How this window's frames were laid on the transport's tile grid. Counts and not a
	// mode, because the choice is per frame: "auto" can produce all spans, all chunks or
	// a mix, and a reader that showed the SETTING would be showing something that is not
	// necessarily what happened.
	Q_PROPERTY(int spanFrames READ spanFrames CONSTANT)
	Q_PROPERTY(int chunkFrames READ chunkFrames CONSTANT)

	QML_VALUE_TYPE(nxwarp_stream_stat)

public:
	wivrn::nxwarp_stream_stats s;

	int streamIndex() const
	{
		return s.stream_index;
	}
	double windowSeconds() const
	{
		return s.window_seconds;
	}
	double fpsSent() const
	{
		return s.fps_sent();
	}
	double pacedFps() const
	{
		return s.paced_fps;
	}
	QString paceModeText() const
	{
		switch (s.pace_mode)
		{
			case wivrn::nxwarp_pace_report::off:
				return QStringLiteral("off");
			case wivrn::nxwarp_pace_report::fixed:
				return QStringLiteral("fixed");
			case wivrn::nxwarp_pace_report::automatic:
				break;
		}
		return QStringLiteral("auto");
	}
	double clientDecodeMs() const
	{
		return s.client_decode_ms;
	}
	bool clientDecodeKnown() const
	{
		return s.client_decode_ms > 0;
	}
	double framesNotSent() const
	{
		return double(s.frames_not_sent);
	}
	double encodeMsMean() const
	{
		return s.encode_ms_mean;
	}
	double encodeMsMax() const
	{
		return s.encode_ms_max;
	}
	double bytesPerFrame() const
	{
		return s.bytes_per_frame;
	}
	double targetBytesPerFrame() const
	{
		return s.target_bytes_per_frame;
	}
	bool hasTarget() const
	{
		return s.rc_auto and s.target_bytes_per_frame > 0;
	}
	double bytesOffTargetPercent() const
	{
		if (not hasTarget())
			return 0;
		return 100.0 * (s.bytes_per_frame - s.target_bytes_per_frame) / s.target_bytes_per_frame;
	}
	double qpMean() const
	{
		return s.qp_mean;
	}
	int qpMin() const
	{
		return s.qp_min;
	}
	int qpMax() const
	{
		return s.qp_max;
	}
	bool rcAuto() const
	{
		return s.rc_auto;
	}
	bool rcUnreachable() const
	{
		return s.rc_unreachable;
	}
	double controllerMbps() const
	{
		return double(s.controller_bitrate_bps) * 1e-6;
	}
	double notReconstructed() const
	{
		return double(s.not_reconstructed);
	}
	double notReconstructedCostly() const
	{
		return double(s.not_reconstructed_costly);
	}
	QString dominantReasonText() const
	{
		return QString::fromUtf8(wivrn::nxwarp_not_held_reason_name(s.dominant_reason));
	}
	double dominantReasonCount() const
	{
		return double(s.dominant_reason_count);
	}
	QString entropy() const
	{
		return QString::fromStdString(s.entropy);
	}
	bool entropyWasAuto() const
	{
		return s.entropy_was_auto;
	}
	QString toolsText() const
	{
		if (s.negotiated_tools == 0)
			return QStringLiteral("none reported");
		return QStringLiteral("0x%1").arg(s.negotiated_tools, 0, 16);
	}
	bool paired() const
	{
		return s.paired();
	}
	int codedFrameWidth() const
	{
		return int(s.coded_frame_width());
	}
	int codedFrameTiles() const
	{
		return int(s.coded_frame_tiles());
	}
	int encodedWidth() const
	{
		return s.encoded_width;
	}
	int encodedHeight() const
	{
		return s.encoded_height;
	}
	int tiles() const
	{
		return int(s.tiles());
	}
	int spanFrames() const
	{
		return int(s.span_frames);
	}
	int chunkFrames() const
	{
		return int(s.chunk_frames);
	}
	double encodeScale() const
	{
		return s.encode_scale;
	}
};
