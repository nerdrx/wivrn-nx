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

#include "settings.h"

#include "client/utils/view_geometry.h"

#include "encoder/stream_scale.h"
#include "escape_string.h"
#include "gui_config.h"
#include "nxwarp_settings.h"
#include "utils/flatpak.h"
#include "wivrn_config.h"
#include "wivrn_server.h"
#include <QList>
#include <QObject>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <qqmlintegration.h>
#include <unistd.h>

namespace
{
const std::vector<std::pair<Settings::encoder_name, std::string>> encoder_ids{
        {Settings::encoder_name::Nvenc, "nvenc"},
        {Settings::encoder_name::Vaapi, "vaapi"},
        {Settings::encoder_name::X264, "x264"},
        {Settings::encoder_name::Vulkan, "vulkan"},
        {Settings::encoder_name::Nxwarp, "nxwarp"},
};

const std::vector<std::tuple<Settings::video_codec, std::string>> codec_ids{
        {Settings::video_codec::H264, "h264"},
        {Settings::video_codec::H264, "avc"},
        {Settings::video_codec::H265, "h265"},
        {Settings::video_codec::H265, "hevc"},
        {Settings::video_codec::Av1, "av1"},
        {Settings::video_codec::Av1, "AV1"},
        {Settings::video_codec::CodecNxwarp, "nxwarp"},
};

bool can10bit(Settings::video_codec c)
{
	switch (c)
	{
		case Settings::CodecAuto:
		case Settings::H264:
			return false;
		// NX Warp v1 is an 8-bit bitstream; get_encoder_settings forces bit_depth 8 for
		// it whatever the configuration says.
		case Settings::CodecNxwarp:
			return false;
		case Settings::H265:
		case Settings::Av1:
			return true;
	}
	return false;
}
} // namespace

Settings::encoder_name Settings::encoder_id_from_string(std::string_view s)
{
	for (auto & [i, j]: encoder_ids)
	{
		if (j == s)
			return i;
	}
	return Settings::encoder_name::EncoderAuto;
}

Settings::video_codec Settings::codec_id_from_string(std::string_view s)
{
	for (auto & [i, j]: codec_ids)
	{
		if (j == s)
			return i;
	}
	return Settings::video_codec::CodecAuto;
}

const std::string & Settings::encoder_from_id(Settings::encoder_name id)
{
	static const std::string default_value = "auto";

	for (auto & [i, j]: encoder_ids)
	{
		if (i == id)
			return j;
	}

	return default_value;
}

const std::string & Settings::codec_from_id(Settings::video_codec id)
{
	static const std::string default_value = "auto";

	for (auto & [i, j]: codec_ids)
	{
		if (i == id)
			return j;
	}

	return default_value;
}

void Settings::emitAllChanged()
{
	simpleConfigChanged();
	encoderChanged();
	codecChanged();
	tenbitChanged();
	bitrateAutoChanged();
	mirrorChanged();
	tcpOnlyChanged();
	applicationChanged();
	openvrChanged();
	debugGuiChanged();
	steamVrLhChanged();
	lhStickDeadzoneChanged();
	hidForwardingChanged();
	portChanged();
	hostnameChanged();
	streamScaleChanged();
	edgeBleedOverscanChanged();
	edgeBleedExtensionChanged();
	nxwarpEntropyChanged();
	nxwarpPaceChanged();
	nxwarpPaceFpsChanged();
	nxwarpRcAutoChanged();
	nxwarpMinQpChanged();
	nxwarpMaxQpChanged();
	nxwarpStereoFrameChanged();
	nxwarpTileMapChanged();
	nxwarpCodedVectorsChanged();
	nxwarpInterChanged();
	nxwarpLensMaskChanged();
	nxwarpIntraPeriodChanged();
}

void Settings::load(const wivrn_server * server)
{
	// Encoders configuration
	try
	{
		auto conf = server->jsonConfiguration();
		load_json(nlohmann::json::parse(conf.toUtf8()));
	}
	catch (std::exception & e)
	{
		qWarning() << "Cannot read configuration: " << e.what();
		m_jsonSettings = nlohmann::json::object();
		emitAllChanged();
		return;
	}
}

void Settings::load_json(nlohmann::json settings)
{
	m_jsonSettings = std::move(settings);
	if (not m_jsonSettings.is_object())
		m_jsonSettings = nlohmann::json::object();
	m_originalSettings = m_jsonSettings;
	emitAllChanged();
}

static Settings::encoder_name encoder_for_item(const nlohmann::json & item)
{
	if (item.is_string())
		return Settings::encoder_id_from_string(std::string(item));
	if (item.is_object())
	{
		if (auto i = item.find("encoder"); i != item.end())
			return Settings::encoder_id_from_string(std::string(*i));
	}
	return Settings::encoder_name::EncoderAuto;
}

