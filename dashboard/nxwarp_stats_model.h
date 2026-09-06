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
	// Where that decode time goes on the headset. passB is the ENVELOPE and the five
	// below are what it is made of -- warp, the three reconstruction segments, and the
	// drain between dispatches that no segment timer covers. They sum to it by
	// construction, because "other" is defined as the remainder.
	Q_PROPERTY(bool passSegmentsKnown READ passSegmentsKnown CONSTANT)
	Q_PROPERTY(double passAMs READ passAMs CONSTANT)
	Q_PROPERTY(double passBMs READ passBMs CONSTANT)
	Q_PROPERTY(double passWMs READ passWMs CONSTANT)
	Q_PROPERTY(double passBSkipMs READ passBSkipMs CONSTANT)
	Q_PROPERTY(double passBCodedMs READ passBCodedMs CONSTANT)
	Q_PROPERTY(double passBDirMs READ passBDirMs CONSTANT)
	Q_PROPERTY(double passBOtherMs READ passBOtherMs CONSTANT)
	Q_PROPERTY(double tilesSkip READ tilesSkip CONSTANT)
	Q_PROPERTY(double tilesCoded READ tilesCoded CONSTANT)
	Q_PROPERTY(double tilesDir READ tilesDir CONSTANT)
	// The share of the envelope the skipped-tile warp accounts for, as a percentage.
	// It is the one number that answers "is the headset slow, or is it just warping" --
	// which is the question the envelope alone could not be asked.
	Q_PROPERTY(double skipSharePct READ skipSharePct CONSTANT)
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

	Q_PROPERTY(int effort READ effort CONSTANT)
	Q_PROPERTY(int snapIdentity READ snapIdentity CONSTANT)
	Q_PROPERTY(QString identityTiles READ identityTiles CONSTANT)
	Q_PROPERTY(QString rateBinding READ rateBinding CONSTANT)
	Q_PROPERTY(bool identityFromDecoder READ identityFromDecoder CONSTANT)
	Q_PROPERTY(QString planar READ planar CONSTANT)
	Q_PROPERTY(QString planarNote READ planarNote CONSTANT)
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
	// The lens mask: tiles per eye the optics cannot show. `lensMaskEnforced` is the
	// honest half -- false means the mask is known and the pixels are flattened, but this
	// backend has no way to be told to skip them.
	Q_PROPERTY(bool lensMaskOn READ lensMaskOn CONSTANT)
	Q_PROPERTY(bool lensMaskEnforced READ lensMaskEnforced CONSTANT)
	Q_PROPERTY(int lensMaskMasked READ lensMaskMasked CONSTANT)
	Q_PROPERTY(int lensMaskTiles READ lensMaskTiles CONSTANT)
	Q_PROPERTY(int lensMaskMargin READ lensMaskMargin CONSTANT)
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
	bool passSegmentsKnown() const
	{
		// The envelope has to be real too: a headset whose nxvc has the timers but
		// whose device cannot stamp them reports segments_known with everything at
		// zero, and a card of noughts is worse than no card.
		return s.client_pass_segments_known and s.client_pass_b_ms > 0;
	}
	double passAMs() const
	{
		return s.client_pass_a_ms;
	}
	double passBMs() const
	{
		return s.client_pass_b_ms;
	}
	double passWMs() const
	{
		return s.client_pass_w_ms;
	}
	double passBSkipMs() const
	{
		return s.client_pass_b_skip_ms;
	}
	double passBCodedMs() const
	{
		return s.client_pass_b_coded_ms;
	}
	double passBDirMs() const
	{
		return s.client_pass_b_dir_ms;
	}
	double passBOtherMs() const
	{
		return s.client_pass_b_other_ms();
	}
	double tilesSkip() const
	{
		return s.client_tiles_skip;
	}
	double tilesCoded() const
	{
		return s.client_tiles_coded;
	}
	double tilesDir() const
	{
		return s.client_tiles_dir;
	}
	double skipSharePct() const
	{
		return s.client_pass_b_ms > 0
		               ? 100.0 * double(s.client_pass_b_skip_ms) / double(s.client_pass_b_ms)
		               : 0.0;
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
	int effort() const
	{
		return int(s.effort);
	}
	int snapIdentity() const
	{
		return int(s.snap_identity);
	}
	// "N/M (share)" or an empty string when there is nothing to say -- an
	// intra-only stream, or a codec with no such notion.
	// Which constraint is deciding the quantiser, and -- when it is the headset's
	// decode deadline -- the measurement and the budget it is being held against, so
	// the line says why the picture is what it is and not merely what it is. Empty
	// when the stream has not reported one, which is what an older client looks like.
	QString rateBinding() const
	{
		if (s.rc_binding.empty() or s.rc_binding == "?")
			return {};
		const QString what = QString::fromStdString(s.rc_binding);
		if (s.rc_binding != "decode" and s.rc_decode_floor == 0)
			return what;
		return QStringLiteral("%1 (decode %2 ms of %3 ms budget, quantiser floor %4)")
		        .arg(what)
		        .arg(double(s.client_decode_us) / 1000.0, 0, 'f', 1)
		        .arg(s.rc_decode_budget_ms, 0, 'f', 1)
		        .arg(s.rc_decode_floor);
	}
	QString identityTiles() const
	{
		if (s.identity_tiles_total == 0)
			return {};
		const double pc = 100.0 * double(s.identity_tiles) /
		                  double(s.identity_tiles_total);
		return QStringLiteral("%1/%2 (%3 %)")
		        .arg(s.identity_tiles)
		        .arg(s.identity_tiles_total)
		        .arg(pc, 0, 'f', 1);
	}
	bool identityFromDecoder() const
	{
		return s.identity_from_decoder;
	}
	QString planar() const
	{
		return QString::fromStdString(s.planar);
	}
	// Empty when the configured level is the level in use; otherwise the
	// reason it is not, which is the half of this field that matters.
	QString planarNote() const
	{
		return QString::fromStdString(s.planar_note);
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
	bool lensMaskOn() const
	{
		return s.lens_mask_on;
	}
	bool lensMaskEnforced() const
	{
		return s.lens_mask_enforced;
	}
	int lensMaskMasked() const
	{
		return int(s.lens_mask_masked);
	}
	int lensMaskTiles() const
	{
		return int(s.lens_mask_tiles ? s.lens_mask_tiles : s.tiles());
	}
	int lensMaskMargin() const
	{
		return int(s.lens_mask_margin);
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
