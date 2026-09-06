/*
 * WiVRn VR streaming
 * Copyright (C) 2024  Guillaume Meunier <guillaume.meunier@centraliens.net>
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

#include <QJSValue>
#include <QList>
#include <QObject>
#include <QPointF>
#include <QSize>
#include <QSizeF>
#include <nlohmann/json.hpp>
#include <qqmlintegration.h>

#define SETTER_GETTER_NOTIFY(type, prop_name)     \
public:                                           \
	type prop_name() const;                   \
	void set_##prop_name(const type & value); \
Q_SIGNALS:                                        \
	void prop_name##Changed();

class wivrn_server;

class Settings : public QObject
{
	Q_OBJECT
	QML_NAMED_ELEMENT(Settings)
	QML_SINGLETON
public:
	enum encoder_name
	{
		EncoderAuto,
		Nvenc,
		Vaapi,
		X264,
		Vulkan,
		Nxwarp,
	};
	Q_ENUM(encoder_name)

	enum video_codec
	{
		CodecAuto,
		H264,
		H265,
		Av1,
		CodecNxwarp,
	};
	Q_ENUM(video_codec)

	// "edge_bleed.extension": what the headset does over the margin when the server did
	// not overscan. `None` is the pre-feature behaviour: a black band at the edge of the
	// view whenever a reprojection outruns the frame.
	enum edge_extension
	{
		ExtensionNone,
		ExtensionClamp,
		ExtensionFade,
	};
	Q_ENUM(edge_extension)

	// "atlas": the ATLAS coding mode ([SYN] 13.12, tool bits 31 and 34). Two values and
	// deliberately no third: `Auto` turns on the atlas together with the per-frame
	// ATLAS/PICTURE mode switch and lets the displacement trigger choose, and there is no
	// "atlas without the switch" because that is the form whose reference goes stale under
	// fast head motion.
	enum nxwarp_atlas
	{
		AtlasOff,
		AtlasAuto,
	};
	Q_ENUM(nxwarp_atlas)

	// "entropy": which entropy coder the NX Warp bitstream uses.
	enum nxwarp_entropy
	{
		EntropyAuto,
		EntropyRans,
		EntropyLite,
	};
	Q_ENUM(nxwarp_entropy)

	// "pace": how often the encoder sends. A fixed rate carries a frame rate alongside.
	enum nxwarp_pace
	{
		PaceAuto,
		PaceOff,
		PaceFixed,
	};
	Q_ENUM(nxwarp_pace)

	// "stereo-frame": whether both eyes are coded as one nxvc stereo frame on stream 0.
	enum nxwarp_stereo
	{
		StereoAuto,
		StereoOn,
		StereoOff,
	};
	Q_ENUM(nxwarp_stereo)

	// "tile-map": how a frame's bytes are laid on the transport's tile grid. See
	// nxwarp_settings.h -- TileAuto and TileSpans behave identically today, and both are
	// offered because the third is the one that changes behaviour.
	enum nxwarp_tile_map
	{
		TileAuto,
		TileSpans,
		TileChunks,
	};
	Q_ENUM(nxwarp_tile_map)

	// "coded-vectors": whether motion vectors are coded into the stream.
	enum nxwarp_coded_vectors
	{
		VectorsDefault,
		VectorsNone,
		VectorsStatic,
	};
	Q_ENUM(nxwarp_coded_vectors)

	Q_PROPERTY(bool simpleConfig READ simpleConfig NOTIFY simpleConfigChanged)
	Q_PROPERTY(encoder_name encoder READ encoder WRITE set_encoder NOTIFY encoderChanged)
	Q_PROPERTY(video_codec codec READ codec WRITE set_codec NOTIFY codecChanged)
	Q_PROPERTY(QList<video_codec> allowedCodecs READ allowedCodecs NOTIFY encoderChanged)
	Q_PROPERTY(bool can10bit READ can10bit NOTIFY codecChanged)
	Q_PROPERTY(bool tenbit READ tenbit WRITE set_tenbit NOTIFY tenbitChanged)
	Q_PROPERTY(bool bitrateAuto READ bitrateAuto WRITE set_bitrateAuto NOTIFY bitrateAutoChanged)
	Q_PROPERTY(bool mirror READ mirror WRITE set_mirror NOTIFY mirrorChanged)

	// NX Warp encoder controls. Every one of these is a server configuration key that had no
	// GUI: the whole section is gated on nxwarpSelected, because none of them mean anything
	// for an H.264/HEVC/AV1 encoder.
	Q_PROPERTY(bool nxwarpSelected READ nxwarpSelected NOTIFY encoderChanged)
	Q_PROPERTY(float streamScale READ streamScale WRITE set_streamScale NOTIFY streamScaleChanged)
	// Edge bleed. Top level, and NOT gated on nxwarpSelected: the overscan is a property
	// of the field of view the application renders and the extension is a client-side
	// render pass, so both apply to an H.264 session exactly as they do to an NX Warp one.
	Q_PROPERTY(float edgeBleedOverscan READ edgeBleedOverscan WRITE set_edgeBleedOverscan NOTIFY edgeBleedOverscanChanged)
	Q_PROPERTY(edge_extension edgeBleedExtension READ edgeBleedExtension WRITE set_edgeBleedExtension NOTIFY edgeBleedExtensionChanged)
	Q_PROPERTY(nxwarp_atlas nxwarpAtlas READ nxwarpAtlas WRITE set_nxwarpAtlas NOTIFY nxwarpAtlasChanged)
	Q_PROPERTY(int nxwarpAtlasPictureThreshold READ nxwarpAtlasPictureThreshold WRITE set_nxwarpAtlasPictureThreshold NOTIFY nxwarpAtlasPictureThresholdChanged)
	Q_PROPERTY(nxwarp_entropy nxwarpEntropy READ nxwarpEntropy WRITE set_nxwarpEntropy NOTIFY nxwarpEntropyChanged)
	Q_PROPERTY(nxwarp_pace nxwarpPace READ nxwarpPace WRITE set_nxwarpPace NOTIFY nxwarpPaceChanged)
	Q_PROPERTY(int nxwarpPaceFps READ nxwarpPaceFps WRITE set_nxwarpPaceFps NOTIFY nxwarpPaceFpsChanged)
	Q_PROPERTY(bool nxwarpRcAuto READ nxwarpRcAuto WRITE set_nxwarpRcAuto NOTIFY nxwarpRcAutoChanged)
	Q_PROPERTY(int nxwarpMinQp READ nxwarpMinQp WRITE set_nxwarpMinQp NOTIFY nxwarpMinQpChanged)
	Q_PROPERTY(int nxwarpMaxQp READ nxwarpMaxQp WRITE set_nxwarpMaxQp NOTIFY nxwarpMaxQpChanged)
	Q_PROPERTY(nxwarp_stereo nxwarpStereoFrame READ nxwarpStereoFrame WRITE set_nxwarpStereoFrame NOTIFY nxwarpStereoFrameChanged)
	Q_PROPERTY(nxwarp_tile_map nxwarpTileMap READ nxwarpTileMap WRITE set_nxwarpTileMap NOTIFY nxwarpTileMapChanged)
	Q_PROPERTY(nxwarp_coded_vectors nxwarpCodedVectors READ nxwarpCodedVectors WRITE set_nxwarpCodedVectors NOTIFY nxwarpCodedVectorsChanged)
	Q_PROPERTY(bool nxwarpEffort READ nxwarpEffort WRITE set_nxwarpEffort NOTIFY nxwarpEffortChanged)
	Q_PROPERTY(bool nxwarpInter READ nxwarpInter WRITE set_nxwarpInter NOTIFY nxwarpInterChanged)
	Q_PROPERTY(int nxwarpIntraPeriod READ nxwarpIntraPeriod WRITE set_nxwarpIntraPeriod NOTIFY nxwarpIntraPeriodChanged)

	Q_PROPERTY(bool tcpOnly READ tcpOnly WRITE set_tcpOnly NOTIFY tcpOnlyChanged)
	Q_PROPERTY(int port READ port WRITE set_port NOTIFY portChanged)
	Q_PROPERTY(QString hostname READ hostname WRITE set_hostname NOTIFY hostnameChanged)
	Q_PROPERTY(QString application READ application WRITE set_application NOTIFY applicationChanged)
	Q_PROPERTY(QString openvr READ openvr WRITE set_openvr NOTIFY openvrChanged)

	Q_PROPERTY(bool hidForwarding READ hidForwarding WRITE set_hidForwarding NOTIFY hidForwardingChanged)
	Q_PROPERTY(bool debugGui READ debugGui WRITE set_debugGui NOTIFY debugGuiChanged)
	Q_PROPERTY(bool steamVrLh READ steamVrLh WRITE set_steamVrLh NOTIFY steamVrLhChanged)
	Q_PROPERTY(float lhStickDeadzone READ lhStickDeadzone WRITE set_lhStickDeadzone NOTIFY lhStickDeadzoneChanged)

	Q_PROPERTY(bool flatpak READ flatpak CONSTANT)
	Q_PROPERTY(int default_port READ default_port CONSTANT)
	Q_PROPERTY(bool hid_forwarding_supported READ hid_forwarding CONSTANT)
	Q_PROPERTY(bool debug_gui_supported READ debug_gui CONSTANT)
	Q_PROPERTY(bool steamvr_lh_supported READ steamvr_lh CONSTANT)

	SETTER_GETTER_NOTIFY(bool, simpleConfig)
	SETTER_GETTER_NOTIFY(encoder_name, encoder)
	SETTER_GETTER_NOTIFY(video_codec, codec)
	SETTER_GETTER_NOTIFY(bool, tenbit)
	SETTER_GETTER_NOTIFY(bool, bitrateAuto)
	SETTER_GETTER_NOTIFY(bool, mirror)
	SETTER_GETTER_NOTIFY(QString, application)
	SETTER_GETTER_NOTIFY(bool, hidForwarding)
	SETTER_GETTER_NOTIFY(bool, debugGui)
	SETTER_GETTER_NOTIFY(bool, steamVrLh)
	SETTER_GETTER_NOTIFY(float, lhStickDeadzone)
	SETTER_GETTER_NOTIFY(bool, tcpOnly)
	SETTER_GETTER_NOTIFY(int, port)
	SETTER_GETTER_NOTIFY(QString, hostname)
	SETTER_GETTER_NOTIFY(QString, openvr)
	SETTER_GETTER_NOTIFY(float, streamScale)
	SETTER_GETTER_NOTIFY(float, edgeBleedOverscan)
	SETTER_GETTER_NOTIFY(edge_extension, edgeBleedExtension)
	SETTER_GETTER_NOTIFY(nxwarp_atlas, nxwarpAtlas)
	SETTER_GETTER_NOTIFY(int, nxwarpAtlasPictureThreshold)
	SETTER_GETTER_NOTIFY(nxwarp_entropy, nxwarpEntropy)
	SETTER_GETTER_NOTIFY(nxwarp_pace, nxwarpPace)
	SETTER_GETTER_NOTIFY(int, nxwarpPaceFps)
	SETTER_GETTER_NOTIFY(bool, nxwarpRcAuto)
	SETTER_GETTER_NOTIFY(int, nxwarpMinQp)
	SETTER_GETTER_NOTIFY(int, nxwarpMaxQp)
	SETTER_GETTER_NOTIFY(nxwarp_stereo, nxwarpStereoFrame)
	SETTER_GETTER_NOTIFY(nxwarp_tile_map, nxwarpTileMap)
	SETTER_GETTER_NOTIFY(nxwarp_coded_vectors, nxwarpCodedVectors)
	SETTER_GETTER_NOTIFY(bool, nxwarpEffort)
	SETTER_GETTER_NOTIFY(bool, nxwarpInter)
	SETTER_GETTER_NOTIFY(int, nxwarpIntraPeriod)

public:
	bool nxwarpSelected() const;

	// The per-eye size the encoder will actually produce for a headset asking for
	// `headsetWidth` x `headsetHeight`, at the streamScale currently set: the same
	// derivation the server does in get_encoder_settings, so the settings page can show the
	// result of the slider before anything is saved. Returns a zero size when the headset
	// size is not known yet.
	Q_INVOKABLE QSize encodedEyeSize(int headsetWidth, int headsetHeight) const;
	// Tiles per eye at that size: the NX Warp decoder's per-frame work, which is the whole
	// reason the slider exists.
	Q_INVOKABLE int encodedTiles(int headsetWidth, int headsetHeight) const;
	// Whether the eyes will actually be paired into one stereo frame at the current
	// settings, and the size and tile count of the paired frame if so. The pairing gate
	// lives in get_encoder_settings; this mirrors it so the slider can show the
	// consequence before anything is saved.
	Q_INVOKABLE bool willPairEyes() const;
	Q_INVOKABLE QSize pairedFrameSize(int headsetWidth, int headsetHeight) const;
	Q_INVOKABLE int pairedTiles(int headsetWidth, int headsetHeight) const;
	// The server's own size gate on pairing, and "pairing was asked for and this size
	// refuses it". Kept as two calls because the first is a pure fact about a width that
	// the test can drive both ways, while the second is only reachable through
	// encodedEyeSize -- which aligns to 64 and therefore never trips it today. That is
	// the point: the warning is the guardrail for the day the alignment changes, and the
	// test pins the claim that no reachable slider position needs it.
	Q_INVOKABLE bool pairingWidthOk(int perEyeWidth) const;
	Q_INVOKABLE bool pairingRefused(int headsetWidth, int headsetHeight) const;

private:
	nlohmann::json m_jsonSettings = nlohmann::json::object();
	nlohmann::json m_originalSettings;
	void emitAllChanged();

public:
	Settings(QObject * parent = nullptr) :
	        QObject(parent) {}

	Q_INVOKABLE void load(const wivrn_server * server);
	Q_INVOKABLE void save(wivrn_server * server);
	Q_INVOKABLE void restore_defaults();

	// The configuration document itself: what load() parsed and what save() will hand the
	// server, verbatim. Split out of load()/save() so the settings document can be read and
	// replaced without a live server on the bus — which is how the round-trip test drives it.
	const nlohmann::json & configuration() const
	{
		return m_jsonSettings;
	}
	void load_json(nlohmann::json settings);

	QList<video_codec> allowedCodecs() const;
	bool can10bit() const;

	bool flatpak() const;
	int default_port() const;
	bool debug_gui() const;
	bool steamvr_lh() const;
	bool hid_forwarding() const;

	static encoder_name encoder_id_from_string(std::string_view s);
	static video_codec codec_id_from_string(std::string_view s);
	static const std::string & encoder_from_id(encoder_name id);
	static const std::string & codec_from_id(video_codec id);

Q_SIGNALS:
	void settingsChanged();
};

#undef SETTER_GETTER_NOTIFY