static Settings::video_codec codec_for_item(const nlohmann::json & item)
{
	if (item.is_object())
	{
		if (auto i = item.find("codec"); i != item.end())
			return Settings::codec_id_from_string(std::string(*i));
	}
	return Settings::video_codec::CodecAuto;
}

bool Settings::simpleConfig() const
{
	// Simple configuration: either unset, single encoder or all encoders + codecs are the same
	try
	{
		auto it = m_jsonSettings.find("encoder");
		if (it == m_jsonSettings.end() or it->is_object() or it->is_string())
			return true;
		std::optional<encoder_name> encoder;
		std::optional<video_codec> codec;
		for (const auto & item: *it)
		{
			auto e = encoder_for_item(item);
			if (encoder and e != encoder)
				return false;
			encoder = e;

			auto c = codec_for_item(item);
			if (codec and c != codec)
				return false;
			codec = c;
		}
		return true;
	}
	catch (...)
	{
		return false;
	}
}

Settings::encoder_name Settings::encoder() const
{
	try
	{
		auto it = m_jsonSettings.find("encoder");
		if (it == m_jsonSettings.end())
			return encoder_name::EncoderAuto;
		if (it->is_array() and it->size())
			it = it->begin();
		return encoder_for_item(*it);
	}
	catch (...)
	{
	}
	return encoder_name::EncoderAuto;
}

void Settings::set_encoder(const encoder_name & value)
{
	if (value == encoder())
		return;
	auto old_codec = codec();
	switch (value)
	{
		case EncoderAuto:
			m_jsonSettings.erase("encoder");
			break;
		case Nvenc:
		case Vaapi:
		case X264:
		case Vulkan:
			m_jsonSettings["encoder"] = encoder_from_id(value);
			break;
		// NX Warp needs the codec spelled out as well: "nxwarp" alone would be read as an
		// encoder name with an automatic codec, and the codec is not negotiable.
		case Nxwarp:
			m_jsonSettings["encoder"] = nlohmann::json::object({
			        {"encoder", std::string(wivrn::dashboard::nxwarp_name)},
			        {"codec", std::string(wivrn::dashboard::nxwarp_name)},
			});
	}
	encoderChanged();
	if (not can10bit())
		set_tenbit(false);
	if (old_codec != codec())
		codecChanged();
	simpleConfigChanged();
}

Settings::video_codec Settings::codec() const
{
	try
	{
		auto it = m_jsonSettings.find("encoder");
		if (it == m_jsonSettings.end())
			return video_codec::CodecAuto;
		if (it->is_array() and it->size())
			it = it->begin();
		return codec_for_item(*it);
	}
	catch (...)
	{
	}
	return video_codec::CodecAuto;
}

void Settings::set_codec(const video_codec & value)
{
	auto old = codec();
	auto it = m_jsonSettings.find("encoder");
	if (it == m_jsonSettings.end())
		return;

	if (it->is_string())
	{
		m_jsonSettings["encoder"] = nlohmann::json::object({
		        {"encoder", *it},
		        {"codec", codec_from_id(value)},
		});
	}

	if (it->is_object())
	{
		if (value == CodecAuto)
		{
			if (auto encoder = it->find("encoder"); encoder != it->end())
				*it = *encoder;
			else
				m_jsonSettings.erase("encoder");
		}
		else
			(*it)["codec"] = codec_from_id(value);
	}
	else if (it->is_array())
	{
		for (auto & item: *it)
		{
			if (it->is_string())
			{
				item = nlohmann::json::object({
				        {"encoder", item},
				        {"codec", codec_from_id(value)},
				});
				continue;
			}
			if (not item.is_object())
				continue;
			if (value == CodecAuto)
				item.erase("codec");
			else
				item["codec"] = codec_from_id(value);
		}
	}
	if (value != old)
	{
		if (::can10bit(value) != ::can10bit(old))
			set_tenbit(::can10bit(value));
		codecChanged();
	}
}

bool Settings::tenbit() const
{
	try
	{
		return m_jsonSettings.value("bit-depth", 8) == 10;
	}
	catch (...)
	{}
	return false;
}

void Settings::set_tenbit(const bool & value)
{
	auto old = tenbit();
	// NX Warp is an 8-bit bitstream with no setting at all -- get_encoder_settings forces
	// bit_depth 8 for it -- so writing one out would suggest a choice that does not exist.
	if (codec() == CodecAuto or codec() == CodecNxwarp)
		m_jsonSettings.erase("bit-depth");
	else
		m_jsonSettings["bit-depth"] = value ? 10 : 8;
	if (value != old)
		tenbitChanged();
}

QString Settings::application() const
{
	// Automatically started application
	try
	{
		std::vector<std::string> application;
		auto it = m_jsonSettings.find("application");
		if (it == m_jsonSettings.end())
			return "";
		if (it->is_array())
			application = *it;
		else if (it->is_string())
			application.push_back(*it);

		return escape_string(application);
	}
	catch (...)
	{
		return "";
	}
}

void Settings::set_application(const QString & value)
{
	auto old = application();
	if (value.isEmpty())
		m_jsonSettings.erase("application");
	else
		m_jsonSettings["application"] = unescape_string(value);
	if (old != value)
		applicationChanged();
}

bool Settings::hidForwarding() const
{
	auto it = m_jsonSettings.find("hid-forwarding");
	if (it != m_jsonSettings.end() and it->is_boolean())
		return *it;
	return false;
}

void Settings::set_hidForwarding(const bool & value)
{
	auto old = hidForwarding();
	m_jsonSettings["hid-forwarding"] = value;
	if (old != value)
		hidForwardingChanged();
}

bool Settings::bitrateAuto() const
{
	// "bitrate-auto" can be a boolean or an object {"enabled": bool, "min-bitrate": bps}
	auto it = m_jsonSettings.find("bitrate-auto");
	if (it != m_jsonSettings.end())
	{
		if (it->is_boolean())
			return *it;
		if (it->is_object())
		{
			if (auto enabled = it->find("enabled"); enabled != it->end() and enabled->is_boolean())
				return *enabled;
		}
	}
	return true;
}

void Settings::set_bitrateAuto(const bool & value)
{
	auto old = bitrateAuto();
	// Keep the object form (and its min-bitrate) if the user configured one by hand
	if (auto it = m_jsonSettings.find("bitrate-auto"); it != m_jsonSettings.end() and it->is_object())
		(*it)["enabled"] = value;
	else
		m_jsonSettings["bitrate-auto"] = value;
	if (old != value)
		bitrateAutoChanged();
}

bool Settings::mirror() const
{
	// "mirror" can be a boolean or an object {"enabled": bool, "fps": int, "scale": float}
	auto it = m_jsonSettings.find("mirror");
	if (it != m_jsonSettings.end())
	{
		if (it->is_boolean())
			return *it;
		if (it->is_object())
		{
			if (auto enabled = it->find("enabled"); enabled != it->end() and enabled->is_boolean())
				return *enabled;
		}
	}
	return false;
}

void Settings::set_mirror(const bool & value)
{
	auto old = mirror();
	// Keep the object form (and its fps/scale) if the user configured one by hand
	if (auto it = m_jsonSettings.find("mirror"); it != m_jsonSettings.end() and it->is_object())
		(*it)["enabled"] = value;
	else
		m_jsonSettings["mirror"] = value;
	if (old != value)
		mirrorChanged();
}

bool Settings::debugGui() const
{
	// Advanced options (debug window, steamvr_lh)
	auto it = m_jsonSettings.find("debug-gui");
	if (it != m_jsonSettings.end() and it->is_boolean())
		return *it;
	return false;
}

void Settings::set_debugGui(const bool & value)
{
	auto old = debugGui();
	m_jsonSettings["debug-gui"] = value;
	if (old != value)
		debugGuiChanged();
}

bool Settings::steamVrLh() const
{
	auto it = m_jsonSettings.find("use-steamvr-lh");
	if (it != m_jsonSettings.end() and it->is_boolean())
		return *it;
	return false;
}

void Settings::set_steamVrLh(const bool & value)
{
	auto old = steamVrLh();
	m_jsonSettings["use-steamvr-lh"] = value;
	if (old != value)
		steamVrLhChanged();
}

float Settings::lhStickDeadzone() const
{
	auto it = m_jsonSettings.find("lh-stick-deadzone");
	if (it != m_jsonSettings.end() and it->is_number())
		return static_cast<float>(*it);
	return 0.0f;
}

void Settings::set_lhStickDeadzone(const float & value)
{
	auto old = lhStickDeadzone();
	m_jsonSettings["lh-stick-deadzone"] = value;
	if (old != value)
		lhStickDeadzoneChanged();
}

bool Settings::tcpOnly() const
{
	auto it = m_jsonSettings.find("tcp-only");
	if (it != m_jsonSettings.end() and it->is_boolean())
		return *it;
	return false;
}

void Settings::set_tcpOnly(const bool & value)
{
	auto old = tcpOnly();
	m_jsonSettings["tcp-only"] = value;
	if (old != value)
		tcpOnlyChanged();
}

int Settings::port() const
{
	auto it = m_jsonSettings.find("port");
	if (it != m_jsonSettings.end() and it->is_number_integer())
		return *it;
	return wivrn::default_port;
}

void Settings::set_port(const int & value)
{
	auto old = port();
	m_jsonSettings["port"] = value;
	if (old != value)
		portChanged();
}

QString Settings::hostname() const
{
	if (auto it = m_jsonSettings.find("hostname"); it != m_jsonSettings.end() and it->is_string())
		return QString::fromStdString(*it);
	return "";
}

void Settings::set_hostname(const QString & value)
{
	auto old = hostname();
	if (value == "")
		m_jsonSettings.erase("hostname");
	else
		m_jsonSettings["hostname"] = value.toStdString();
	if (old != value)
		hostnameChanged();
}

QString Settings::openvr() const
{
	// OpenVR compat library
	if (auto it = m_jsonSettings.find("openvr-compat-path"); it != m_jsonSettings.end())
	{
		if (it->is_null())
			return "-";
		if (it->is_string())
			return QString::fromStdString(*it);
	}
	return "";
}

void Settings::set_openvr(const QString & value)
{
	auto old = openvr();
	if (value == "-")
		m_jsonSettings["openvr-compat-path"] = nullptr;
	else if (value == "")
		m_jsonSettings.erase("openvr-compat-path");
	else
		m_jsonSettings["openvr-compat-path"] = value.toStdString();
	if (old != value)
		openvrChanged();
}

QList<Settings::video_codec> Settings::allowedCodecs() const
{
	switch (encoder())
	{
		case encoder_name::EncoderAuto:
			return {video_codec::CodecAuto};
		case encoder_name::Nvenc:
		case encoder_name::Vaapi:
			return {
			        video_codec::CodecAuto,
			        video_codec::H264,
			        video_codec::H265,
			        video_codec::Av1,
			};
		case encoder_name::Vulkan:
			return {
			        video_codec::CodecAuto,
			        video_codec::H264,
			        video_codec::H265,
			};
		case encoder_name::X264:
			return {
			        video_codec::H264,
			};
		// NX Warp is its own codec: the encoder and the bitstream are the same thing.
		case encoder_name::Nxwarp:
			return {
			        video_codec::CodecNxwarp,
			};
	}
	return {video_codec::CodecAuto};
}

bool Settings::can10bit() const
{
	return ::can10bit(codec());
}

// ---------------------------------------------------------------------------------------------
// NX Warp encoder controls
//
// Everything below is a thin wrapper over nxwarp_settings.h, which does the JSON and is what the
// round-trip test drives. The rule these all share: setting a control to the encoder's own default
// erases the key rather than writing it, so config.json keeps only what the user actually chose.
// ---------------------------------------------------------------------------------------------

namespace nxd = wivrn::dashboard;

bool Settings::nxwarpSelected() const
{
	return nxd::has_nxwarp(m_jsonSettings);
}

float Settings::streamScale() const
{
	// Top level, not an encoder option: it is the compositor's encode size, not a codec
	// parameter. Out-of-range values are what the server would refuse, so they read as the
	// default and the slider shows what will actually run.
	auto it = m_jsonSettings.find("stream_scale");
	if (it == m_jsonSettings.end())
		it = m_jsonSettings.find("stream-scale");
	if (it != m_jsonSettings.end() and it->is_number())
	{
		const double v = it->get<double>();
		if (v > 0 and v <= 1)
			return float(v);
	}
	return 1.0f;
}

void Settings::set_streamScale(const float & value)
{
	const auto old = streamScale();
	// Snapped to the slider's own step so a float that reads back a hair off does not make
	// every save look like a change.
	// Rounded in double, not float: a float 0.8 widened to double is 0.800000011920929, and
	// that is what would land in the user's config.json.
	const double clamped = std::clamp(std::round(double(value) * 100.0) / 100.0, 0.5, 1.0);
	// The dashed spelling is accepted on read; normalise to the documented one on write so a
	// config never carries both.
	m_jsonSettings.erase("stream-scale");
	if (clamped >= 1.0)
		m_jsonSettings.erase("stream_scale");
	else
		m_jsonSettings["stream_scale"] = clamped;
	if (old != streamScale())
		streamScaleChanged();
}

// Edge bleed. One object, "edge_bleed", because the two controls are one feature and a
// person turning it off should be deleting one thing. The server clamps every member and
// ignores an unknown one, so writing a partial object is safe; the dashboard still erases
// the whole key when both controls sit at their defaults, the same rule the rest of this
// file follows.
namespace
{
constexpr const char * edge_bleed_key = "edge_bleed";

const nlohmann::json * find_edge_bleed(const nlohmann::json & settings)
{
	auto it = settings.find(edge_bleed_key);
	if (it == settings.end())
		it = settings.find("edge-bleed");
	return (it != settings.end() and it->is_object()) ? &*it : nullptr;
}

// The object to write into, created on demand. Kept in one place because both setters
// need it and both must also drop the "edge-bleed" spelling so the two cannot disagree.
nlohmann::json & edge_bleed_object(nlohmann::json & settings)
{
	settings.erase("edge-bleed");
	auto & o = settings[edge_bleed_key];
	if (not o.is_object())
		o = nlohmann::json::object();
	return o;
}
} // namespace

float Settings::edgeBleedOverscan() const
{
	if (const auto * o = find_edge_bleed(m_jsonSettings))
	{
		if (auto it = o->find("overscan"); it != o->end() and it->is_number())
		{
			const double v = it->get<double>();
			if (v >= 0 and v <= double(wivrn::view_geometry::max_overscan))
				return float(v);
		}
	}
	// The value the server applies for an absent key, so the slider shows what will
	// actually run rather than zero.
	return wivrn::view_geometry::default_overscan;
}

void Settings::set_edgeBleedOverscan(const float & value)
{
	const auto old = edgeBleedOverscan();
	// Snapped to the slider's own step, in whole percent: the file should hold the number
	// the person chose, not a float that drifted through a QML binding.
	const double clamped = std::clamp(std::round(double(value) * 100.0) / 100.0,
	                                  0.0,
	                                  double(wivrn::view_geometry::max_overscan));
	auto & o = edge_bleed_object(m_jsonSettings);
	// The default is a float and `clamped` is a double snapped to whole percent, so they
	// are compared at the slider's own resolution: double(0.05f) is 0.050000000745, which
	// is not 0.05, and a bare == would write the default out as if it were a choice.
	const double dflt = std::round(double(wivrn::view_geometry::default_overscan) * 100.0) / 100.0;
	if (clamped == dflt)
		o.erase("overscan");
	else
		o["overscan"] = clamped;
	if (o.empty())
		m_jsonSettings.erase(edge_bleed_key);
	if (old != edgeBleedOverscan())
		edgeBleedOverscanChanged();
}

Settings::edge_extension Settings::edgeBleedExtension() const
{
	if (const auto * o = find_edge_bleed(m_jsonSettings))
	{
		if (auto it = o->find("extension"); it != o->end() and it->is_string())
		{
			const auto v = it->get<std::string>();
			if (v == "none" or v == "off")
				return ExtensionNone;
			if (v == "clamp")
				return ExtensionClamp;
			if (v == "fade")
				return ExtensionFade;
		}
	}
	return ExtensionFade;
}

void Settings::set_edgeBleedExtension(const edge_extension & value)
{
	const auto old = edgeBleedExtension();
	auto & o = edge_bleed_object(m_jsonSettings);
	switch (value)
	{
		case ExtensionNone:
			o["extension"] = "none";
			break;
		case ExtensionClamp:
			o["extension"] = "clamp";
			break;
		case ExtensionFade:
		default:
			// The default the server applies for an absent key.
			o.erase("extension");
			break;
	}
	if (o.empty())
		m_jsonSettings.erase(edge_bleed_key);
	if (old != edgeBleedExtension())
		edgeBleedExtensionChanged();
}

QSize Settings::encodedEyeSize(int headsetWidth, int headsetHeight) const
{
	if (headsetWidth <= 0 or headsetHeight <= 0)
		return {};
	// The server's own derivation, shared rather than reimplemented: get_encoder_settings
	// calls the same function. The client scale is passed as 1 because the dashboard is
	// showing the effect of THIS slider — the headset's own reduced-resolution setting is not
	// on the wire until it connects, and when it asks for less than the slider it wins (the
	// two compose as a minimum), so this is an upper bound.
	const auto r = wivrn::stream_encode_size(uint16_t(std::min(headsetWidth, 0xffff)),
	                                         uint16_t(std::min(headsetHeight, 0xffff)),
	                                         1.0f,
	                                         streamScale());
	return QSize(r.width, r.height);
}

int Settings::encodedTiles(int headsetWidth, int headsetHeight) const
{
	const auto size = encodedEyeSize(headsetWidth, headsetHeight);
	if (size.isEmpty())
		return 0;
	return (size.width() / wivrn::encode_alignment) * (size.height() / wivrn::encode_alignment);
}

Settings::nxwarp_entropy Settings::nxwarpEntropy() const
{
	const auto v = nxd::nxwarp_option(m_jsonSettings, "entropy").value_or(std::string(nxd::nxwarp_default_entropy));
	if (v == "rans")
		return EntropyRans;
	if (v == "lite")
		return EntropyLite;
	return EntropyAuto;
}

void Settings::set_nxwarpEntropy(const nxwarp_entropy & value)
{
	const auto old = nxwarpEntropy();
	const char * v = value == EntropyRans ? "rans" : value == EntropyLite ? "lite"
	                                                                      : "auto";
	nxd::set_nxwarp_option_or_default(m_jsonSettings, "entropy", v, nxd::nxwarp_default_entropy);
	if (old != nxwarpEntropy())
		nxwarpEntropyChanged();
}

Settings::nxwarp_pace Settings::nxwarpPace() const
{
	switch (nxd::nxwarp_pace_mode(m_jsonSettings))
	{
		case nxd::pace_mode::off:
			return PaceOff;
		case nxd::pace_mode::fixed:
			return PaceFixed;
		case nxd::pace_mode::automatic:
			break;
	}
	return PaceAuto;
}

void Settings::set_nxwarpPace(const nxwarp_pace & value)
{
	const auto old = nxwarpPace();
	const auto mode = value == PaceOff ? nxd::pace_mode::off : value == PaceFixed ? nxd::pace_mode::fixed
	                                                                              : nxd::pace_mode::automatic;
	nxd::set_nxwarp_pace(m_jsonSettings, mode, uint32_t(nxwarpPaceFps()));
	if (old != nxwarpPace())
		nxwarpPaceChanged();
}

int Settings::nxwarpPaceFps() const
{
	// 72 is the rate the fixed mode is nearly always reached for: the floor of the refresh
	// rates the supported headsets run at.
	return int(nxd::nxwarp_pace_fps(m_jsonSettings, 72));
}

void Settings::set_nxwarpPaceFps(const int & value)
{
	const auto old = nxwarpPaceFps();
	// Only meaningful in the fixed mode; in the other two there is no number to write and the
	// spin box is disabled.
	if (nxwarpPace() == PaceFixed)
		nxd::set_nxwarp_pace(m_jsonSettings, nxd::pace_mode::fixed, uint32_t(std::clamp(value, 1, 1000)));
	if (old != nxwarpPaceFps())
		nxwarpPaceFpsChanged();
}

bool Settings::nxwarpRcAuto() const
{
	return nxd::nxwarp_option(m_jsonSettings, "rc").value_or(std::string(nxd::nxwarp_default_rc)) != "fixed";
}

void Settings::set_nxwarpRcAuto(const bool & value)
{
	const auto old = nxwarpRcAuto();
	nxd::set_nxwarp_option_or_default(m_jsonSettings, "rc", value ? "auto" : "fixed", nxd::nxwarp_default_rc);
	if (old != nxwarpRcAuto())
		nxwarpRcAutoChanged();
}

int Settings::nxwarpMinQp() const
{
	return int(nxd::nxwarp_option_u32(m_jsonSettings, "min-qp", nxd::nxwarp_default_min_qp));
}

void Settings::set_nxwarpMinQp(const int & value)
{
	const auto old = nxwarpMinQp();
	// The server clamps to 63 and refuses min above max; keep the GUI inside both so a saved
	// configuration can never fail to start a session.
	const uint32_t v = uint32_t(std::clamp(value, 0, 63));
	nxd::set_nxwarp_option_u32(m_jsonSettings, "min-qp", v, nxd::nxwarp_default_min_qp);
	if (int(v) > nxwarpMaxQp())
		set_nxwarpMaxQp(int(v));
	if (old != nxwarpMinQp())
		nxwarpMinQpChanged();
}

int Settings::nxwarpMaxQp() const
{
	return int(nxd::nxwarp_option_u32(m_jsonSettings, "max-qp", nxd::nxwarp_default_max_qp));
}

void Settings::set_nxwarpMaxQp(const int & value)
{
	const auto old = nxwarpMaxQp();
	const uint32_t v = uint32_t(std::clamp(value, 0, 63));
	nxd::set_nxwarp_option_u32(m_jsonSettings, "max-qp", v, nxd::nxwarp_default_max_qp);
	if (int(v) < nxwarpMinQp())
		set_nxwarpMinQp(int(v));
	if (old != nxwarpMaxQp())
		nxwarpMaxQpChanged();
}

Settings::nxwarp_stereo Settings::nxwarpStereoFrame() const
{
	switch (nxd::nxwarp_stereo_frame_mode(m_jsonSettings))
	{
		case nxd::stereo_frame_mode::on:
			return StereoOn;
		case nxd::stereo_frame_mode::off:
			return StereoOff;
		case nxd::stereo_frame_mode::automatic:
			break;
	}
	return StereoAuto;
}

void Settings::set_nxwarpStereoFrame(const nxwarp_stereo & value)
{
	const auto old = nxwarpStereoFrame();
	const auto mode = value == StereoOn ? nxd::stereo_frame_mode::on
	                                    : (value == StereoOff ? nxd::stereo_frame_mode::off
	                                                          : nxd::stereo_frame_mode::automatic);
	nxd::set_nxwarp_stereo_frame(m_jsonSettings, mode);
	if (old != nxwarpStereoFrame())
		nxwarpStereoFrameChanged();
}

Settings::nxwarp_tile_map Settings::nxwarpTileMap() const
{
	switch (nxd::nxwarp_tile_map_mode(m_jsonSettings))
	{
		case nxd::tile_map_mode::spans:
			return TileSpans;
		case nxd::tile_map_mode::chunks:
			return TileChunks;
		case nxd::tile_map_mode::automatic:
			break;
	}
	return TileAuto;
}

void Settings::set_nxwarpTileMap(const nxwarp_tile_map & value)
{
	const auto old = nxwarpTileMap();
	const auto mode = value == TileSpans ? nxd::tile_map_mode::spans
	                                     : (value == TileChunks ? nxd::tile_map_mode::chunks
	                                                            : nxd::tile_map_mode::automatic);
	nxd::set_nxwarp_tile_map(m_jsonSettings, mode);
	if (old != nxwarpTileMap())
		nxwarpTileMapChanged();
}

bool Settings::willPairEyes() const
{
	// The same gate get_encoder_settings applies, in the same order. "auto" and "on"
	// both require the whole eye pair to be NX Warp -- pairing one eye with a hardware
	// encoder removes nothing, because the win is that the headset stops running two
	// decoders. The dashboard's simple configuration gives both eyes the same encoder,
	// so "is NX Warp selected" answers "is the whole pair NX Warp".
	if (nxwarpStereoFrame() == StereoOff)
		return false;
	return nxwarpSelected();
}

bool Settings::pairingWidthOk(int perEyeWidth) const
{
	// nxvc refuses eyes == 2 unless the per-eye width is a multiple of 64, so that the
	// seam between the eyes falls on a tile boundary and each eye's sub-picture is
	// addressable. get_encoder_settings tests exactly this and logs "not pairing the
	// eyes" when it fails, having already decided to pair -- so it is a refusal after
	// the fact, which is the thing the preview has to be able to say out loud.
	return perEyeWidth > 0 and perEyeWidth % wivrn::encode_alignment == 0;
}

bool Settings::pairingRefused(int headsetWidth, int headsetHeight) const
{
	// Asked for, and then refused on the size. Not "will not pair": choosing off is not a
	// refusal, and neither is a non-NX Warp encoder.
	if (not willPairEyes())
		return false;
	const QSize eye = encodedEyeSize(headsetWidth, headsetHeight);
	if (eye.isEmpty())
		return false;
	return not pairingWidthOk(eye.width());
}

QSize Settings::pairedFrameSize(int headsetWidth, int headsetHeight) const
{
	const QSize eye = encodedEyeSize(headsetWidth, headsetHeight);
	if (eye.isEmpty() or not willPairEyes() or not pairingWidthOk(eye.width()))
		return {};
	// Side by side, eye 0 left: the pair is twice as wide and the same height.
	return QSize(eye.width() * 2, eye.height());
}

int Settings::pairedTiles(int headsetWidth, int headsetHeight) const
{
	const QSize pair = pairedFrameSize(headsetWidth, headsetHeight);
	if (pair.isEmpty())
		return 0;
	return (pair.width() / wivrn::encode_alignment) * (pair.height() / wivrn::encode_alignment);
}

Settings::nxwarp_coded_vectors Settings::nxwarpCodedVectors() const
{
	const auto v = nxd::nxwarp_option(m_jsonSettings, "coded-vectors")
	                       .value_or(std::string(nxd::nxwarp_default_coded_vectors));
	if (v == "none")
		return VectorsNone;
	if (v == "static")
		return VectorsStatic;
	return VectorsDefault;
}

void Settings::set_nxwarpCodedVectors(const nxwarp_coded_vectors & value)
{
	const auto old = nxwarpCodedVectors();
	const char * v = value == VectorsNone ? "none" : value == VectorsStatic ? "static"
	                                                                        : "default";
	nxd::set_nxwarp_option_or_default(m_jsonSettings, "coded-vectors", v, nxd::nxwarp_default_coded_vectors);
	if (old != nxwarpCodedVectors())
		nxwarpCodedVectorsChanged();
}

// "effort" is 0 or 1 on the server and a checkbox here: there is no level 2 --
// nxvc refuses one, and the measurements that say why are in its
// vk/encoder/README.md -- so a two-valued knob is the whole of it rather than a
// simplification of a range.
bool Settings::nxwarpEffort() const
{
	return nxd::nxwarp_option_u32(m_jsonSettings, "effort", nxd::nxwarp_default_effort) >= 1;
}

void Settings::set_nxwarpEffort(const bool & value)
{
	const auto old = nxwarpEffort();
	nxd::set_nxwarp_option_u32(m_jsonSettings, "effort", value ? 1u : 0u, nxd::nxwarp_default_effort);
	if (old != nxwarpEffort())
		nxwarpEffortChanged();
}

// The four settings and the thresholds they mean.  An out-of-range value in a
// hand-edited file reads as the nearest lower setting rather than being
// silently dropped: the file is the authority on what the server will do, and
// the dashboard's job is to show it honestly.
Settings::nxwarp_snap Settings::nxwarpSnapIdentity() const
{
	const uint32_t v = nxd::nxwarp_option_u32(m_jsonSettings, "snap-identity",
	                                          nxd::nxwarp_default_snap_identity);
	if (v >= 32)
		return SnapTwo;
	if (v >= 24)
		return SnapOneAndHalf;
	if (v >= 16)
		return SnapOneSample;
	return SnapOff;
}

void Settings::set_nxwarpSnapIdentity(const nxwarp_snap & value)
{
	const auto old = nxwarpSnapIdentity();
	const uint32_t v = value == SnapTwo ? 32u
	                                    : (value == SnapOneAndHalf
	                                               ? 24u
	                                               : (value == SnapOneSample ? 16u : 0u));
	nxd::set_nxwarp_option_u32(m_jsonSettings, "snap-identity", v,
	                           nxd::nxwarp_default_snap_identity);
	if (old != nxwarpSnapIdentity())
		nxwarpSnapIdentityChanged();
}

bool Settings::nxwarpInter() const
{
	return nxd::nxwarp_option_bool(m_jsonSettings, "inter", nxd::nxwarp_default_inter);
}

void Settings::set_nxwarpInter(const bool & value)
{
	const auto old = nxwarpInter();
	nxd::set_nxwarp_option_bool(m_jsonSettings, "inter", value, nxd::nxwarp_default_inter);
	if (old != nxwarpInter())
		nxwarpInterChanged();
}

bool Settings::nxwarpLensMask() const
{
	return nxd::nxwarp_option_bool(m_jsonSettings, "lens-mask", nxd::nxwarp_default_lens_mask);
}

void Settings::set_nxwarpLensMask(const bool & value)
{
	const auto old = nxwarpLensMask();
	nxd::set_nxwarp_option_bool(m_jsonSettings, "lens-mask", value, nxd::nxwarp_default_lens_mask);
	if (old != nxwarpLensMask())
		nxwarpLensMaskChanged();
}

int Settings::nxwarpIntraPeriod() const
{
	return int(nxd::nxwarp_option_u32(m_jsonSettings, "intra-period", nxd::nxwarp_default_intra_period));
}

void Settings::set_nxwarpIntraPeriod(const int & value)
{
	const auto old = nxwarpIntraPeriod();
	nxd::set_nxwarp_option_u32(m_jsonSettings,
	                           "intra-period",
	                           uint32_t(std::clamp(value, 1, 100000)),
	                           nxd::nxwarp_default_intra_period);
	if (old != nxwarpIntraPeriod())
		nxwarpIntraPeriodChanged();
}

void Settings::save(wivrn_server * server)
{
	server->setJsonConfiguration(QString::fromStdString(m_jsonSettings.dump(2)));
	if (m_jsonSettings != m_originalSettings)
		settingsChanged();
}

void Settings::restore_defaults()
{
	m_jsonSettings.erase("encoder");
	m_jsonSettings.erase("application");
	m_jsonSettings.erase("hid-forwarding");
	m_jsonSettings.erase("mirror");
	m_jsonSettings.erase("debug-gui");
	m_jsonSettings.erase("use-steamvr-lh");
	m_jsonSettings.erase("lh-stick-deadzone");
	m_jsonSettings.erase("tcp-only");
	m_jsonSettings.erase("application");
	m_jsonSettings.erase("port");
	m_jsonSettings.erase("hostname");
	// The NX Warp options live inside "encoder", which is erased above, so they go with it.
	// "stream_scale" is top level and has to be named.
	m_jsonSettings.erase("stream_scale");
	// Edge bleed is top level too, and one key covers both of its controls.
	m_jsonSettings.erase("edge_bleed");
	m_jsonSettings.erase("edge-bleed");
	emitAllChanged();
}

bool Settings::flatpak() const
{
	return wivrn::is_flatpak();
}

int Settings::default_port() const
{
	return wivrn::default_port;
}

bool Settings::debug_gui() const
{
#if WIVRN_FEATURE_DEBUG_GUI
	return true;
#else
	return false;
#endif
}
bool Settings::steamvr_lh() const
{
#if WIVRN_FEATURE_STEAMVR_LIGHTHOUSE
	return true;
#else
	return false;
#endif
}
bool Settings::hid_forwarding() const
{
	// only called from the UI thread → no locking needed
	static std::optional<bool> can_open_uinput;
	if (can_open_uinput.has_value())
		return can_open_uinput.value();

	can_open_uinput = false;
	constexpr std::array paths = {"/dev/uinput", "/dev/input/uinput"};
	for (const char * p: paths)
	{
		int fd = ::open(p, O_WRONLY | O_NONBLOCK);
		if (fd >= 0)
		{
			::close(fd);
			can_open_uinput = true;
			break;
		}
	}
	return can_open_uinput.value();
}

#include "moc_settings.cpp"
